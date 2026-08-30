#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <format>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <locale>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <filesystem>
#include <fstream>
#include <unistd.h>

#include "cli/args.h"
#include "cli/config.h"
#include "cli/faux_control_mode.h"
#include "cli/model_selector.h"
#include "cli/readline.h"
#include "cli/rpc_mode.h"
#include "cli/session_runtime.h"
#include "cli/system_prompt.h"
#include "cli/tree_selector.h"
#include "core/agent.h"
#include "core/agent_loop.h"
#include "core/agent_state.h"
#include "core/agent_task.h"
#include "core/auth/auth_resolver.h"
#include "core/auth/openai_codex_oauth.h"
#include "core/auth_types.h"
#include "core/builtin_tools.h"
#include "core/compaction.h"
#include "core/event_types.h"
#include "core/lua_tool.h"
#include "core/mailbox/mailbox_bindings.h"
#include "core/mailbox/mailbox_coordinator.h"
#include "core/mailbox/mailbox_types.h"
#include "core/memory_stats.h"
#include "core/message_types.h"
#include "core/models.h"
#include "core/otel_init.h"
#include "core/providers/faux_control.h"
#include "core/providers/muse_messages.h"
#include "core/providers/openai_codex_responses.h"
#include "core/providers/openai_completions.h"
#include "core/sandbox.h"
#include "core/session/agent_session.h"
#include "core/session/session_id.h"
#include "core/session/session_record.h"
#include "core/session/session_store.h"
#include "core/session/session_tree.h"
#include "core/skills.h"
#include "core/stream_diagnostics.h"
#include "core/stream_renderer.h"
#include "core/subagent_activity.h"
#include "core/subagent_panel.h"
#include "core/terminal.h"
#include "nlohmann/json_fwd.hpp"
#include <nlohmann/json.hpp>

#include "cli/cmd_run_session.h"

namespace pi {

namespace {

void print_version() { std::cout << "pi-cpp " PI_CPP_VERSION "\n"; }

std::shared_ptr<const core::ModelRegistry>
build_model_registry(const cli::Args &args) {
  static const std::map<std::string, cli::ProviderConfig> empty;
  const auto &configured =
      args.config_document ? args.config_document->providers : empty;
  auto registry = std::make_shared<core::ModelRegistry>(configured);
  registry->validate_registered_apis();
  return registry;
}

int cmd_list_models(
    const cli::Args &args,
    const std::shared_ptr<const core::ModelRegistry> &registry) {
  std::cout << format_model_catalog(args.list_models_filter, registry);
  return 0;
}

void print_auth_help(const char *prog) {
  std::cout
      << "Usage: " << prog
      << " auth <login|status|logout> openai-codex [--device|--browser]\n\n"
         "Commands:\n"
         "  login openai-codex       Sign in with ChatGPT/Codex OAuth\n"
         "  status openai-codex      Show stored authentication status\n"
         "  logout openai-codex      Remove stored authentication\n\n"
         "Login defaults to a loopback browser flow. Use --device when a\n"
         "browser callback cannot be used. Credentials are stored in the\n"
         "pici auth file with restrictive permissions.\n";
}

int cmd_auth(const cli::Args &args, const char *prog) {
  if (args.auth_action == cli::AuthAction::help) {
    print_auth_help(prog);
    return 0;
  }
  if (args.auth_provider != "openai-codex") {
    std::cerr << "error: only openai-codex authentication is supported\n";
    return 1;
  }
  if (args.auth_action != cli::AuthAction::login &&
      (args.auth_device || args.auth_browser)) {
    std::cerr << "error: login mode flags are only valid with auth login\n";
    return 1;
  }

  try {
    pi::auth::OpenAICodexOAuth oauth;
    if (args.auth_action == cli::AuthAction::status) {
      const auto credential = oauth.store().read_oauth("openai-codex");
      if (!credential) {
        std::cout << "openai-codex    oauth   not authenticated\n";
        return 0;
      }
      const auto expires = std::chrono::system_clock::time_point(
          std::chrono::milliseconds(credential->expires_at_ms));
      const auto time = std::chrono::system_clock::to_time_t(expires);
      std::tm utc{};
      gmtime_r(&time, &utc);
      std::cout << "openai-codex    oauth   authenticated "
                << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ") << "\n";
      return 0;
    }
    if (args.auth_action == cli::AuthAction::logout) {
      oauth.store().erase("openai-codex");
      std::cout << "Logged out of openai-codex.\n";
      return 0;
    }

    pi::auth::OpenAICodexLoginOptions options;
    options.mode = args.auth_device ? pi::auth::OpenAICodexLoginMode::device
                                    : pi::auth::OpenAICodexLoginMode::browser;
    options.notify = [](std::string_view message) {
      std::cout << message << "\n";
    };
    const auto credential = oauth.login(options);
    oauth.store().modify_oauth(
        "openai-codex", [&](const std::optional<pi::auth::OAuthCredential> &) {
          return std::optional<pi::auth::OAuthCredential>{credential};
        });
    std::cout << "Logged in to openai-codex.\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << "\n";
    return 1;
  }
}

} // namespace

} // namespace pi

// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char *argv[]) noexcept {
  // Installed once, before any worker threads exist, so there is no
  // concurrent std::signal() call to race with.
  std::signal(SIGINT, [](int) {
    pi::core::notify_sigint();
  }); // NOLINT(concurrency-mt-unsafe)
  std::signal(SIGTERM,
              [](int) { std::exit(0); }); // NOLINT(concurrency-mt-unsafe)

  pi::core::register_openai_completions_client();
  pi::core::register_openai_codex_responses_client();
  pi::core::register_muse_messages_client();

  auto args = pi::cli::load_and_merge(argc, argv);

  for (const auto &d : args.diagnostics) {
    auto &out = d.is_error ? std::cerr : std::cout;
    out << (d.is_error ? "error: " : "warning: ") << d.message << "\n";
    if (d.is_error)
      return 1;
  }
  if (args.help) {
    pi::cli::print_help(*argv);
    return 0;
  }
  if (args.version) {
    pi::print_version();
    return 0;
  }
  if (args.auth_action != pi::cli::AuthAction::none)
    return pi::cmd_auth(args, *argv);

  // Initialise OTel export if requested. The RAII guard + atexit ensure
  // BatchSpanProcessor is flushed before normal process exit.
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

  std::shared_ptr<const pi::core::ModelRegistry> model_registry;
  try {
    model_registry = pi::build_model_registry(args);
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << "\n";
    return 1;
  }

  if (args.list_models) {
    return pi::cmd_list_models(args, model_registry);
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

  return pi::cmd_run(args, model_registry);
}
