#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include <filesystem>
#include <fstream>
#include <unistd.h>

#include "cli/args.h"
#include "cli/config.h"
#include "core/agent.h"
#include "core/agent_state.h"
#include "core/builtin_tools.h"
#include "core/env_api_keys.h"
#include "core/event_types.h"
#include "core/lua_tool.h"
#include "core/message_types.h"
#include "core/models.h"
#include "cli/readline.h"
#include "core/providers/openai_completions.h"
#include "core/stream_renderer.h"

namespace pi {

static void print_version() {
  std::cout << "pi-cpp " PI_CPP_VERSION "\n";
}

static void print_tools(
    const std::vector<std::shared_ptr<const core::ToolDefinition>> &tools) {
  if (tools.empty()) { std::cout << "(no tools loaded)\n"; return; }
  std::size_t wsrc = 7, wname = 4;
  for (const auto &t : tools) {
    wsrc  = std::max(wsrc,  t->source_path().size());
    wname = std::max(wname, t->name().size());
  }
  std::cout << std::left
            << std::setw(static_cast<int>(wsrc + 2))  << "source"
            << std::setw(static_cast<int>(wname + 2)) << "name"
            << "description\n";
  for (const auto &t : tools) {
    std::cout << std::left
              << std::setw(static_cast<int>(wsrc + 2))  << t->source_path()
              << std::setw(static_cast<int>(wname + 2)) << t->name()
              << t->description() << "\n";
  }
}

static void print_addons(
    const std::vector<std::shared_ptr<core::LuaHooks>> &hooks_list) {
  if (hooks_list.empty()) { std::cout << "(no add-ons loaded)\n"; return; }
  for (const auto &h : hooks_list) {
    std::cout << (h->source_path.empty() ? "<composed>" : h->source_path) << "\n";
    std::vector<std::string> active;
    if (h->before_tool_call)       active.push_back("before_tool_call");
    if (h->after_tool_call)        active.push_back("after_tool_call");
    if (h->should_stop_after_turn) active.push_back("should_stop_after_turn");
    if (h->on_command)             active.push_back("on_command");
    if (h->complete)               active.push_back("complete");
    if (!active.empty()) {
      std::cout << "  hooks:";
      for (const auto &a : active) std::cout << "  " << a;
      std::cout << "\n";
    }
    if (!h->commands.empty()) {
      for (const auto &cmd : h->commands) {
        std::cout << "  /" << cmd.name;
        if (!cmd.args_hint.empty()) std::cout << " " << cmd.args_hint;
        if (!cmd.description.empty()) std::cout << "  — " << cmd.description;
        std::cout << "\n";
      }
    }
  }
}

static std::string format_tokens(std::uint64_t n) {
  std::ostringstream ss;
  if (n >= 1'000'000) {
    double m = static_cast<double>(n) / 1'000'000.0;
    double whole = static_cast<double>(static_cast<std::uint64_t>(m));
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
  std::size_t wprov = 8, wid = 5, wctx = 7, wmax = 7;
  for (const auto *m : hits) {
    wprov = std::max(wprov, m->provider.size());
    wid   = std::max(wid,   m->id.size());
    wctx  = std::max(wctx,  format_tokens(m->context_window).size());
    wmax  = std::max(wmax,  format_tokens(m->max_tokens).size());
  }

  auto row = [&](std::string_view prov, std::string_view id,
                 std::string_view ctx, std::string_view mx,
                 std::string_view reason, std::string_view img) {
    std::cout << std::left
              << std::setw(static_cast<int>(wprov + 2)) << prov
              << std::setw(static_cast<int>(wid   + 2)) << id
              << std::setw(static_cast<int>(wctx  + 2)) << ctx
              << std::setw(static_cast<int>(wmax  + 2)) << mx
              << std::setw(10) << reason
              << img << "\n";
  };

  row("provider", "model", "context", "max-out", "thinking", "images");

  for (const auto *m : hits) {
    bool has_image = false;
    for (const auto &cap : m->input_capabilities)
      if (cap == "image") { has_image = true; break; }

    row(m->provider, m->id,
        format_tokens(m->context_window),
        format_tokens(m->max_tokens),
        m->reasoning ? "yes" : "no",
        has_image    ? "yes" : "no");
  }
  return 0;
}

static core::ThinkingLevel to_core_thinking(cli::ThinkingLevel t) {
  switch (t) {
  case cli::ThinkingLevel::off:     return core::ThinkingLevel::off;
  case cli::ThinkingLevel::minimal: return core::ThinkingLevel::minimal;
  case cli::ThinkingLevel::low:     return core::ThinkingLevel::low;
  case cli::ThinkingLevel::medium:  return core::ThinkingLevel::medium;
  case cli::ThinkingLevel::high:    return core::ThinkingLevel::high;
  case cli::ThinkingLevel::xhigh:   return core::ThinkingLevel::xhigh;
  }
  return core::ThinkingLevel::off;
}

static std::optional<core::Model> resolve_model(const cli::Args &args) {
  if (!args.model.empty()) {
    auto m = core::find_model(args.model, args.provider);
    if (m && !args.base_url.empty()) m->base_url = args.base_url;
    if (m && !args.provider.empty() && m->provider == "custom") m->provider = args.provider;
    return m;
  }
  // No --model given: build a model from explicit flags or use a sensible default
  core::Model m;
  m.api      = "openai-completions";
  m.provider = args.provider.empty() ? "local" : args.provider;
  m.base_url = args.base_url.empty() ? "http://127.0.0.1:8080/v1" : args.base_url;
  m.id       = "default";
  m.name     = "default";
  m.context_window = 128000;
  m.max_tokens     = 4096;
  return m;
}

static std::unique_ptr<core::StreamRenderer> make_renderer(const cli::Args &args) {
  if (args.render == "markdown") return core::make_diff_renderer(STDOUT_FILENO);
  if (args.render == "raw")      return core::make_raw_renderer(STDOUT_FILENO);
  return core::make_auto_renderer(STDOUT_FILENO);
}

static void run_turn(core::Agent &agent, const std::string &input,
                     core::StreamRenderer &renderer, bool verbose) {
  renderer.reset();
  auto stream = agent.prompt(input);
  bool done = false;
  for (const auto &ev : stream) {
    if (done) break;
    std::visit(
        [&](const auto &e) {
          using T = std::decay_t<decltype(e)>;
          if constexpr (std::is_same_v<T, core::MessageUpdateEvent>) {
            std::visit(
                [&](const auto &ae) {
                  using AE = std::decay_t<decltype(ae)>;
                  if constexpr (std::is_same_v<AE, core::AssistantMessageTextDeltaEvent>) {
                    renderer.update(ae.delta);
                  } else if constexpr (std::is_same_v<AE, core::AssistantMessageErrorEvent>) {
                    std::cerr << "\nerror: "
                              << ae.error.error_message.value_or("LLM request failed") << "\n";
                  }
                },
                e.assistant_message_event);
          } else if constexpr (std::is_same_v<T, core::MessageEndEvent>) {
            if (const auto *am = std::get_if<core::AssistantMessage>(&e.message)) {
              if (am->error_message) {
                std::cerr << "\nerror: " << *am->error_message << "\n";
              }
              if (verbose) {
                std::cerr << "[usage: in=" << am->usage.input
                          << " out=" << am->usage.output << "]\n";
              }
            }
            renderer.finish();
          } else if constexpr (std::is_same_v<T, core::ToolExecutionStartEvent>) {
            std::cout << "\n[tool: " << e.tool_name << "]\n" << std::flush;
          } else if constexpr (std::is_same_v<T, core::ToolExecutionEndEvent>) {
            if (e.result && verbose) {
              std::cout << e.result->content() << "\n" << std::flush;
            }
          } else if constexpr (std::is_same_v<T, core::AgentEndEvent>) {
            done = true;
          }
        },
        ev);
  }
}

struct ContextFile {
  std::string path;
  std::string content;
};

static std::vector<ContextFile> load_context_files() {
  namespace fs = std::filesystem;
  static constexpr std::string_view kCandidates[] = {
      "AGENTS.md", "AGENTS.MD", "CLAUDE.md", "CLAUDE.MD"};

  auto try_load = [&](const fs::path &dir) -> std::optional<ContextFile> {
    for (auto name : kCandidates) {
      fs::path p = dir / name;
      std::error_code ec;
      if (!fs::exists(p, ec)) continue;
      std::ifstream f(p);
      if (!f) continue;
      std::string content((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
      return ContextFile{p.string(), std::move(content)};
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
    if (parent == cur) break;
    cur = parent;
  }
  // Outermost first so inner files override/append last
  std::reverse(ancestors.begin(), ancestors.end());
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
    if (!system.empty()) system += "\n\n";
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
  opts.get_api_key = [&args, &model](std::string_view p) -> std::optional<std::string> {
    if (!args.api_key.empty()) return args.api_key;
    return core::get_env_api_key(p.empty() ? model.provider : p);
  };
  opts.should_stop_after_turn = nullptr;

  // Load and compose Lua hooks
  std::vector<std::shared_ptr<core::LuaHooks>> hooks_list;
  for (const auto &path : args.hooks_files) {
    try {
      hooks_list.push_back(core::load_lua_hooks(path));
      if (args.verbose) std::cerr << "[hooks: " << path << "]\n";
    } catch (const std::exception &e) {
      std::cerr << "warning: failed to load hooks file " << path
                << ": " << e.what() << "\n";
    }
  }
  if (!args.hooks_dir.empty()) {
    auto dir_hooks = core::load_lua_hooks_dir(args.hooks_dir);
    if (dir_hooks) {
      hooks_list.push_back(dir_hooks);
      if (args.verbose) std::cerr << "[hooks-dir: " << args.hooks_dir << "]\n";
    }
  }
  // Keep a copy for --list-addons / /addons before moving into compose
  auto hooks_list_saved = hooks_list;
  auto hooks = core::compose_hooks(std::move(hooks_list));
  if (hooks) {
    if (hooks->before_tool_call)       opts.before_tool_call       = hooks->before_tool_call;
    if (hooks->after_tool_call)        opts.after_tool_call        = hooks->after_tool_call;
    if (hooks->should_stop_after_turn) opts.should_stop_after_turn = hooks->should_stop_after_turn;
  }

  core::Agent agent(opts);

  if (!args.no_tools && !args.no_builtin_tools) {
    if (args.tools.empty()) {
      agent.set_tools(core::create_all_tools());
    } else {
      // Allowlist filter
      for (auto &t : core::create_all_tools()) {
        for (const auto &name : args.tools) {
          if (t->name() == name) { agent.add_tool(t); break; }
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
          if (t->name() == name) { agent.add_tool(t); break; }
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

  // Configure pici.* globals now that agent + tools exist
  if (hooks && hooks->configure) {
    // Build tool name list
    std::vector<std::string> tool_names;
    for (const auto &t : agent.state().tools())
      tool_names.push_back(std::string(t->name()));

    // Storage path: first explicit hooks file name + ".storage.json"
    std::filesystem::path storage_path;
    if (!args.hooks_files.empty())
      storage_path = std::filesystem::path(args.hooks_files[0]).string() + ".storage.json";

    core::LuaHooks::AgentInfo info;
    info.model_id       = model.id;
    info.model_provider = model.provider;
    info.model_api      = model.api;
    info.tool_names     = std::move(tool_names);
    info.cwd            = std::filesystem::current_path().string();
    info.storage_path   = std::move(storage_path);
    info.run_agent      =
        [&agent, &opts](const core::LuaHooks::AgentRunConfig &cfg)
            -> core::LuaHooks::AgentRunResult {
          core::Agent::Options sub_opts = opts;
          sub_opts.before_tool_call       = nullptr;
          sub_opts.after_tool_call        = nullptr;
          sub_opts.should_stop_after_turn = nullptr;
          if (cfg.system_prompt) sub_opts.system_prompt = *cfg.system_prompt;
          if (cfg.model_id)      sub_opts.model.id      = *cfg.model_id;

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
                if (t->name() == name) { sub.add_tool(t); break; }
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
                                              AE, core::AssistantMessageTextDeltaEvent>)
                              result.text += ae.delta;
                          },
                          e.assistant_message_event);
                    } else if constexpr (std::is_same_v<T, core::MessageEndEvent>) {
                      if (const auto *am =
                              std::get_if<core::AssistantMessage>(&e.message))
                        if (am->error_message) result.error = *am->error_message;
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
      if (!prompt.empty()) prompt += "\n";
      prompt += msg;
    }
    if (!prompt.empty()) {
      run_turn(agent, prompt, *renderer, args.verbose);
      std::cout << "\n";
    }
    if (args.print_mode) return 0;
  }

  // Build completion function.
  // Command-name completion (/... with no space) is handled here from the
  // declared commands list — no Lua needed.  Argument completion (/cmd ...
  // with a space) is delegated to hooks->complete.
  cli::CompleteFn complete_fn = [&hooks, &agent](std::string_view partial)
      -> std::vector<std::string> {
    std::vector<std::string> result;
    const bool is_slash = !partial.empty() && partial[0] == '/';
    const bool has_space = partial.find(' ') != std::string_view::npos;

    if (is_slash && !has_space) {
      // Complete command names: builtins + declared add-on commands
      for (std::string_view b : {std::string_view("/exit"), std::string_view("/quit"),
                                  std::string_view("/tools"), std::string_view("/addons")}) {
        if (b.substr(0, partial.size()) == partial)
          result.emplace_back(b);
      }
      if (hooks) {
        for (const auto &cmd : hooks->commands) {
          std::string full = '/' + cmd.name;
          if (std::string_view(full).substr(0, partial.size()) == partial)
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

  // Interactive REPL
  while (true) {
    // Build the prompt — let add-ons customise it
    std::string prompt = "\n> ";
    if (hooks && hooks->prompt_line) {
      const auto &msgs = agent.state().messages();
      std::size_t turns = 0;
      for (const auto &m : msgs)
        if (std::holds_alternative<core::AssistantMessage>(m)) ++turns;
      auto tools_cnt  = agent.state().tools().size();
      auto custom     = hooks->prompt_line(turns, model.id, tools_cnt);
      if (custom) prompt = "\n" + *custom;
    }
    auto maybe_line = cli::readline(prompt, complete_fn);
    if (!maybe_line) break;
    const std::string &line = *maybe_line;
    if (line.empty()) continue;
    if (line == "/exit" || line == "/quit") break;
    if (line == "/tools")  { print_tools(agent.state().tools()); continue; }
    if (line == "/addons") { print_addons(hooks_list_saved); continue; }

    // Slash command dispatch
    if (line[0] == '/' && hooks && hooks->on_command) {
      // Split "/cmd rest" → cmd="cmd", rest_args="rest"
      auto space = line.find(' ');
      std::string cmd  = line.substr(1, space == std::string::npos ? std::string::npos : space - 1);
      std::string rest = space == std::string::npos ? "" : line.substr(space + 1);

      auto result = hooks->on_command(cmd, rest, agent.state().messages());

      if (result.handled) {
        if (result.truncate_to) {
          auto msgs = agent.state().messages();
          auto n = std::min(*result.truncate_to, msgs.size());
          msgs.resize(n);
          agent.state().set_messages(std::move(msgs));
        }
        if (result.prompt) {
          run_turn(agent, *result.prompt, *renderer, args.verbose);
        }
        continue;
      }
      // not handled — fall through and send to agent as text
    }

    run_turn(agent, line, *renderer, args.verbose);
  }
  return 0;
}

} // namespace pi

int main(int argc, char *argv[]) {
  std::signal(SIGINT, [](int) { std::exit(0); });
  std::signal(SIGTERM, [](int) { std::exit(0); });

  pi::core::register_openai_completions_client();

  auto args = pi::cli::load_and_merge(argc, argv);

  for (const auto &d : args.diagnostics) {
    auto &out = d.is_error ? std::cerr : std::cout;
    out << (d.is_error ? "error: " : "warning: ") << d.message << "\n";
    if (d.is_error) return 1;
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
    int total_passed = 0, total_failed = 0;
    for (const auto &f : args.test_files) {
      std::cout << "=== " << f << " ===\n";
      try {
        auto r = pi::core::run_lua_test_file(f);
        total_passed += r.passed;
        total_failed += r.failed;
        std::cout << r.passed << "/" << r.total << " passed";
        if (r.failed > 0) std::cout << ", " << r.failed << " failed";
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
