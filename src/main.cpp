#include <algorithm>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <filesystem>
#include <fstream>
#include <unistd.h>

#include "cli/args.h"
#include "cli/config.h"
#include "cli/readline.h"
#include "core/agent.h"
#include "core/agent_state.h"
#include "core/builtin_tools.h"
#include "core/env_api_keys.h"
#include "core/event_types.h"
#include "core/lua_tool.h"
#include "core/message_types.h"
#include "core/models.h"
#include "core/otel_init.h"
#include "core/providers/openai_completions.h"
#include "core/stream_renderer.h"

namespace pi {

static void print_version() { std::cout << "pi-cpp " PI_CPP_VERSION "\n"; }

static void print_tools(
    const std::vector<std::shared_ptr<const core::ToolDefinition>> &tools) {
  if (tools.empty()) {
    std::cout << "(no tools loaded)\n";
    return;
  }
  std::size_t wsrc = 7;
  std::size_t wname = 4;
  for (const auto &t : tools) {
    wsrc = std::max(wsrc, t->source_path().size());
    wname = std::max(wname, t->name().size());
  }
  std::cout << std::left << std::setw(static_cast<int>(wsrc + 2)) << "source"
            << std::setw(static_cast<int>(wname + 2)) << "name"
            << "description\n";
  for (const auto &t : tools) {
    std::cout << std::left << std::setw(static_cast<int>(wsrc + 2))
              << t->source_path() << std::setw(static_cast<int>(wname + 2))
              << t->name() << t->description() << "\n";
  }
}

static void
print_addons(const std::vector<std::shared_ptr<core::LuaHooks>> &hooks_list) {
  if (hooks_list.empty()) {
    std::cout << "(no add-ons loaded)\n";
    return;
  }
  for (const auto &h : hooks_list) {
    std::cout << (h->source_path.empty() ? "<composed>" : h->source_path)
              << "\n";
    std::vector<std::string> active;
    if (h->before_tool_call)
      active.emplace_back("before_tool_call");
    if (h->after_tool_call)
      active.emplace_back("after_tool_call");
    if (h->should_stop_after_turn)
      active.emplace_back("should_stop_after_turn");
    if (h->on_command)
      active.emplace_back("on_command");
    if (h->complete)
      active.emplace_back("complete");
    if (!active.empty()) {
      std::cout << "  hooks:";
      for (const auto &a : active)
        std::cout << "  " << a;
      std::cout << "\n";
    }
    if (!h->commands.empty()) {
      for (const auto &cmd : h->commands) {
        std::cout << "  /" << cmd.name;
        if (!cmd.args_hint.empty())
          std::cout << " " << cmd.args_hint;
        if (!cmd.description.empty())
          std::cout << "  — " << cmd.description;
        std::cout << "\n";
      }
    }
  }
}

static std::string format_tokens(std::uint64_t n) {
  std::ostringstream ss;
  if (n >= 1'000'000) {
    double m = static_cast<double>(n) / 1'000'000.0;
    auto whole = static_cast<double>(static_cast<std::uint64_t>(m));
    ss << std::fixed << std::setprecision(m == whole ? 0 : 1) << m << "M";
  } else if (n >= 1'000) {
    ss << (n / 1'000) << "K";
  } else {
    ss << n;
  }
  return ss.str();
}

static int cmd_list_models(const cli::Args &args) {
  auto hits = pi::core::search_models(args.list_models_filter);
  if (hits.empty()) {
    if (!args.list_models_filter.empty())
      std::cout << "No models matching \"" << args.list_models_filter << "\"\n";
    else
      std::cout << "No models available.\n";
    return 0;
  }

  // Column widths
  std::size_t wprov = 8;
  std::size_t wid = 5;
  std::size_t wctx = 7;
  std::size_t wmax = 7;
  for (const auto *m : hits) {
    wprov = std::max(wprov, m->provider.size());
    wid = std::max(wid, m->id.size());
    wctx = std::max(wctx, format_tokens(m->context_window).size());
    wmax = std::max(wmax, format_tokens(m->max_tokens).size());
  }

  auto row = [&](std::string_view prov, std::string_view id,
                 std::string_view ctx, std::string_view mx,
                 std::string_view reason, std::string_view img) {
    std::cout << std::left << std::setw(static_cast<int>(wprov + 2)) << prov
              << std::setw(static_cast<int>(wid + 2)) << id
              << std::setw(static_cast<int>(wctx + 2)) << ctx
              << std::setw(static_cast<int>(wmax + 2)) << mx << std::setw(10)
              << reason << img << "\n";
  };

  row("provider", "model", "context", "max-out", "thinking", "images");

  for (const auto *m : hits) {
    bool has_image = false;
    for (const auto &cap : m->input_capabilities)
      if (cap == "image") {
        has_image = true;
        break;
      }

    row(m->provider, m->id, format_tokens(m->context_window),
        format_tokens(m->max_tokens), m->reasoning ? "yes" : "no",
        has_image ? "yes" : "no");
  }
  return 0;
}

static core::ThinkingLevel to_core_thinking(cli::ThinkingLevel t) {
  switch (t) {
  case cli::ThinkingLevel::off:
    return core::ThinkingLevel::off;
  case cli::ThinkingLevel::minimal:
    return core::ThinkingLevel::minimal;
  case cli::ThinkingLevel::low:
    return core::ThinkingLevel::low;
  case cli::ThinkingLevel::medium:
    return core::ThinkingLevel::medium;
  case cli::ThinkingLevel::high:
    return core::ThinkingLevel::high;
  case cli::ThinkingLevel::xhigh:
    return core::ThinkingLevel::xhigh;
  }
  return core::ThinkingLevel::off;
}

static std::optional<core::Model> resolve_model(const cli::Args &args) {
  if (!args.model.empty()) {
    auto m = core::find_model(args.model, args.provider);
    if (m && !args.base_url.empty())
      m->base_url = args.base_url;
    if (m && !args.provider.empty() && m->provider == "custom")
      m->provider = args.provider;
    return m;
  }
  // No --model given: build a model from explicit flags or use a sensible
  // default
  core::Model m;
  m.api = "openai-completions";
  m.provider = args.provider.empty() ? "local" : args.provider;
  m.base_url =
      args.base_url.empty() ? "http://127.0.0.1:8080/v1" : args.base_url;
  m.id = "default";
  m.name = "default";
  m.context_window = 128000;
  m.max_tokens = 4096;
  return m;
}

static std::unique_ptr<core::Renderer> make_renderer(const cli::Args &args) {
  if (!args.render.empty()) {
    auto r = core::StreamRendererRegistry::instance().make(args.render,
                                                           STDOUT_FILENO);
    if (r)
      return r;
    // Unknown name — warn and fall through to auto
    std::cerr << "warning: unknown renderer \"" << args.render
              << "\", using auto\n";
  }
  return core::make_auto_renderer(STDOUT_FILENO);
}

// A renderer adapter that adds verbose tool/usage output on top of any base
// renderer.
class VerboseRenderer final : public core::Renderer {
public:
  VerboseRenderer(core::Renderer &base, bool verbose)
      : base_(base), verbose_(verbose) {}

  void on_turn_start() override { base_.on_turn_start(); }
  void on_text_delta(std::string_view d) override { base_.on_text_delta(d); }
  void on_thinking_start() override { base_.on_thinking_start(); }
  void on_thinking_delta(std::string_view d) override {
    base_.on_thinking_delta(d);
  }
  void on_thinking_end() override { base_.on_thinking_end(); }

  void on_tool_start(std::string_view, std::string_view name,
                     std::string_view) override {
    std::cout << "\n[tool: " << name << "]\n" << std::flush;
  }
  void on_tool_end(std::string_view, std::string_view,
                   const core::ToolResult &result, bool) override {
    if (verbose_)
      std::cout << result.content() << "\n" << std::flush;
  }

  void on_message_end(const core::TokenUsage &u) override {
    base_.on_message_end(u);
    if (verbose_)
      std::cerr << "[usage: in=" << u.input << " out=" << u.output << "]\n";
    last_usage_ = u;
  }

  void on_turn_end() override { base_.on_turn_end(); }

  void on_error(core::RendererErrorKind, std::string_view msg) override {
    std::cerr << "\nerror: " << msg << "\n";
  }

  void on_scroll(core::RendererScrollCommand command) override {
    base_.on_scroll(command);
  }

  const core::TokenUsage &last_usage() const { return last_usage_; }

private:
  core::Renderer &base_;
  bool verbose_;
  core::TokenUsage last_usage_;
};

static core::TokenUsage run_turn(core::Agent &agent, const std::string &input,
                                 core::Renderer &renderer, bool verbose) {
  VerboseRenderer vr(renderer, verbose);
  for (const auto &ev : agent.prompt(input))
    core::dispatch_event(ev, vr);
  return vr.last_usage();
}

static void print_usage(const core::TokenUsage &last,
                        const core::TokenUsage &session, std::size_t turns) {
  auto row = [](std::string_view label, std::uint64_t in, std::uint64_t out,
                std::uint64_t total) {
    std::cout << std::left << std::setw(10) << label << "  in=" << std::setw(8)
              << in << "  out=" << std::setw(8) << out << "  total=" << total
              << "\n";
  };
  std::cout << "\n";
  row("last turn:", last.input, last.output, last.total_tokens);
  row("session:", session.input, session.output, session.total_tokens);
  std::cout << "  turns: " << turns << "\n";
}

struct ContextFile {
  std::string path;
  std::string content;
};

static std::vector<ContextFile> load_context_files() {
  namespace fs = std::filesystem;
  static constexpr std::string_view kCandidates[] = {"AGENTS.md", "AGENTS.MD",
                                                     "CLAUDE.md", "CLAUDE.MD"};

  auto try_load = [&](const fs::path &dir) -> std::optional<ContextFile> {
    for (auto name : kCandidates) {
      fs::path p = dir / name;
      std::error_code ec;
      if (!fs::exists(p, ec))
        continue;
      std::ifstream f(p);
      if (!f)
        continue;
      std::string content((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
      return ContextFile{.path = p.string(), .content = std::move(content)};
    }
    return std::nullopt;
  };

  std::vector<ContextFile> result;
  std::set<std::string> seen;

  // Walk up from cwd to root, collecting innermost-first then reversing
  std::vector<ContextFile> ancestors;
  fs::path cur = fs::current_path();
  while (true) {
    if (auto cf = try_load(cur)) {
      if (seen.insert(cf->path).second)
        ancestors.push_back(std::move(*cf));
    }
    fs::path parent = cur.parent_path();
    if (parent == cur)
      break;
    cur = parent;
  }
  // Outermost first so inner files override/append last
  std::ranges::reverse(ancestors, );
  result.insert(result.end(), ancestors.begin(), ancestors.end());
  return result;
}

static int cmd_run(const cli::Args &args) {
  auto model_opt = resolve_model(args);
  if (!model_opt) {
    std::cerr << "error: could not resolve model\n";
    return 1;
  }
  core::Model model = *model_opt;

  // Build system prompt
  std::string system = args.system_prompt;
  for (const auto &extra : args.append_system_prompts) {
    if (!system.empty())
      system += "\n\n";
    system += extra;
  }

  // Inject AGENTS.md / CLAUDE.md context files
  if (!args.no_context_files) {
    auto ctx_files = load_context_files();
    if (!ctx_files.empty()) {
      system += "\n\n# Project Context\n\n";
      for (const auto &cf : ctx_files) {
        system += "## " + cf.path + "\n\n" + cf.content + "\n\n";
        if (args.verbose)
          std::cerr << "[context: " << cf.path << "]\n";
      }
    }
  }

  core::Agent::Options opts;
  opts.model = model;
  opts.system_prompt = system;
  opts.thinking_level = to_core_thinking(args.thinking);
  opts.get_api_key =
      [&args, &model](std::string_view p) -> std::optional<std::string> {
    if (!args.api_key.empty())
      return args.api_key;
    return core::get_env_api_key(p.empty() ? model.provider : p);
  };
  opts.should_stop_after_turn = nullptr;

  // Load and compose Lua hooks
  std::vector<std::shared_ptr<core::LuaHooks>> hooks_list;
  for (const auto &path : args.hooks_files) {
    try {
      hooks_list.push_back(core::load_lua_hooks(path));
      if (args.verbose)
        std::cerr << "[hooks: " << path << "]\n";
    } catch (const std::exception &e) {
      std::cerr << "warning: failed to load hooks file " << path << ": "
                << e.what() << "\n";
    }
  }
  if (!args.hooks_dir.empty()) {
    auto dir_hooks = core::load_lua_hooks_dir(args.hooks_dir);
    if (dir_hooks) {
      hooks_list.push_back(dir_hooks);
      if (args.verbose)
        std::cerr << "[hooks-dir: " << args.hooks_dir << "]\n";
    }
  }
  // Keep a copy for --list-addons / /addons before moving into compose
  auto hooks_list_saved = hooks_list;
  auto hooks = core::compose_hooks(std::move(hooks_list));
  if (hooks) {
    if (hooks->before_tool_call)
      opts.before_tool_call = hooks->before_tool_call;
    if (hooks->after_tool_call)
      opts.after_tool_call = hooks->after_tool_call;
    if (hooks->should_stop_after_turn)
      opts.should_stop_after_turn = hooks->should_stop_after_turn;
  }

  core::Agent agent(opts);

  if (!args.no_tools && !args.no_builtin_tools) {
    if (args.tools.empty()) {
      agent.set_tools(core::create_all_tools());
    } else {
      // Allowlist filter
      for (auto &t : core::create_all_tools()) {
        for (const auto &name : args.tools) {
          if (t->name() == name) {
            agent.add_tool(t);
            break;
          }
        }
      }
    }
  }

  if (!args.no_tools && !args.tools_dir.empty()) {
    for (auto &t : core::load_lua_tools(args.tools_dir)) {
      if (args.tools.empty()) {
        agent.add_tool(t);
      } else {
        for (const auto &name : args.tools)
          if (t->name() == name) {
            agent.add_tool(t);
            break;
          }
      }
    }
  }

  // --list-tools / --list-addons (exit immediately after printing)
  if (args.list_tools) {
    print_tools(agent.state().tools());
    return 0;
  }
  if (args.list_addons) {
    print_addons(hooks_list_saved);
    return 0;
  }

  // Tools registered via pici.add_tool() in hooks files
  if (hooks) {
    for (const auto &t : hooks->registered_tools)
      agent.add_tool(t);
  }

  // Configure pici.* globals now that agent + tools exist
  if (hooks && hooks->configure) {
    // Build tool name list
    std::vector<std::string> tool_names;
    for (const auto &t : agent.state().tools())
      tool_names.emplace_back(t->name());

    // Storage path: first explicit hooks file name + ".storage.json"
    std::filesystem::path storage_path;
    if (!args.hooks_files.empty())
      storage_path =
          std::filesystem::path(args.hooks_files[0]).string() + ".storage.json";

    core::LuaHooks::AgentInfo info;
    info.model_id = model.id;
    info.model_provider = model.provider;
    info.model_api = model.api;
    info.tool_names = std::move(tool_names);
    info.cwd = std::filesystem::current_path().string();
    info.storage_path = std::move(storage_path);
    info.run_agent = [&agent, &opts](const core::LuaHooks::AgentRunConfig &cfg)
        -> core::LuaHooks::AgentRunResult {
      core::Agent::Options sub_opts = opts;
      sub_opts.before_tool_call = nullptr;
      sub_opts.after_tool_call = nullptr;
      sub_opts.should_stop_after_turn = nullptr;
      if (cfg.system_prompt)
        sub_opts.system_prompt = *cfg.system_prompt;
      if (cfg.model_id)
        sub_opts.model.id = *cfg.model_id;

      core::Agent sub(sub_opts);

      if (cfg.fork_at > 0) {
        auto msgs = agent.state().messages();
        msgs.resize(std::min(cfg.fork_at, msgs.size()));
        sub.state().set_messages(std::move(msgs));
      }

      if (cfg.tools.empty()) {
        sub.set_tools(agent.state().tools());
      } else {
        for (const auto &t : agent.state().tools())
          for (const auto &name : cfg.tools)
            if (t->name() == name) {
              sub.add_tool(t);
              break;
            }
      }

      core::LuaHooks::AgentRunResult result;
      try {
        auto stream = sub.prompt(cfg.prompt);
        for (const auto &ev : stream) {
          std::visit(
              [&result](const auto &e) {
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, core::MessageUpdateEvent>) {
                  std::visit(
                      [&result](const auto &ae) {
                        using AE = std::decay_t<decltype(ae)>;
                        if constexpr (std::is_same_v<
                                          AE,
                                          core::AssistantMessageTextDeltaEvent>)
                          result.text += ae.delta;
                      },
                      e.assistant_message_event);
                } else if constexpr (std::is_same_v<T, core::MessageEndEvent>) {
                  if (const auto *am =
                          std::get_if<core::AssistantMessage>(&e.message))
                    if (am->error_message)
                      result.error = *am->error_message;
                }
              },
              ev);
        }
      } catch (const std::exception &e) {
        result.error = e.what();
      }
      return result;
    };
    hooks->configure(info);
  }

  auto renderer = make_renderer(args);

  if (args.verbose) {
    std::cerr << "[model: " << model.provider << "/" << model.id << "]\n";
    std::cerr << "[tools: " << agent.state().tools().size() << "]\n";
  }

  // Print mode: run the provided messages and exit
  if (args.print_mode || !args.messages.empty()) {
    std::string prompt;
    for (const auto &msg : args.messages) {
      if (!prompt.empty())
        prompt += '\n';
      prompt += msg;
    }
    if (!prompt.empty()) {
      run_turn(agent, prompt, *renderer, args.verbose);
      std::cout << "\n";
    }
    if (args.print_mode)
      return 0;
  }

  // Build completion function.
  // Command-name completion (/... with no space) is handled here from the
  // declared commands list — no Lua needed.  Argument completion (/cmd ...
  // with a space) is delegated to hooks->complete.
  cli::CompleteFn complete_fn =
      [&hooks, &agent](std::string_view partial) -> std::vector<std::string> {
    std::vector<std::string> result;
    const bool is_slash = !partial.empty() && partial[0] == '/';
    const bool has_space = partial.contains(' ');

    if (is_slash && !has_space) {
      // Complete command names: builtins + declared add-on commands
      for (std::string_view b :
           {std::string_view("/exit"), std::string_view("/quit"),
            std::string_view("/tools"), std::string_view("/addons"),
            std::string_view("/usage")}) {
        if (b.starts_with(partial))
          result.emplace_back(b);
      }
      if (hooks) {
        for (const auto &cmd : hooks->commands) {
          std::string full = '/' + cmd.name;
          if (std::string_view(full).starts_with(partial))
            result.push_back(std::move(full));
        }
      }
      return result;
    }

    // Argument completion — delegate to hook
    if (hooks && hooks->complete)
      return hooks->complete(partial, agent.state().messages());
    return {};
  };

  cli::ControlFn control_fn = [&renderer](cli::ControlAction action) {
    switch (action) {
    case cli::ControlAction::scroll_line_up:
      renderer->on_scroll(core::RendererScrollCommand::line_up);
      break;
    case cli::ControlAction::scroll_line_down:
      renderer->on_scroll(core::RendererScrollCommand::line_down);
      break;
    case cli::ControlAction::scroll_page_up:
      renderer->on_scroll(core::RendererScrollCommand::page_up);
      break;
    case cli::ControlAction::scroll_page_down:
      renderer->on_scroll(core::RendererScrollCommand::page_down);
      break;
    case cli::ControlAction::scroll_top:
      renderer->on_scroll(core::RendererScrollCommand::top);
      break;
    case cli::ControlAction::scroll_bottom:
      renderer->on_scroll(core::RendererScrollCommand::bottom);
      break;
    }
  };

  // Interactive REPL — track usage across turns
  core::TokenUsage last_usage;
  core::TokenUsage session_usage;
  std::size_t session_turns = 0;

  auto accumulate = [&](const core::TokenUsage &u) {
    last_usage = u;
    session_usage.input += u.input;
    session_usage.output += u.output;
    session_usage.cache_read += u.cache_read;
    session_usage.cache_write += u.cache_write;
    session_usage.total_tokens += u.total_tokens;
    ++session_turns;
  };

  while (true) {
    // Build the prompt — let add-ons customise it
    std::string prompt = "\n> ";
    if (hooks && hooks->prompt_line) {
      const auto &msgs = agent.state().messages();
      std::size_t turns = 0;
      for (const auto &m : msgs)
        if (std::holds_alternative<core::AssistantMessage>(m))
          ++turns;
      auto custom =
          hooks->prompt_line(turns, model.id, agent.state().tools().size());
      if (custom)
        prompt = "\n" + *custom;
    }
    auto maybe_line = cli::readline(prompt, complete_fn, control_fn);
    if (!maybe_line)
      break;
    const std::string &line = *maybe_line;
    if (line.empty())
      continue;
    if (line == "/exit" || line == "/quit")
      break;
    if (line == "/tools") {
      print_tools(agent.state().tools());
      continue;
    }
    if (line == "/addons") {
      print_addons(hooks_list_saved);
      continue;
    }
    if (line == "/usage") {
      print_usage(last_usage, session_usage, session_turns);
      continue;
    }

    // Slash command dispatch
    if (line[0] == '/' && hooks && hooks->on_command) {
      auto space = line.find(' ');
      std::string cmd = line.substr(
          1, space == std::string::npos ? std::string::npos : space - 1);
      std::string rest =
          space == std::string::npos ? "" : line.substr(space + 1);

      auto result = hooks->on_command(cmd, rest, agent.state().messages());
      if (result.handled) {
        if (result.truncate_to) {
          auto msgs = agent.state().messages();
          msgs.resize(std::min(*result.truncate_to, msgs.size()));
          agent.state().set_messages(std::move(msgs));
        }
        if (result.prompt)
          accumulate(run_turn(agent, *result.prompt, *renderer, args.verbose));
        continue;
      }
    }

    accumulate(run_turn(agent, line, *renderer, args.verbose));
  }
  return 0;
}

} // namespace pi

int main(int argc, char *argv[]) {
  std::signal(SIGINT, [](int) { std::exit(0); });
  std::signal(SIGTERM, [](int) { std::exit(0); });

  pi::core::register_openai_completions_client();

  auto args = pi::cli::load_and_merge(argc, argv);

  // Initialise OTel export if requested.  The RAII guard + atexit ensure
  // BatchSpanProcessor is flushed before process exit (including Ctrl-C via
  // the signal handler above which calls std::exit).
  struct OtelGuard {
    ~OtelGuard() { pi::core::shutdown_otel(); }
  } otel_guard;
  std::atexit([] { pi::core::shutdown_otel(); });
  if (!args.otel_endpoint.empty())
    pi::core::init_otel(args.otel_endpoint);

  for (const auto &d : args.diagnostics) {
    auto &out = d.is_error ? std::cerr : std::cout;
    out << (d.is_error ? "error: " : "warning: ") << d.message << "\n";
    if (d.is_error)
      return 1;
  }

  if (args.help) {
    pi::cli::print_help(argv[0]);
    return 0;
  }
  if (args.version) {
    pi::print_version();
    return 0;
  }
  if (args.list_models) {
    return pi::cmd_list_models(args);
  }

  if (!args.test_files.empty()) {
    int total_passed = 0;
    int total_failed = 0;
    for (const auto &f : args.test_files) {
      std::cout << "=== " << f << " ===\n";
      try {
        auto r = pi::core::run_lua_test_file(f);
        total_passed += r.passed;
        total_failed += r.failed;
        std::cout << r.passed << "/" << r.total << " passed";
        if (r.failed > 0)
          std::cout << ", " << r.failed << " failed";
        std::cout << "\n\n";
      } catch (const std::exception &e) {
        std::cerr << "error: " << e.what() << "\n\n";
        ++total_failed;
      }
    }
    if (total_failed == 0)
      std::cout << "All tests passed.\n";
    else
      std::cout << total_failed << " test(s) failed.\n";
    return total_failed > 0 ? 1 : 0;
  }

  return pi::cmd_run(args);
}
