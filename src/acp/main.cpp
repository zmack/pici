#include "acp/server.h"
#include "cli/args.h"
#include "cli/config.h"
#include "cli/system_prompt.h"
#include "core/auth/auth_resolver.h"
#include "core/auth_types.h"
#include "core/builtin_tools.h"
#include "core/env_api_keys.h"
#include "core/lua_tool.h"
#include "core/message_types.h"
#include "core/models.h"
#include "core/otel_init.h"
#include "core/providers/muse_messages.h"
#include "core/providers/openai_codex_responses.h"
#include "core/providers/openai_completions.h"
#include "core/sandbox.h"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void print_usage(const char *prog) {
  std::cout << "Usage: " << prog
            << " [--port <n>] [--model <id>] [--provider <name>]"
               " [--base-url <url>] [--api-key <key>]"
               " [--system-prompt <text>] [--tools-dir <dir>]"
               " [--no-tools] [--sandbox <mode>] [--acp-threads <n>]\n";
}

} // namespace

// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char *argv[]) noexcept {
  // Installed once, before any worker threads exist, so there is no
  // concurrent std::signal() call to race with.
  std::signal(SIGINT,
              [](int) { std::exit(0); }); // NOLINT(concurrency-mt-unsafe)
  std::signal(SIGTERM,
              [](int) { std::exit(0); }); // NOLINT(concurrency-mt-unsafe)

  pi::core::register_openai_completions_client();
  pi::core::register_openai_codex_responses_client();
  pi::core::register_muse_messages_client();

  // Parse shared CLI flags
  auto args = pi::cli::load_and_merge(argc, argv);
  const std::span<char *> argv_view(argv, static_cast<std::size_t>(argc));

  struct OtelGuard {
    OtelGuard() = default;
    ~OtelGuard() { pi::core::shutdown_otel(); }
    OtelGuard(const OtelGuard &) = delete;
    OtelGuard &operator=(const OtelGuard &) = delete;
    OtelGuard(OtelGuard &&) = delete;
    OtelGuard &operator=(OtelGuard &&) = delete;
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

  auto sandbox_mode = pi::core::SandboxMode::auto_mode;
  if (!args.sandbox_mode.empty()) {
    const auto parsed = pi::core::sandbox_mode_from_string(args.sandbox_mode);
    if (!parsed) {
      std::cerr << "error: invalid sandbox mode \"" << args.sandbox_mode
                << "\"; valid: auto, required, disabled\n";
      return 1;
    }
    sandbox_mode = *parsed;
  }
  auto sandbox_policy = std::make_shared<pi::core::SandboxPolicy>(sandbox_mode);
  if (args.help) {
    print_usage(argv_view.front());
    return 0;
  }
  if (args.version) {
    std::cout << "pi-acp " PI_CPP_VERSION "\n";
    return 0;
  }
  if (args.auth_action != pi::cli::AuthAction::none) {
    std::cerr << "error: pi-acp does not support auth subcommands; use pi-cli "
                 "auth ...\n";
    return 1;
  }

  // --port (not in shared Args, parse manually)
  std::atomic<int> port{8080};
  int acp_threads = 4;
  for (int i = 1; i < argc; ++i) {
    std::string_view a = argv_view[static_cast<std::size_t>(i)];
    auto next = [&]() -> std::string_view {
      return (i + 1 < argc) ? argv_view[static_cast<std::size_t>(++i)] : "";
    };
    if (a == "--port")
      port = std::stoi(std::string(next()));
    if (a == "--acp-threads")
      acp_threads = std::stoi(std::string(next()));
  }

  // Resolve model (same logic as pi-cli)
  auto model_opt = pi::core::find_model(args.model, args.provider);
  pi::core::Model model = model_opt.value_or(pi::core::Model{});
  if (model.id.empty()) {
    model.id = args.model.empty() ? "default" : args.model;
    model.name = model.id;
    model.api = "openai-completions";
    model.provider = args.provider.empty() ? "local" : args.provider;
    model.base_url =
        args.base_url.empty() ? "http://127.0.0.1:8080/v1" : args.base_url;
    model.context_window = 128000;
    model.max_tokens = 4096;
  }
  if (!args.base_url.empty())
    model.base_url = args.base_url;
  if (model.provider == "openai-codex" && !args.api_key.empty()) {
    std::cerr << "error: --api-key cannot be used with openai-codex; run "
                 "pi-cli auth login openai-codex\n";
    return 1;
  }

  // Build agent options
  pi::acp::ServerConfig cfg;
  cfg.agent_description = "pi-cpp coding agent running " + model.id;
  cfg.threads = acp_threads;
  cfg.sandbox_policy = sandbox_policy;
  if (!args.session_dir.empty())
    cfg.session_dir = args.session_dir;

  cfg.agent_opts.model = model;
  auto auth_resolver = std::make_shared<pi::auth::AuthResolver>();
  if (model.provider == "openai-codex") {
    try {
      (void)auth_resolver->resolve(model.provider, args.api_key);
    } catch (const std::exception &error) {
      std::cerr << "error: " << error.what() << "\n";
      return 1;
    }
  }
  cfg.agent_opts.get_auth =
      [&args, &model, auth_resolver](
          std::string_view p) -> std::optional<pi::core::RequestAuth> {
    return auth_resolver->resolve(p.empty() ? model.provider : p, args.api_key);
  };
  cfg.agent_opts.get_api_key =
      [&args, &model](std::string_view p) -> std::optional<std::string> {
    if (!args.api_key.empty())
      return args.api_key;
    return pi::core::get_env_api_key(p.empty() ? model.provider : p);
  };

  // Tools
  if (!args.no_tools) {
    cfg.tools = pi::core::create_all_tools(std::filesystem::current_path(),
                                           sandbox_policy);
    if (!args.tools_dir.empty()) {
      for (auto &t : pi::core::load_lua_tools(args.tools_dir))
        cfg.tools.push_back(t);
    }
  }

  std::vector<std::string> tool_names;
  tool_names.reserve(cfg.tools.size());
  for (const auto &tool : cfg.tools)
    tool_names.emplace_back(tool->name());
  cfg.agent_opts.system_prompt = pi::cli::build_system_prompt(
      args.system_prompt, args.append_system_prompts, {}, tool_names,
      std::filesystem::current_path());

  std::cerr << "[acp] agent: " << cfg.agent_name
            << "  model: " << model.provider << "/" << model.id << "\n";
  std::cerr << "[acp] tools: " << cfg.tools.size() << "\n";

  try {
    pi::acp::run_server(port, std::move(cfg));
  } catch (const std::exception &e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
