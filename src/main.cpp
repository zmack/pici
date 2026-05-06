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
#include "core/agent.h"
#include "core/agent_state.h"
#include "core/builtin_tools.h"
#include "core/env_api_keys.h"
#include "core/event_types.h"
#include "core/lua_tool.h"
#include "core/message_types.h"
#include "core/models.h"
#include "core/providers/openai_completions.h"
#include "core/stream_renderer.h"

namespace pi {

static void print_version() {
  std::cout << "pi-cpp " PI_CPP_VERSION "\n";
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

  // Load Lua hooks before constructing agent
  if (!args.hooks_file.empty()) {
    try {
      auto hooks = core::load_lua_hooks(args.hooks_file);
      if (hooks->before_tool_call)     opts.before_tool_call     = hooks->before_tool_call;
      if (hooks->after_tool_call)      opts.after_tool_call      = hooks->after_tool_call;
      if (hooks->should_stop_after_turn) opts.should_stop_after_turn = hooks->should_stop_after_turn;
      if (args.verbose)
        std::cerr << "[hooks: " << args.hooks_file << "]\n";
    } catch (const std::exception &e) {
      std::cerr << "warning: failed to load hooks file: " << e.what() << "\n";
    }
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

  // Interactive REPL
  std::string line;
  while (true) {
    std::cout << "\n> " << std::flush;
    if (!std::getline(std::cin, line)) break;
    if (line.empty()) continue;
    if (line == "/exit" || line == "/quit") break;
    run_turn(agent, line, *renderer, args.verbose);
  }
  return 0;
}

} // namespace pi

int main(int argc, char *argv[]) {
  std::signal(SIGINT, [](int) { std::exit(0); });
  std::signal(SIGTERM, [](int) { std::exit(0); });

  pi::core::register_openai_completions_client();

  auto args = pi::cli::parse_args(argc, argv);

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

  return pi::cmd_run(args);
}
