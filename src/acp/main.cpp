#include "acp/server.h"
#include "cli/args.h"
#include "cli/config.h"
#include "core/builtin_tools.h"
#include "core/env_api_keys.h"
#include "core/lua_tool.h"
#include "core/message_types.h"
#include "core/models.h"
#include "core/otel_init.h"
#include "core/providers/openai_completions.h"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

static void print_usage(const char *prog) {
  std::cout << "Usage: " << prog
            << " [--port <n>] [--model <id>] [--provider <name>]"
               " [--base-url <url>] [--api-key <key>]"
               " [--system-prompt <text>] [--tools-dir <dir>]"
               " [--no-tools] [--acp-threads <n>]\n";
}

int main(int argc, char *argv[]) {
  // Installed once, before any worker threads exist, so there is no
  // concurrent std::signal() call to race with.
  std::signal(SIGINT, [](int) { std::exit(0); });   // NOLINT(concurrency-mt-unsafe)
  std::signal(SIGTERM, [](int) { std::exit(0); });  // NOLINT(concurrency-mt-unsafe)

  pi::core::register_openai_completions_client();

  // Parse shared CLI flags
  auto args = pi::cli::load_and_merge(argc, argv);

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
    print_usage(argv[0]);
    return 0;
  }
  if (args.version) {
    std::cout << "pi-acp " PI_CPP_VERSION "\n";
    return 0;
  }

  // --port (not in shared Args, parse manually)
  std::atomic<int> port{8080};
  int acp_threads = 4;
  for (int i = 1; i < argc; ++i) {
    std::string_view a = argv[i];
    auto next = [&]() -> std::string_view {
      return (i + 1 < argc) ? argv[++i] : "";
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

  // Build agent options
  pi::acp::ServerConfig cfg;
  cfg.agent_description = "pi-cpp coding agent running " + model.id;
  cfg.threads = acp_threads;

  cfg.agent_opts.model = model;
  cfg.agent_opts.system_prompt = args.system_prompt;
  cfg.agent_opts.get_api_key =
      [&args, &model](std::string_view p) -> std::optional<std::string> {
    if (!args.api_key.empty())
      return args.api_key;
    return pi::core::get_env_api_key(p.empty() ? model.provider : p);
  };

  // Tools
  if (!args.no_tools) {
    cfg.tools = pi::core::create_all_tools(std::filesystem::current_path());
    if (!args.tools_dir.empty()) {
      for (auto &t : pi::core::load_lua_tools(args.tools_dir))
        cfg.tools.push_back(t);
    }
  }

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
