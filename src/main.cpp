#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <format>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
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
#include "core/stream_diagnostics.h"
#include "core/stream_renderer.h"
#include "core/terminal.h"
#include "nlohmann/json_fwd.hpp"
#include <nlohmann/json.hpp>

namespace pi {

namespace {

void print_version() { std::cout << "pi-cpp " PI_CPP_VERSION "\n"; }

struct HookRuntime {
  std::mutex mutex;
  std::shared_ptr<core::LuaHooks> hooks;
};

std::filesystem::path bundled_mailbox_addon_path() {
  // The project currently has no install target; keep the bundled addon next
  // to its source tree until runtime resource packaging is introduced.
  return std::filesystem::path(PI_CPP_SOURCE_DIR) / "addons" / "mailbox.lua";
}

bool is_mailbox_tool(std::string_view name) {
  return name == "agents_list" || name == "agents_send" ||
         name == "agents_request" || name == "agents_reply" ||
         name == "agents_inbox" || name == "agents_close";
}

struct MailboxTaskObserver {
  std::mutex mutex;
  std::weak_ptr<core::MailboxCoordinator> coordinator;

  void observe(const core::AgentTaskEvent &event) {
    std::shared_ptr<core::MailboxCoordinator> current;
    {
      std::scoped_lock lock(mutex);
      current = coordinator.lock();
    }
    if (current)
      current->observe_task_event(event);
  }

  void detach() {
    std::scoped_lock lock(mutex);
    coordinator.reset();
  }
};

struct MailboxLifecycleGuard {
  std::shared_ptr<core::AgentTaskManager> task_manager;
  std::shared_ptr<MailboxTaskObserver> observer;
  std::shared_ptr<core::MailboxCoordinator> coordinator;
  std::shared_ptr<core::MailboxDeliveryTargets> delivery;
  MailboxLifecycleGuard(std::shared_ptr<core::AgentTaskManager> manager,
                        std::shared_ptr<MailboxTaskObserver> value,
                        std::shared_ptr<core::MailboxCoordinator> native,
                        std::shared_ptr<core::MailboxDeliveryTargets> target)
      : task_manager(std::move(manager)), observer(std::move(value)),
        coordinator(std::move(native)), delivery(std::move(target)) {}
  MailboxLifecycleGuard(const MailboxLifecycleGuard &) = delete;
  MailboxLifecycleGuard &operator=(const MailboxLifecycleGuard &) = delete;
  MailboxLifecycleGuard(MailboxLifecycleGuard &&) = delete;
  MailboxLifecycleGuard &operator=(MailboxLifecycleGuard &&) = delete;
  ~MailboxLifecycleGuard() noexcept {
    if (task_manager) {
      try {
        task_manager->shutdown();
      } catch (...) {
        static_cast<void>(0);
      }
    }
    if (coordinator)
      coordinator->detach_delivery();
    delivery.reset();
    if (observer)
      observer->detach();
    if (coordinator)
      coordinator->stop();
  }
};

struct RootRunningGuard {
  std::shared_ptr<core::MailboxCoordinator> coordinator;
  explicit RootRunningGuard(std::shared_ptr<core::MailboxCoordinator> value)
      : coordinator(std::move(value)) {}
  RootRunningGuard(const RootRunningGuard &) = delete;
  RootRunningGuard &operator=(const RootRunningGuard &) = delete;
  RootRunningGuard(RootRunningGuard &&) = delete;
  RootRunningGuard &operator=(RootRunningGuard &&) = delete;
  ~RootRunningGuard() noexcept {
    if (coordinator) {
      try {
        coordinator->set_root_running(false);
      } catch (...) {
        static_cast<void>(0);
      }
    }
  }
};

std::string local_hostname() {
  std::array<char, 256> buffer{};
  if (::gethostname(buffer.data(), buffer.size() - 1) == 0) {
    buffer.back() = '\0';
    return {buffer.data()};
  }
  return "local";
}

std::string workspace_identity(const std::filesystem::path &workspace_path) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (const auto character : workspace_path.string()) {
    hash ^= static_cast<unsigned char>(character);
    hash *= 1099511628211ULL;
  }
  std::ostringstream result;
  result << "workspace-" << std::hex << hash;
  return result.str();
}

void print_tools(
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

void print_addons(
    const std::vector<std::shared_ptr<core::LuaHooks>> &hooks_list) {
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
    if (h->prompt_line)
      active.emplace_back("prompt_line");
    if (h->status_line)
      active.emplace_back("status_line");
    if (h->tab_title)
      active.emplace_back("tab_title");
    if (h->format_tool_call)
      active.emplace_back("format_tool_call");
    if (h->format_tool_result)
      active.emplace_back("format_tool_result");
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

// Returns "$0.0023" for 0.002341928, "$1.23" for 1.234, "$12.34" for 12.345
std::string format_cost(double usd) {
  std::ostringstream ss;
  ss << '$';
  if (usd < 0.01)
    ss << std::format("{:.4g}", usd);
  else if (usd < 1.00)
    ss << std::fixed << std::setprecision(4) << usd;
  else
    ss << std::fixed << std::setprecision(2) << usd;
  return ss.str();
}

std::string format_tokens(std::uint64_t n) {
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

std::shared_ptr<const core::ModelRegistry>
build_model_registry(const cli::Args &args) {
  static const std::map<std::string, cli::ProviderConfig> empty;
  const auto &configured =
      args.config_document ? args.config_document->providers : empty;
  auto registry = std::make_shared<core::ModelRegistry>(configured);
  registry->validate_registered_apis();
  return registry;
}

std::string format_model_catalog(
    std::string_view filter,
    const std::shared_ptr<const core::ModelRegistry> &registry) {
  auto hits = registry->search(filter);
  if (hits.empty()) {
    if (!filter.empty())
      return "No models matching \"" + std::string(filter) + "\"\n";
    return "No models available.\n";
  }

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

  std::ostringstream out;
  auto row = [&](std::string_view prov, std::string_view id,
                 std::string_view ctx, std::string_view mx,
                 std::string_view reason, std::string_view img) {
    out << std::left << std::setw(static_cast<int>(wprov + 2)) << prov
        << std::setw(static_cast<int>(wid + 2)) << id
        << std::setw(static_cast<int>(wctx + 2)) << ctx
        << std::setw(static_cast<int>(wmax + 2)) << mx << std::setw(10)
        << reason << img << "\n";
  };
  row("provider", "model", "context", "max-out", "thinking", "images");
  for (const auto *m : hits) {
    const bool has_image = std::ranges::find(m->input_capabilities, "image") !=
                           m->input_capabilities.end();
    row(m->provider, m->id, format_tokens(m->context_window),
        format_tokens(m->max_tokens), m->reasoning ? "yes" : "no",
        has_image ? "yes" : "no");
  }
  return out.str();
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

core::ThinkingLevel to_core_thinking(cli::ThinkingLevel t) {
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

core::ModelResolution
resolve_model(const cli::Args &args,
              const std::shared_ptr<const core::ModelRegistry> &registry) {
  if (!args.model.empty()) {
    core::ModelSelection selection;
    if (!args.provider.empty())
      selection.provider = args.provider;
    selection.model = args.model;
    if (!args.base_url.empty())
      selection.base_url = args.base_url;
    selection.source = "cli";
    auto resolution = registry->resolve(selection);
    return resolution;
  }

  if (!args.provider.empty()) {
    if (const auto *provider = registry->provider(args.provider)) {
      core::Model model;
      model.id = "default";
      model.name = "default";
      model.api = provider->api;
      model.provider = provider->id;
      model.base_url = provider->base_url;
      model.input_capabilities = {"text"};
      model.context_window = 128000;
      model.max_tokens = 4096;
      if (!args.base_url.empty())
        model.base_url = args.base_url;
      return {.model = std::move(model)};
    }
    if (args.base_url.empty())
      return {.error = "cli: unknown provider '" + args.provider + "'"};
  }

  core::Model model;
  model.id = "default";
  model.name = "default";
  model.api = "openai-completions";
  model.provider = args.provider.empty() ? "local" : args.provider;
  model.base_url =
      args.base_url.empty() ? "http://127.0.0.1:8080/v1" : args.base_url;
  model.input_capabilities = {"text"};
  model.context_window = 128000;
  model.max_tokens = 4096;
  return {.model = std::move(model)};
}

std::unique_ptr<core::Renderer> make_renderer(const cli::Args &args) {
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

std::string format_tool_result(std::string_view content) {
  return core::truncate_tool_result(content);
}

// A renderer adapter that adds verbose tool/usage output on top of any base
// renderer.
class VerboseRenderer final : public core::Renderer {
public:
  VerboseRenderer(core::Renderer &base, bool verbose,
                  std::shared_ptr<core::StreamDiagnostics> diagnostics,
                  std::shared_ptr<HookRuntime> hook_runtime = nullptr)
      : base_(base), verbose_(verbose), diagnostics_(std::move(diagnostics)),
        hook_runtime_(std::move(hook_runtime)) {}

  void on_turn_start() override {
    if (diagnostics_)
      diagnostics_->record_renderer_event("turn_start");
    base_.on_turn_start();
  }
  void on_request(const core::RendererRequest &request) override {
    base_.on_request(request);
  }
  void on_mailbox_reply_queued(
      std::string_view call_id,
      const core::MailboxReplyQueuedNotice &notice) override {
    base_.on_mailbox_reply_queued(call_id, notice);
  }

  void on_text_delta(std::string_view d) override {
    if (diagnostics_)
      diagnostics_->record_renderer_event("text_delta", d.size());
    base_.on_text_delta(d);
  }
  void on_thinking_start() override {
    if (diagnostics_)
      diagnostics_->record_renderer_event("thinking_start");
    base_.on_thinking_start();
  }
  void on_thinking_delta(std::string_view d) override {
    if (diagnostics_)
      diagnostics_->record_renderer_event("thinking_delta", d.size());
    base_.on_thinking_delta(d);
  }
  void on_thinking_end() override {
    if (diagnostics_)
      diagnostics_->record_renderer_event("thinking_end");
    base_.on_thinking_end();
  }

  void on_tool_update(std::string_view call_id, std::string_view name,
                      std::string_view partial_result) override {
    if (diagnostics_)
      diagnostics_->record_renderer_event("tool_update", partial_result.size());
    base_.on_tool_update(call_id, name, partial_result);
  }

  void on_tool_start(std::string_view call_id, std::string_view name,
                     std::string_view args_json) override {
    if (diagnostics_)
      diagnostics_->record_renderer_event("tool_start", args_json.size());
    base_.on_tool_start(call_id, name, args_json);
    // Cache parsed args for format_tool_result hook (which doesn't receive them
    // natively).
    nlohmann::json parsed_args = nlohmann::json::object();
    {
      auto j = nlohmann::json::parse(args_json, nullptr, false);
      if (!j.is_discarded() && j.is_object())
        parsed_args = std::move(j);
    }
    pending_tool_args_[std::string(call_id)] = parsed_args;

    std::optional<std::string> custom_display;
    std::shared_ptr<core::LuaHooks> hooks;
    if (hook_runtime_) {
      std::scoped_lock lock(hook_runtime_->mutex);
      hooks = hook_runtime_->hooks;
    }
    if (hooks && hooks->format_tool_call) {
      core::LuaHooks::FormatToolCallContext ctx;
      ctx.tool_name = std::string(name);
      ctx.call_id = std::string(call_id);
      ctx.args = parsed_args;
      if (auto custom = hooks->format_tool_call(ctx)) {
        auto sanitized = core::sanitize_tool_output(*custom);
        if (!sanitized.empty() && sanitized.back() != '\n')
          sanitized += '\n';
        custom_display = std::move(sanitized);
      }
    }
    if (custom_display) {
      // Genuine hook-produced text: route it through the base renderer (or
      // stdout) exactly like before.
      emit_tool_output(call_id, *custom_display);
    } else if (!owns_tool_output()) {
      // No hook: only the flat-stdout renderers need the fallback string.
      // A renderer that owns its own tool presentation (RegionRenderer)
      // already has call_id/name/args_json from base_.on_tool_start above
      // and builds its own display from that — piping this fallback text
      // through on_tool_output_text would just clobber it.
      std::string display = "\n[tool: ";
      display += name;
      display += "(\033[38;5;214m";
      display += args_json;
      display += "\033[0m)]\n";
      std::cout << display << std::flush;
    }
  }
  void on_tool_end(std::string_view call_id, std::string_view name,
                   const core::ToolResult &result, bool is_error) override {
    if (diagnostics_)
      diagnostics_->record_renderer_event("tool_end", result.content().size());
    base_.on_tool_end(call_id, name, result, is_error);
    std::shared_ptr<core::LuaHooks> hooks;
    if (hook_runtime_) {
      std::scoped_lock lock(hook_runtime_->mutex);
      hooks = hook_runtime_->hooks;
    }
    nlohmann::json args = nlohmann::json::object();
    auto it = pending_tool_args_.find(std::string(call_id));
    if (it != pending_tool_args_.end()) {
      args = it->second;
      pending_tool_args_.erase(it);
    }

    std::optional<std::string> custom_display;
    if (hooks && hooks->format_tool_result) {
      core::LuaHooks::FormatToolResultContext ctx;
      ctx.tool_name = std::string(name);
      ctx.call_id = std::string(call_id);
      ctx.args = std::move(args);
      ctx.content = result.content();
      ctx.is_error = is_error;
      if (auto custom = hooks->format_tool_result(ctx)) {
        auto sanitized = core::sanitize_tool_output(*custom);
        if (!sanitized.empty() && sanitized.back() != '\n')
          sanitized += '\n';
        custom_display = std::move(sanitized);
      }
    }
    if (custom_display) {
      emit_tool_output(call_id, *custom_display);
    } else if (!owns_tool_output()) {
      // See the matching comment in on_tool_start: a renderer that owns its
      // own tool presentation already has the result via base_.on_tool_end
      // above and builds its own display from that.
      std::string display = "\033[38;5;245m  [";
      display += name;
      display += "] ";
      display += format_tool_result(result.content());
      display += "\033[0m\n";
      std::cout << display << std::flush;
    }
  }

  void on_message_end_presentation(
      const core::MessageEndPresentation &end) override {
    if (diagnostics_)
      diagnostics_->record_renderer_event("message_end");
    base_.on_message_end_presentation(end);
    const auto &u = end.usage;
    if (verbose_ && !base_.owns_status_line()) {
      bool has_pricing = u.cost.total != 0 || u.cost.input != 0;
      std::cerr << "[usage: in=" << format_tokens(u.input)
                << " out=" << format_tokens(u.output)
                << " cache_r=" << format_tokens(u.cache_read);
      if (has_pricing)
        std::cerr << " " << format_cost(u.cost.total);
      std::cerr << "]\n";
    }
    last_usage_ = u;
  }

  void on_message_end(const core::TokenUsage &u) override {
    if (diagnostics_)
      diagnostics_->record_renderer_event("message_end");
    base_.on_message_end(u);
    if (verbose_ && !base_.owns_status_line()) {
      bool has_pricing = u.cost.total != 0 || u.cost.input != 0;
      std::cerr << "[usage: in=" << format_tokens(u.input)
                << " out=" << format_tokens(u.output)
                << " cache_r=" << format_tokens(u.cache_read);
      if (has_pricing)
        std::cerr << " " << format_cost(u.cost.total);
      std::cerr << "]\n";
    }
    last_usage_ = u;
  }

  void on_turn_end() override {
    if (diagnostics_)
      diagnostics_->record_renderer_event("turn_end");
    base_.on_turn_end();
  }

  void on_command_output(std::string_view text) override {
    base_.on_command_output(text);
  }

  void on_error(core::RendererErrorKind kind, std::string_view msg) override {
    if (diagnostics_)
      diagnostics_->record_renderer_event("error", msg.size());
    base_.on_error(kind, msg);
    if (!base_.owns_status_line())
      std::cerr << "\nerror: " << msg << "\n";
  }

  void on_compaction_start() override {
    if (diagnostics_)
      diagnostics_->record_renderer_event("compaction_start");
    base_.on_compaction_start();
  }

  void on_compaction_complete(std::size_t retained_message_count,
                              const core::TokenUsage &usage_before,
                              const core::TokenUsage &usage_after) override {
    if (diagnostics_)
      diagnostics_->record_renderer_event("compaction_complete");
    base_.on_compaction_complete(retained_message_count, usage_before,
                                 usage_after);
  }

  void on_compaction_error(std::string_view message, bool cancelled) override {
    if (diagnostics_)
      diagnostics_->record_renderer_event("compaction_error", message.size());
    base_.on_compaction_error(message, cancelled);
  }

  void on_scroll(core::RendererScrollCommand command) override {
    base_.on_scroll(command);
  }

  bool owns_status_line() const override { return base_.owns_status_line(); }

  bool owns_tool_output() const override { return base_.owns_tool_output(); }

  void on_tool_output_text(std::string_view call_id,
                           std::string_view text) override {
    base_.on_tool_output_text(call_id, text);
  }

  void set_status_line(const std::optional<std::string> &text) override {
    base_.set_status_line(text);
  }

  const core::TokenUsage &last_usage() const { return last_usage_; }

private:
  void emit_tool_output(std::string_view call_id, std::string_view text) {
    if (owns_tool_output()) {
      base_.on_tool_output_text(call_id, text);
      return;
    }
    std::cout << text << std::flush;
  }

  core::Renderer &base_;
  bool verbose_;
  std::shared_ptr<core::StreamDiagnostics> diagnostics_;
  std::shared_ptr<HookRuntime> hook_runtime_;
  std::unordered_map<std::string, nlohmann::json> pending_tool_args_;
  core::TokenUsage last_usage_;
};

template <typename Invoke>
core::TokenUsage
run_turn_impl(core::AgentSession &session, core::Renderer &renderer,
              bool verbose,
              std::shared_ptr<core::StreamDiagnostics> diagnostics,
              std::shared_ptr<HookRuntime> hook_runtime, Invoke &&invoke) {
  VerboseRenderer vr(renderer, verbose, std::move(diagnostics),
                     std::move(hook_runtime));
  std::jthread interrupt_watcher([&session](const std::stop_token &stop_token) {
    while (!stop_token.stop_requested()) {
      if (core::consume_sigint()) {
        session.agent().interrupt(core::TurnAbortReason::user_interrupt);
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  });
  auto result =
      std::forward<Invoke>(invoke)([&vr](const core::AgentEvent &event) {
        core::dispatch_event(event, vr);
      });
  interrupt_watcher.request_stop();
  if (result.error && !session.agent().state().error_message())
    renderer.on_error(core::RendererErrorKind::unknown, *result.error);
  return vr.last_usage();
}

core::TokenUsage run_turn(core::AgentSession &session, const std::string &input,
                          core::Renderer &renderer, bool verbose,
                          std::shared_ptr<core::StreamDiagnostics> diagnostics,
                          std::shared_ptr<HookRuntime> hook_runtime = nullptr) {
  return run_turn_impl(session, renderer, verbose, std::move(diagnostics),
                       std::move(hook_runtime),
                       [&session, &input](const auto &callback) {
                         return session.run_prompt(input, callback);
                       });
}

core::TokenUsage
run_message_turn(core::AgentSession &session,
                 std::vector<core::AgentMessageEnvelope> messages,
                 core::Renderer &renderer, bool verbose,
                 std::shared_ptr<core::StreamDiagnostics> diagnostics,
                 std::shared_ptr<HookRuntime> hook_runtime = nullptr) {
  return run_turn_impl(
      session, renderer, verbose, std::move(diagnostics),
      std::move(hook_runtime),
      [&session, messages = std::move(messages)](const auto &callback) mutable {
        return session.run_messages(std::move(messages), callback);
      });
}

// Drives AgentSession::compact_active_session to completion, reusing the
// exact interrupt-watcher pattern run_turn_impl uses for prompts: Ctrl-C is
// polled on a jthread and forwarded to Agent::interrupt(), which stops the
// shared stop_token a running compaction request observes. Unlike
// run_turn_impl, the result type here (CompactionRunResult) carries
// success/unsupported/cancelled/error directly, so callers do not need to
// re-derive status from renderer state.
core::AgentSession::CompactionRunResult
run_compaction_command(core::AgentSession &session, core::Renderer &renderer,
                       bool verbose,
                       std::shared_ptr<core::StreamDiagnostics> diagnostics,
                       std::shared_ptr<HookRuntime> hook_runtime,
                       core::CompactionTrigger trigger) {
  VerboseRenderer vr(renderer, verbose, std::move(diagnostics),
                     std::move(hook_runtime));
  std::jthread interrupt_watcher([&session](const std::stop_token &stop_token) {
    while (!stop_token.stop_requested()) {
      if (core::consume_sigint()) {
        session.agent().interrupt(core::TurnAbortReason::user_interrupt);
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  });
  auto result = session.compact_active_session(
      trigger, [&vr](const core::AgentEvent &event) {
        core::dispatch_event(event, vr);
      });
  interrupt_watcher.request_stop();
  return result;
}

struct CostAccumulator {
  std::uint64_t input_tokens{0};
  std::uint64_t output_tokens{0};
  std::uint64_t cache_read_tokens{0};
  std::uint64_t cache_write_tokens{0};
  std::uint64_t total_tokens{0};
  double total_cost{0.0};
  std::size_t turns{0};

  void add(const core::TokenUsage &u) {
    input_tokens += u.input;
    output_tokens += u.output;
    cache_read_tokens += u.cache_read;
    cache_write_tokens += u.cache_write;
    total_tokens += u.total_tokens;
    total_cost += u.cost.total;
    ++turns;
  }
};

void print_usage(const CostAccumulator &last, const CostAccumulator &session,
                 bool has_pricing) {
  auto row = [&](std::string_view label, const CostAccumulator &acc) {
    std::cout << std::left << std::setw(10) << label << "  in=" << std::setw(8)
              << format_tokens(acc.input_tokens) << "  out=" << std::setw(8)
              << format_tokens(acc.output_tokens)
              << "  cache_r=" << std::setw(8)
              << format_tokens(acc.cache_read_tokens)
              << "  cache_w=" << std::setw(8)
              << format_tokens(acc.cache_write_tokens);
    if (has_pricing)
      std::cout << "  " << format_cost(acc.total_cost);
    std::cout << "\n";
  };
  std::cout << "\n";
  row("last turn:", last);
  row("session:", session);
  std::cout << "  turns: " << session.turns;
  if (!has_pricing)
    std::cout << "  (cost unknown)";
  std::cout << "\n";
}

std::vector<cli::ContextFile> load_context_files() {
  namespace fs = std::filesystem;
  static constexpr std::array<std::string_view, 4> kCandidates = {
      "AGENTS.md", "AGENTS.MD", "CLAUDE.md", "CLAUDE.MD"};

  auto try_load = [&](const fs::path &dir) -> std::optional<cli::ContextFile> {
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
      return cli::ContextFile{.path = p.string(),
                              .content = std::move(content)};
    }
    return std::nullopt;
  };

  std::vector<cli::ContextFile> result;
  std::set<std::string> seen;

  // Walk up from cwd to root, collecting innermost-first then reversing
  std::vector<cli::ContextFile> ancestors;
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
  std::ranges::reverse(ancestors);
  result.insert(result.end(), ancestors.begin(), ancestors.end());
  return result;
}

// cmd_run is the CLI's whole interactive-session entry point (renderer,
// session, mailbox, and REPL command-dispatch setup all live in this one
// scope so they can share local state via closures without a context
// struct). Splitting it apart is a real, worthwhile refactor, but it's a
// standalone architectural change with meaningful regression risk, not a
// lint cleanup — deliberately not attempted here.
// NOLINTNEXTLINE(readability-function-size)
int cmd_run(const cli::Args &args,
            const std::shared_ptr<const core::ModelRegistry> &registry) {
  auto effective_registry = registry;
  std::shared_ptr<core::RemoteFauxClient> remote_client;
  std::shared_ptr<core::ScriptedToolRegistry> scripted_registry;
  core::Model model;
  if (args.faux_control_socket.empty()) {
    const auto resolution = resolve_model(args, registry);
    if (!resolution) {
      std::cerr << "error: " << resolution.error << "\n";
      return 1;
    }
    model = *resolution.model;
  } else {
    core::ProviderConfig provider;
    provider.id = "faux-control";
    provider.api = "faux-control";
    provider.base_url = "http://faux-control";
    provider.auth = core::ProviderAuthPolicy::none;
    core::ConfiguredModel configured;
    configured.id = "faux-control";
    configured.name = "faux-control";
    provider.models.push_back(configured);
    effective_registry = std::make_shared<const core::ModelRegistry>(
        std::map<std::string, core::ProviderConfig>{
            {"faux-control", provider}});
    model.id = "faux-control";
    model.name = "faux-control";
    model.api = "faux-control";
    model.provider = "faux-control";
    model.base_url = "http://faux-control";
    model.input_capabilities = {"text"};
    model.context_window = 128000;
    model.max_tokens = 4096;
    remote_client = std::make_shared<core::RemoteFauxClient>();
    scripted_registry = std::make_shared<core::ScriptedToolRegistry>();
    core::LLMClientRegistry::instance().register_client(
        "faux-control", [remote_client] { return remote_client; });
  }
  if (model.provider == "openai-codex" && !args.api_key.empty()) {
    std::cerr << "error: --api-key cannot be used with openai-codex; run "
                 "pi-cli auth login openai-codex\n";
    return 1;
  }

  auto store = std::make_shared<core::SessionStore>(
      args.session_dir.empty() ? core::SessionStore::default_sessions_dir()
                               : std::filesystem::path(args.session_dir));

  std::optional<core::SessionRecord> loaded_session;

  if (args.session_continue) {
    auto id = store->latest_session_id();
    if (!id) {
      std::cerr << "error: no previous session found\n";
      return 1;
    }
    loaded_session = store->load(*id);
  } else if (!args.session_resume.empty()) {
    auto matches = store->find_by_prefix(args.session_resume);
    if (matches.empty()) {
      std::cerr << "error: no session matching \"" << args.session_resume
                << "\"\n";
      return 1;
    }
    if (matches.size() > 1) {
      std::cerr << "error: ambiguous prefix \"" << args.session_resume
                << "\" matches " << matches.size() << " sessions:\n";
      for (const auto &h : matches)
        std::cerr << "  " << h.id << (h.name ? ("  " + *h.name) : "") << "\n";
      return 1;
    }
    loaded_session = store->load(matches[0].id);
  }

  core::SandboxMode sandbox_mode = core::SandboxMode::auto_mode;
  std::optional<std::string_view> sandbox_setting;
  if (loaded_session && !args.sandbox_mode_explicit &&
      loaded_session->header.sandbox_mode)
    sandbox_setting = *loaded_session->header.sandbox_mode;
  else if (!args.sandbox_mode.empty())
    sandbox_setting = args.sandbox_mode;
  if (sandbox_setting) {
    const auto parsed = core::sandbox_mode_from_string(*sandbox_setting);
    if (!parsed) {
      std::cerr << "error: invalid sandbox mode \"" << *sandbox_setting
                << "\"; valid: auto, required, disabled\n";
      return 1;
    }
    sandbox_mode = *parsed;
  }
  auto sandbox_policy = std::make_shared<core::SandboxPolicy>(sandbox_mode);

  std::vector<cli::ContextFile> context_files;
  if (!args.no_context_files) {
    context_files = load_context_files();
    for (const auto &cf : context_files) {
      if (args.verbose)
        std::cerr << "[context: " << cf.path << "]\n";
    }
  }

  core::Agent::Options opts;
  opts.model = model;
  opts.model_registry = effective_registry;
  opts.system_prompt = args.system_prompt;
  opts.thinking_level = to_core_thinking(args.thinking);
  auto hook_runtime = std::make_shared<HookRuntime>();
  std::mutex effective_context_mutex;
  std::optional<core::AgentContext> effective_context;
  opts.on_effective_context = [&](const core::AgentContext &context) {
    std::scoped_lock lock(effective_context_mutex);
    effective_context = context;
  };
  std::shared_ptr<core::StreamDiagnostics> stream_diagnostics;
  if (!args.stream_trace.empty()) {
    try {
      stream_diagnostics =
          std::make_shared<core::StreamDiagnostics>(args.stream_trace);
    } catch (const std::exception &e) {
      std::cerr << "error: " << e.what() << "\n";
      return 1;
    }
  }
  opts.diagnostics = stream_diagnostics;
  auto auth_resolver =
      std::make_shared<pi::auth::AuthResolver>(effective_registry);
  if (!args.api_key.empty())
    auth_resolver->set_runtime_api_key(model.provider, args.api_key);
  opts.get_auth =
      [auth_resolver](std::string_view p) -> std::optional<core::RequestAuth> {
    return auth_resolver->resolve(p);
  };
  opts.get_api_key =
      [auth_resolver](std::string_view p) -> std::optional<std::string> {
    auto auth = auth_resolver->resolve(p);
    if (!auth || !auth->bearer_token)
      return std::nullopt;
    return auth->bearer_token;
  };
  opts.verbose = args.verbose;

  // Load and compose Lua hooks
  std::vector<std::shared_ptr<core::LuaHooks>> hooks_list_saved;
  bool auto_mailbox_addon_loaded = false;
  auto load_hooks = [&](bool allow_auto_mailbox =
                            true) -> std::shared_ptr<core::LuaHooks> {
    std::vector<std::shared_ptr<core::LuaHooks>> hooks_list;
    const auto bundled_mailbox = bundled_mailbox_addon_path();
    auto same_path = [](const std::filesystem::path &left,
                        const std::filesystem::path &right) {
      std::error_code left_error;
      std::error_code right_error;
      const auto left_canonical =
          std::filesystem::weakly_canonical(left, left_error);
      const auto right_canonical =
          std::filesystem::weakly_canonical(right, right_error);
      if (!left_error && !right_error)
        return left_canonical == right_canonical;
      return left.lexically_normal() == right.lexically_normal();
    };
    bool mailbox_addon_explicit = false;
    for (const auto &path : args.hooks_files) {
      const bool is_bundled_mailbox = same_path(path, bundled_mailbox);
      mailbox_addon_explicit |= is_bundled_mailbox;
      try {
        hooks_list.push_back(core::load_lua_hooks(path, is_bundled_mailbox));
        if (args.verbose)
          std::cerr << "[hooks: " << path << "]\n";
      } catch (const std::exception &e) {
        std::cerr << "warning: failed to load hooks file " << path << ": "
                  << e.what() << "\n";
      }
    }
    if (!args.hooks_dir.empty()) {
      const bool bundled_mailbox_dir =
          same_path(std::filesystem::path(args.hooks_dir) / "mailbox.lua",
                    bundled_mailbox);
      auto dir_hooks =
          core::load_lua_hooks_dir(args.hooks_dir, bundled_mailbox_dir);
      if (dir_hooks) {
        hooks_list.push_back(dir_hooks);
        if (args.verbose)
          std::cerr << "[hooks-dir: " << args.hooks_dir << "]\n";
      }
    }
    if (!args.hooks_dir.empty()) {
      std::error_code mailbox_entry_error;
      const auto mailbox_entry =
          std::filesystem::path(args.hooks_dir) / "mailbox.lua";
      mailbox_addon_explicit =
          std::filesystem::exists(mailbox_entry, mailbox_entry_error) ||
          mailbox_addon_explicit;
    }
    if (allow_auto_mailbox && args.mailbox_enabled && !mailbox_addon_explicit) {
      try {
        hooks_list.push_back(core::load_lua_hooks(bundled_mailbox, true));
        auto_mailbox_addon_loaded = true;
        if (args.verbose)
          std::cerr << "[hooks: " << bundled_mailbox << "]\n";
      } catch (const std::exception &e) {
        std::cerr << "warning: failed to load bundled mailbox addon: "
                  << e.what() << "\n";
      }
    }

    hooks_list_saved = hooks_list;
    return core::compose_hooks(std::move(hooks_list));
  };
  std::shared_ptr<core::LuaHooks> hooks;
  auto load_presentation_hooks = [&]() -> std::shared_ptr<core::LuaHooks> {
    if (args.faux_control_socket.empty())
      return load_hooks();
    if (args.hooks_files.empty() && args.hooks_dir.empty())
      return nullptr;
    return core::tool_formatter_hooks_only(load_hooks(false));
  };
  hooks = load_presentation_hooks();
  {
    std::scoped_lock lock(hook_runtime->mutex);
    hook_runtime->hooks = hooks;
  }

  opts.before_tool_call =
      [hook_runtime](const core::BeforeToolCallContext &context,
                     std::stop_token stop_tok)
      -> std::optional<core::BeforeToolCallResult> {
    std::shared_ptr<core::LuaHooks> hooks;
    {
      std::scoped_lock lock(hook_runtime->mutex);
      hooks = hook_runtime->hooks;
    }
    if (hooks && hooks->before_tool_call)
      return hooks->before_tool_call(context, std::move(stop_tok));
    return std::nullopt;
  };
  opts.after_tool_call =
      [hook_runtime](const core::AfterToolCallContext &context,
                     std::stop_token stop_tok)
      -> std::optional<core::AfterToolCallResult> {
    std::shared_ptr<core::LuaHooks> hooks;
    {
      std::scoped_lock lock(hook_runtime->mutex);
      hooks = hook_runtime->hooks;
    }
    if (hooks && hooks->after_tool_call)
      return hooks->after_tool_call(context, std::move(stop_tok));
    return std::nullopt;
  };
  opts.should_stop_after_turn =
      [hook_runtime](const core::Message &message,
                     const std::vector<core::ToolResultMessage> &results,
                     const core::AgentContext &context) {
        std::shared_ptr<core::LuaHooks> hooks;
        {
          std::scoped_lock lock(hook_runtime->mutex);
          hooks = hook_runtime->hooks;
        }
        return hooks && hooks->should_stop_after_turn
                   ? hooks->should_stop_after_turn(message, results, context)
                   : false;
      };
  opts.on_event = [hook_runtime](const core::AgentEvent &event) {
    std::shared_ptr<core::LuaHooks> hooks;
    {
      std::scoped_lock lock(hook_runtime->mutex);
      hooks = hook_runtime->hooks;
    }
    if (hooks && hooks->on_event)
      hooks->on_event(event);
  };
  opts.prepare_context = [hook_runtime](const core::AgentContext &context,
                                        std::size_t estimated_tokens,
                                        std::stop_token stop_tok)
      -> std::optional<std::vector<core::Message>> {
    std::shared_ptr<core::LuaHooks> hooks;
    {
      std::scoped_lock lock(hook_runtime->mutex);
      hooks = hook_runtime->hooks;
    }
    if (hooks && hooks->prepare_context)
      return hooks->prepare_context(context, estimated_tokens,
                                    std::move(stop_tok));
    return std::nullopt;
  };

  core::AgentSession runtime(
      {.agent_options = opts,
       .session_store = store,
       .sandbox_policy = sandbox_policy,
       .auto_compaction = {.enabled = args.remote_compaction_enabled,
                           .threshold_pct = args.compaction_threshold_pct > 0.0
                                                ? args.compaction_threshold_pct
                                                : 0.85}});
  auto &agent = runtime.agent();
  std::function<void(const std::string &)> faux_tool_registrar;
  if (scripted_registry) {
    faux_tool_registrar = [scripted_registry, &agent](const std::string &name) {
      agent.add_tool(
          std::make_shared<core::ScriptedTool>(name, scripted_registry));
    };
  }

  if (!args.no_tools && !args.no_builtin_tools &&
      args.faux_control_socket.empty()) {
    if (args.tools.empty()) {
      agent.set_tools(core::create_all_tools(std::filesystem::current_path(),
                                             sandbox_policy));
    } else {
      // Allowlist filter
      for (auto &t : core::create_all_tools(std::filesystem::current_path(),
                                            sandbox_policy)) {
        for (const auto &name : args.tools) {
          if (t->name() == name) {
            agent.add_tool(t);
            break;
          }
        }
      }
    }
  }

  if (!args.no_tools && !args.tools_dir.empty() &&
      args.faux_control_socket.empty()) {
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

  const auto base_tools = agent.state().tools();
  auto apply_hook_tools = [&]() {
    auto tools = base_tools;
    if (hooks)
      tools.insert(tools.end(), hooks->registered_tools.begin(),
                   hooks->registered_tools.end());
    agent.set_tools(std::move(tools));
  };
  if (args.faux_control_socket.empty())
    apply_hook_tools();

  std::vector<std::string> tool_names;
  for (const auto &tool : agent.state().tools())
    tool_names.emplace_back(tool->name());
  const auto system = cli::build_system_prompt(
      args.system_prompt, args.append_system_prompts, context_files, tool_names,
      std::filesystem::current_path());
  agent.state().set_system_prompt(system);
  opts.system_prompt = system;

  // --list-tools / --list-addons (exit immediately after printing)
  if (args.list_tools) {
    print_tools(agent.state().tools());
    return 0;
  }
  if (args.list_addons) {
    print_addons(hooks_list_saved);
    return 0;
  }

  std::shared_ptr<core::MailboxCoordinator> mailbox;
  if (args.mailbox_enabled) {
    std::filesystem::path workspace_path;
    try {
      workspace_path =
          std::filesystem::weakly_canonical(std::filesystem::current_path());
    } catch (...) {
      workspace_path = std::filesystem::current_path();
    }
    const auto settings = args.config_document ? args.config_document->mailbox
                                               : cli::MailboxSettings{};
    std::filesystem::path mailbox_path =
        args.mailbox_path.empty() ? settings.path
                                  : std::filesystem::path(args.mailbox_path);
    if (mailbox_path.empty())
      mailbox_path = cli::default_mailbox_path(
          args.config_path.empty() ? cli::default_config_path()
                                   : std::filesystem::path(args.config_path));
    const auto resolved_mailbox_path = mailbox_path;
    const auto now = [] {
      return std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::system_clock::now().time_since_epoch())
          .count();
    };
    core::MailboxCoordinatorOptions mailbox_options;
    mailbox_options.store.path = std::move(mailbox_path);
    mailbox_options.store.workspace_id = workspace_identity(workspace_path);
    mailbox_options.store.workspace_path = workspace_path.string();
    mailbox_options.store.scope = settings.scope == "global"
                                      ? core::MailboxScope::global
                                      : core::MailboxScope::workspace;
    mailbox_options.store.claim_lease_ms = settings.claim_lease_ms;
    mailbox_options.store.retention_days = settings.retention_days;
    mailbox_options.store.clock = now;
    mailbox_options.store.id_generator = [] {
      return core::generate_session_id();
    };
    mailbox_options.process_id = core::generate_session_id();
    mailbox_options.root_agent_id = core::generate_session_id();
    mailbox_options.provider = model.provider;
    mailbox_options.model_id = model.id;
    mailbox_options.hostname = local_hostname();
    mailbox_options.heartbeat_interval =
        std::chrono::milliseconds(settings.heartbeat_interval_ms);
    mailbox_options.stale_after =
        std::chrono::milliseconds(settings.stale_after_ms);
    mailbox_options.poll_interval =
        std::chrono::milliseconds(settings.poll_interval_ms);
    mailbox_options.cleanup_interval = std::chrono::hours(1);
    try {
      mailbox = std::make_shared<core::MailboxCoordinator>(mailbox_options);
      if (args.verbose)
        std::cerr << "mailbox enabled: path=" << resolved_mailbox_path
                  << " workspace_id=" << mailbox_options.store.workspace_id
                  << " workspace_path=" << workspace_path
                  << " process_id=" << mailbox_options.process_id
                  << " agent_id=" << mailbox_options.root_agent_id
                  << " lease_ms=" << settings.stale_after_ms
                  << " poll_ms=" << settings.poll_interval_ms << " schema=1\n";
    } catch (const core::MailboxError &error) {
      std::cerr << "mailbox disabled: "
                << core::mailbox_error_code_to_string(error.code()) << ": "
                << error.what() << "\n";
      mailbox.reset();
    } catch (const std::exception &error) {
      std::cerr << "mailbox disabled: " << error.what() << "\n";
      mailbox.reset();
    }
    if (!mailbox && auto_mailbox_addon_loaded) {
      std::erase_if(hooks->registered_tools, [](const auto &tool) {
        return tool && is_mailbox_tool(tool->name());
      });
      auto_mailbox_addon_loaded = false;
      {
        std::scoped_lock lock(hook_runtime->mutex);
        hook_runtime->hooks = hooks;
      }
      if (args.faux_control_socket.empty())
        apply_hook_tools();
    }
  }
  auto mailbox_observer = std::make_shared<MailboxTaskObserver>();
  mailbox_observer->coordinator = mailbox;
  auto task_callbacks = std::vector<core::AgentTaskEventCallback>{};
  if (mailbox) {
    task_callbacks.emplace_back(
        [mailbox_observer](const core::AgentTaskEvent &event) {
          mailbox_observer->observe(event);
        });
  }
  auto task_manager = std::make_shared<core::AgentTaskManager>(
      runtime, opts, core::AgentTaskManager::Limits{},
      core::fan_out_agent_task_callbacks(std::move(task_callbacks)));
  if (mailbox) {
    task_manager->set_endpoint_registration(
        [mailbox](const core::AgentTaskId &task_id,
                  const std::string &task_path,
                  const std::optional<core::AgentTaskId> &parent_id) {
          return mailbox->register_subagent(task_id, task_path, parent_id);
        },
        [mailbox](const core::AgentTaskId &task_id) {
          mailbox->unregister_subagent(task_id);
        });
  }
  auto mailbox_wake = mailbox ? std::make_shared<cli::ReadlineWake>() : nullptr;
  auto mailbox_delivery = std::make_shared<core::MailboxDeliveryTargets>();
  mailbox_delivery->root =
      [&runtime](std::vector<core::AgentMessageEnvelope> messages) {
        runtime.agent().steer_envelopes(std::move(messages));
        return true;
      };
  mailbox_delivery->subagent =
      [task_manager](const std::string &, const std::string &task_id,
                     std::vector<core::AgentMessageEnvelope> messages) {
        try {
          task_manager->steer_envelopes(task_id, std::move(messages));
          return true;
        } catch (const core::AgentTaskError &) {
          return false;
        } catch (...) {
          return false;
        }
      };
  mailbox_delivery->drop_queued = [&runtime, task_manager] {
    runtime.agent().clear_mailbox_steering_queue();
    task_manager->drop_mailbox_envelopes();
  };
  mailbox_delivery->drop_root_queued = [&runtime] {
    runtime.agent().clear_mailbox_steering_queue();
  };
  mailbox_delivery->root_wake = [mailbox_wake] {
    if (mailbox_wake)
      static_cast<void>(mailbox_wake->notify());
  };
  if (mailbox)
    mailbox->attach_delivery(mailbox_delivery);
  MailboxLifecycleGuard mailbox_lifecycle{task_manager, mailbox_observer,
                                          mailbox, mailbox_delivery};
  auto compat_counter = std::make_shared<std::atomic_uint64_t>(0);

  auto task_error_json = [](const core::AgentTaskError &error) {
    return nlohmann::json{
        {"error", {{"code", error.code()}, {"message", error.what()}}}};
  };
  auto exception_json = [](const std::exception &error) {
    return nlohmann::json{
        {"error", {{"code", "internal"}, {"message", error.what()}}}};
  };
  auto snapshot_json = [](const core::AgentTaskSnapshot &snapshot) {
    nlohmann::json value = {
        {"id", snapshot.id},
        {"task_path", snapshot.task_path},
        {"task_name", snapshot.task_name},
        {"status", core::agent_task_status_to_string(snapshot.status)},
        {"child_count", snapshot.child_count},
        {"queued_message_count", snapshot.queued_message_count},
        {"generation", snapshot.generation},
    };
    if (snapshot.parent_id)
      value["parent_id"] = *snapshot.parent_id;
    else
      value["parent_id"] = nullptr;
    if (snapshot.result) {
      value["result"] = {
          {"text", snapshot.result->text},
          {"stop_reason",
           core::stop_reason_to_string(snapshot.result->stop_reason)},
          {"truncated", snapshot.result->truncated},
          {"usage",
           {{"input", snapshot.result->usage.input},
            {"output", snapshot.result->usage.output},
            {"total_tokens", snapshot.result->usage.total_tokens}}},
      };
      if (snapshot.result->error)
        value["result"]["error"] = *snapshot.result->error;
      else
        value["result"]["error"] = nullptr;
    } else {
      value["result"] = nullptr;
    }
    return value;
  };

  auto configure_hooks = [&]() {
    if (!hooks || !hooks->configure)
      return;

    std::vector<std::string> tool_names;
    for (const auto &t : agent.state().tools())
      tool_names.emplace_back(t->name());

    std::filesystem::path storage_path;
    if (!args.hooks_files.empty())
      storage_path =
          std::filesystem::path(args.hooks_files[0]).string() + ".storage.json";

    const auto current_model = agent.state().model();
    core::LuaHooks::AgentInfo info;
    info.model_id = current_model.id;
    info.model_provider = current_model.provider;
    info.model_api = current_model.api;
    info.tool_names = std::move(tool_names);
    info.cwd = std::filesystem::current_path().string();
    info.storage_path = std::move(storage_path);
    info.run_agent = [task_manager,
                      compat_counter](const core::LuaHooks::AgentRunConfig &cfg)
        -> core::LuaHooks::AgentRunResult {
      core::LuaHooks::AgentRunResult result;
      try {
        core::SpawnAgentRequest request;
        request.task_name =
            "compat_" + std::to_string(compat_counter->fetch_add(1) + 1);
        request.prompt = cfg.prompt;
        request.system_prompt = cfg.system_prompt;
        request.model_spec = cfg.model_id;
        request.requested_tools = cfg.tools;
        if (cfg.fork_at > 0) {
          request.context.mode = core::ContextInheritanceMode::through_message;
          request.context.through = cfg.fork_at;
        }
        auto current = task_manager->spawn(request);
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(60);
        for (;;) {
          if (current.status == core::AgentTaskStatusKind::completed ||
              current.status == core::AgentTaskStatusKind::errored ||
              current.status == core::AgentTaskStatusKind::interrupted)
            break;
          const auto remaining =
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  deadline - std::chrono::steady_clock::now());
          if (remaining <= std::chrono::milliseconds::zero()) {
            task_manager->interrupt(current.id,
                                    core::AgentInterruptReason::timeout);
            result.error = "sub-agent wait timed out";
            break;
          }
          core::AgentWaitRequest wait_request;
          wait_request.targets = {current.id};
          wait_request.after_generation = current.generation;
          wait_request.timeout =
              std::min(remaining, std::chrono::milliseconds(1000));
          auto update = task_manager->wait(wait_request);
          if (update.timed_out)
            continue;
          for (const auto &changed : update.changed)
            if (changed.id == current.id)
              current = changed;
        }
        if (!result.error && current.result) {
          result.text = current.result->text;
          result.error = current.result->error;
        }
        task_manager->close(current.id);
      } catch (const std::exception &error) {
        result.error = error.what();
      }
      return result;
    };

    info.mailbox = core::make_mailbox_bindings(mailbox);

    auto make_message = [](const nlohmann::json &value) {
      core::UserMessage message;
      message.content.emplace_back(
          core::TextContent{.text = value.value("message", std::string{})});
      return core::Message{std::move(message)};
    };
    auto parse_context = [](const nlohmann::json &value,
                            core::ContextInheritance &context) {
      if (!value.is_object())
        return;
      const auto mode = value.value("mode", std::string("none"));
      if (mode == "none")
        context.mode = core::ContextInheritanceMode::none;
      else if (mode == "full")
        context.mode = core::ContextInheritanceMode::full;
      else if (mode == "through_message")
        context.mode = core::ContextInheritanceMode::through_message;
      else if (mode == "recent_messages")
        context.mode = core::ContextInheritanceMode::recent_messages;
      else
        throw core::AgentTaskError(core::AgentTaskErrorKind::invalid_context,
                                   "unknown context inheritance mode");
      if (value.contains("through"))
        context.through = value.at("through").get<std::size_t>();
      if (value.contains("recent_count"))
        context.recent_count = value.at("recent_count").get<std::size_t>();
    };

    info.agents.spawn = [task_manager, snapshot_json, task_error_json,
                         exception_json,
                         parse_context](const nlohmann::json &v) {
      try {
        core::SpawnAgentRequest request;
        request.parent_id = v.value("parent_id", std::string{});
        request.task_name = v.value("task_name", std::string{});
        request.prompt = v.value("message", v.value("prompt", std::string{}));
        if (v.contains("context"))
          parse_context(v.at("context"), request.context);
        if (v.contains("system_prompt") && !v.at("system_prompt").is_null())
          request.system_prompt = v.at("system_prompt").get<std::string>();
        if (v.contains("model") && !v.at("model").is_null())
          request.model_spec = v.at("model").get<std::string>();
        if (v.contains("tools"))
          request.requested_tools =
              v.at("tools").get<std::vector<std::string>>();
        request.allow_subagents = v.value("allow_subagents", false);
        return snapshot_json(task_manager->spawn(request));
      } catch (const core::AgentTaskError &error) {
        return task_error_json(error);
      } catch (const std::exception &error) {
        return exception_json(error);
      }
    };
    info.agents.get = [task_manager, snapshot_json](const nlohmann::json &v) {
      try {
        const auto target = v.value("target", v.value("id", std::string{}));
        auto snapshot = task_manager->get(target);
        if (!snapshot)
          return nlohmann::json{
              {"error",
               {{"code", "not_found"}, {"message", "task not found"}}}};
        return snapshot_json(*snapshot);
      } catch (const std::exception &error) {
        return nlohmann::json{
            {"error", {{"code", "internal"}, {"message", error.what()}}}};
      }
    };
    info.agents.list = [task_manager, snapshot_json](const nlohmann::json &v) {
      nlohmann::json values = nlohmann::json::array();
      const auto prefix = v.value("path_prefix", std::string{});
      for (const auto &snapshot : task_manager->list(
               prefix.empty() ? std::optional<std::string_view>{}
                              : std::optional<std::string_view>{prefix}))
        values.push_back(snapshot_json(snapshot));
      return values;
    };
    auto queue_binding = [task_manager, snapshot_json, task_error_json,
                          exception_json, make_message](const nlohmann::json &v,
                                                        bool follow_up) {
      try {
        const auto target = v.value("target", std::string{});
        const auto message = make_message(v);
        auto snapshot = follow_up ? task_manager->follow_up(target, message)
                                  : task_manager->send_message(target, message);
        return snapshot_json(snapshot);
      } catch (const core::AgentTaskError &error) {
        return task_error_json(error);
      } catch (const std::exception &error) {
        return exception_json(error);
      }
    };
    info.agents.send_message = [queue_binding](const nlohmann::json &v) {
      return queue_binding(v, false);
    };
    info.agents.follow_up = [queue_binding](const nlohmann::json &v) {
      return queue_binding(v, true);
    };
    info.agents.interrupt = [task_manager, snapshot_json, task_error_json,
                             exception_json](const nlohmann::json &v) {
      try {
        const auto reason = v.value("reason", std::string("parent"));
        core::AgentInterruptReason parsed = core::AgentInterruptReason::parent;
        if (reason == "user")
          parsed = core::AgentInterruptReason::user;
        else if (reason == "shutdown")
          parsed = core::AgentInterruptReason::shutdown;
        else if (reason == "timeout")
          parsed = core::AgentInterruptReason::timeout;
        return snapshot_json(
            task_manager->interrupt(v.value("target", std::string{}), parsed));
      } catch (const core::AgentTaskError &error) {
        return task_error_json(error);
      } catch (const std::exception &error) {
        return exception_json(error);
      }
    };
    info.agents.wait = [task_manager, snapshot_json, task_error_json,
                        exception_json](const nlohmann::json &v) {
      try {
        core::AgentWaitRequest request;
        if (v.contains("targets"))
          request.targets = v.at("targets").get<std::vector<std::string>>();
        request.after_generation = v.value("after_generation", 0ULL);
        request.timeout =
            std::chrono::milliseconds(v.value("timeout_ms", 30000ULL));
        const auto result = task_manager->wait(request);
        nlohmann::json changed = nlohmann::json::array();
        for (const auto &snapshot : result.changed)
          changed.push_back(snapshot_json(snapshot));
        return nlohmann::json{{"timed_out", result.timed_out},
                              {"caller_interrupted", result.caller_interrupted},
                              {"generation", result.generation},
                              {"changed", std::move(changed)}};
      } catch (const core::AgentTaskError &error) {
        return task_error_json(error);
      } catch (const std::exception &error) {
        return exception_json(error);
      }
    };
    info.agents.close = [task_manager, snapshot_json, task_error_json,
                         exception_json](const nlohmann::json &v) {
      try {
        return snapshot_json(
            task_manager->close(v.value("target", std::string{})));
      } catch (const core::AgentTaskError &error) {
        return task_error_json(error);
      } catch (const std::exception &error) {
        return exception_json(error);
      }
    };

    hooks->configure(info);
  };

  std::string current_session_id;
  std::optional<std::string> current_session_name;

  auto reload_addons = [&]() {
    hooks = load_presentation_hooks();
    {
      std::scoped_lock lock(hook_runtime->mutex);
      hook_runtime->hooks = hooks;
    }
    if (args.faux_control_socket.empty())
      apply_hook_tools();
    configure_hooks();
  };

  if (loaded_session) {
    current_session_id = loaded_session->header.id;
    current_session_name = loaded_session->header.name;
    runtime.activate_session(*loaded_session);
    if (runtime.last_warning())
      std::cerr << "warning: " << *runtime.last_warning() << "\n";
    if (args.sandbox_mode_explicit)
      runtime.set_sandbox_mode(sandbox_mode);
    std::cerr << "[session: " << current_session_id;
    if (loaded_session->header.name)
      std::cerr << "  " << *loaded_session->header.name;
    std::cerr << "]\n";
  } else {
    core::SessionHeader hdr;
    hdr.id = core::generate_session_id();
    hdr.created =
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    const auto current_model = agent.state().model();
    hdr.model = current_model.id;
    hdr.provider = current_model.provider;
    hdr.sandbox_mode = std::string(core::sandbox_mode_to_string(sandbox_mode));
    current_session_id = runtime.create_session(hdr);
  }

  if (loaded_session && (args.model_explicit || args.provider_explicit ||
                         args.base_url_explicit)) {
    try {
      const auto result =
          runtime.set_model(model, to_core_thinking(args.thinking));
      if (result.warning)
        std::cerr << "warning: " << *result.warning << "\n";
    } catch (const std::exception &error) {
      std::cerr << "error: unable to apply explicit model selection: "
                << error.what() << "\n";
      return 1;
    }
  }

  if (mailbox) {
    const auto identity =
        mailbox->activate_root(current_session_id, current_session_name);
    agent.set_runtime_identity(identity);
    const auto current_model = agent.state().model();
    mailbox->set_model(current_model.provider, current_model.id);
  }

  configure_hooks();

  if (remote_client) {
    auto faux_renderer = make_renderer(args);
    VerboseRenderer renderer_adapter(*faux_renderer, args.verbose,
                                     stream_diagnostics, hook_runtime);
    return cli::run_faux_control_socket(
        runtime, *remote_client, scripted_registry, args.faux_control_socket,
        faux_tool_registrar,
        [&renderer_adapter](const core::AgentEvent &event) {
          core::dispatch_event(event, renderer_adapter);
        });
  }

  if (args.rpc_mode)
    return cli::run_rpc_mode(runtime, std::cin, std::cout, task_manager.get(),
                             auth_resolver);

  std::string startup_cwd;
  try {
    startup_cwd = std::filesystem::current_path().string();
  } catch (...) {
    startup_cwd = "";
  }
  const std::string initial_project_label =
      core::terminal_project_label(startup_cwd);
  core::TerminalTitleController title_controller(STDOUT_FILENO,
                                                 initial_project_label);

  auto renderer = make_renderer(args);

  if (sandbox_mode == core::SandboxMode::disabled)
    std::cerr << "[sandbox: disabled; bash runs without bubblewrap]\n";
  else if (args.verbose)
    std::cerr << "[sandbox: " << core::sandbox_mode_to_string(sandbox_mode)
              << "]\n";

  if (args.verbose) {
    const auto current_model = agent.state().model();
    std::cerr << "[model: " << current_model.provider << "/" << current_model.id
              << "]\n";
    std::cerr << "[tools: " << agent.state().tools().size() << "]\n";
  }

  // Build completion function.
  // Command-name completion (/... with no space) is handled here from the
  // declared commands list — no Lua needed.  Argument completion (/cmd ...
  // with a space) is delegated to hooks->complete.
  cli::CompleteFn complete_fn =
      [&hooks, &agent,
       &registry](std::string_view partial) -> std::vector<std::string> {
    std::vector<std::string> result;
    const bool is_slash = !partial.empty() && partial[0] == '/';
    const bool has_space = partial.contains(' ');

    if (is_slash && !has_space) {
      // Complete command names: builtins + declared add-on commands
      for (std::string_view b :
           {std::string_view("/exit"), std::string_view("/quit"),
            std::string_view("/tools"), std::string_view("/addons"),
            std::string_view("/reload-addons"), std::string_view("/usage"),
            std::string_view("/model"), std::string_view("/models"),
            std::string_view("/name"), std::string_view("/fork"),
            std::string_view("/tree"), std::string_view("/compact")}) {
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

    const auto space = partial.find(' ');
    if (is_slash && has_space && space != std::string_view::npos) {
      const auto command = partial.substr(0, space);
      if (command == "/model") {
        const auto prefix = partial.substr(space + 1);
        for (const auto &candidate : registry->models()) {
          const std::string canonical = candidate.provider + "/" + candidate.id;
          if (std::string_view(canonical).starts_with(prefix))
            result.push_back(canonical);
        }
        return result;
      }
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
  CostAccumulator last_turn;
  CostAccumulator session;
  // Last single-turn TokenUsage for Lua prompt_line hook.
  core::TokenUsage last_usage_for_prompt{};
  core::TokenUsage session_usage_for_prompt{};
  auto build_session_usage = [&]() -> core::TokenUsage {
    core::TokenUsage u;
    u.input = session.input_tokens;
    u.output = session.output_tokens;
    u.cache_read = session.cache_read_tokens;
    u.cache_write = session.cache_write_tokens;
    u.total_tokens = session.total_tokens;
    u.cost.total = session.total_cost;
    return u;
  };
  auto build_ui_context = [&]() {
    const auto current_model = agent.state().model();
    core::LuaUiContext context;
    context.model = current_model.id;
    context.tools = agent.state().tools().size();
    context.last = last_usage_for_prompt;
    context.session = session_usage_for_prompt;
    context.session_id = current_session_id;
    context.session_name = current_session_name;
    for (const auto &message : agent.state().messages()) {
      if (std::holds_alternative<core::AssistantMessage>(message))
        ++context.turn;
    }
    return context;
  };
  auto has_current_pricing = [&]() {
    const auto current_model = agent.state().model();
    return current_model.cost.input_per_mtok != 0 ||
           current_model.cost.output_per_mtok != 0;
  };

  auto accumulate = [&](const core::TokenUsage &u) {
    last_turn = CostAccumulator{};
    last_turn.add(u);
    session.add(u);
    last_usage_for_prompt = u;
    session_usage_for_prompt = build_session_usage();
  };

  // Run a turn and persist all new messages to the session file.
  auto run_and_persist = [&](const std::string &input) {
    core::TerminalTitleActivityGuard activity(title_controller);
    if (mailbox) {
      mailbox->set_root_running(true);
      mailbox->pump_inbox();
    }
    RootRunningGuard running_guard{mailbox};
    auto result = run_turn(runtime, input, *renderer, args.verbose,
                           stream_diagnostics, hook_runtime);
    return result;
  };

  constexpr std::size_t kAutonomousBatchLimit = 16;
  core::MailboxAutonomousTurnBudget autonomous_budget;
  bool autonomous_budget_exhausted = false;
  auto run_idle_mailbox_turns = [&] {
    if (!mailbox || autonomous_budget_exhausted)
      return;
    while (autonomous_budget.can_run()) {
      std::vector<core::AgentMessageEnvelope> messages;
      try {
        messages = mailbox->claim_idle_root_turn(kAutonomousBatchLimit);
      } catch (const std::exception &error) {
        if (args.verbose)
          std::cerr << "[mailbox autonomous turn unavailable: " << error.what()
                    << "]\n";
        return;
      }
      if (messages.empty())
        return;
      {
        core::TerminalTitleActivityGuard activity(title_controller);
        RootRunningGuard running_guard{mailbox};
        accumulate(run_message_turn(runtime, std::move(messages), *renderer,
                                    args.verbose, stream_diagnostics,
                                    hook_runtime));
      }
      autonomous_budget.record();
    }
    autonomous_budget_exhausted = autonomous_budget.exhausted();
  };

  auto update_terminal_ui = [&]() -> std::optional<std::string> {
    const auto context = build_ui_context();
    std::optional<std::string> status_line;
    if (hooks && hooks->status_line)
      status_line = hooks->status_line(context);
    if (autonomous_budget_exhausted) {
      constexpr std::string_view paused =
          "mailbox autonomous turns paused; submit input to resume";
      if (status_line)
        *status_line += " | " + std::string(paused);
      else
        status_line = std::string(paused);
    }
    renderer->set_status_line(status_line);
    if (hooks && hooks->tab_title) {
      if (auto title = hooks->tab_title(context))
        title_controller.set_base_title(*title);
    }
    return status_line;
  };

  // Print mode / initial message
  if (args.print_mode || !args.messages.empty()) {
    std::string prompt;
    for (const auto &msg : args.messages) {
      if (!prompt.empty())
        prompt += '\n';
      prompt += msg;
    }
    if (!prompt.empty()) {
      accumulate(run_and_persist(prompt));
      std::cout << "\n";
    }
    if (args.print_mode)
      return 0;
  }

  std::string readline_draft;
  std::size_t readline_cursor = 0;
  while (true) {
    // Build the prompt — let add-ons customise it. The chevron uses the same
    // bold-cyan accent as the region renderer's REQUEST heading so the "this
    // is user input" color reads consistently end to end; \033[22;39m clears
    // only weight/foreground so the input box's background tint survives.
    std::string prompt = "\n\033[1;36m›\033[22;39m ";
    if (hooks && hooks->prompt_line) {
      const auto &msgs = agent.state().messages();
      std::size_t turns = 0;
      for (const auto &m : msgs)
        if (std::holds_alternative<core::AssistantMessage>(m))
          ++turns;
      auto custom = hooks->prompt_line(
          turns, agent.state().model().id, agent.state().tools().size(),
          last_usage_for_prompt, session_usage_for_prompt);
      if (custom)
        prompt = "\n" + *custom;
    }
    const auto status_line = update_terminal_ui();
    const std::string_view readline_status =
        renderer->owns_status_line() || !status_line ? std::string_view{}
                                                     : *status_line;
    const int readline_wake_fd = mailbox_wake && !autonomous_budget_exhausted
                                     ? mailbox_wake->read_fd()
                                     : -1;
    // Full-screen renderers (currently only --render region) echo the
    // submitted request in their own scrolling history, so the readline box
    // should empty immediately on submit instead of leaving the typed text
    // on screen — duplicated — for the whole turn. Such renderers also own a
    // fixed alt-screen layout that needs repainting on a terminal resize.
    const bool full_screen_prompt = renderer->owns_status_line();
    const auto on_prompt_resize = [&] { renderer->on_resize(); };
    auto readline_result =
        cli::readline(prompt, complete_fn, control_fn, readline_status,
                      readline_draft, readline_cursor, readline_wake_fd,
                      full_screen_prompt, on_prompt_resize, args.vim_mode);
    if (readline_result.reason == cli::ReadlineExit::eof)
      break;
    if (readline_result.reason == cli::ReadlineExit::mailbox_wake) {
      readline_draft = std::move(readline_result.text);
      readline_cursor = readline_result.cursor;
      run_idle_mailbox_turns();
      continue;
    }
    if (autonomous_budget_exhausted && mailbox_wake)
      mailbox_wake->drain();
    autonomous_budget.reset();
    autonomous_budget_exhausted = false;
    readline_draft.clear();
    readline_cursor = 0;
    const std::string &line = readline_result.text;
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
    if (line == "/reload-addons") {
      try {
        reload_addons();
        renderer->on_command_output("reloaded " +
                                    std::to_string(hooks_list_saved.size()) +
                                    " add-on(s)");
      } catch (const std::exception &e) {
        renderer->on_command_output("add-on reload failed: " +
                                    std::string(e.what()));
      }
      continue;
    }
    if (line == "/compact") {
      // Manual compaction is interactive-only: it drives a live status
      // hook and an interruptible network request, neither of which has a
      // meaningful non-TTY/piped equivalent. The interactive command loop
      // (this while(true) loop) does run with piped stdin — see /model's
      // isatty() branch above for the established pattern of a slash
      // command explicitly degrading for non-interactive input rather than
      // silently misbehaving — so this guard is reachable and load-bearing,
      // not defensive dead code.
      if (isatty(STDIN_FILENO) == 0) {
        renderer->on_command_output(
            "/compact is only available in an interactive terminal session");
        continue;
      }
      core::TerminalTitleActivityGuard activity(title_controller);
      auto result = run_compaction_command(runtime, *renderer, args.verbose,
                                           stream_diagnostics, hook_runtime,
                                           core::CompactionTrigger::manual);
      if (result.success) {
        renderer->on_command_output(
            "compacted context: " +
            std::to_string(result.retained_message_count) +
            " message(s) retained");
      } else if (result.unsupported) {
        renderer->on_command_output(
            "compaction is not supported by the active provider/model");
      } else if (result.cancelled) {
        renderer->on_command_output("compaction cancelled");
      } else {
        renderer->on_command_output("compaction failed: " +
                                    result.error.value_or("unknown error"));
      }
      continue;
    }
    if (line == "/model" || line.starts_with("/model ")) {
      std::string spec = line.size() > 6 ? line.substr(6) : "";
      spec.erase(0, spec.find_first_not_of(" \t"));
      if (spec.empty()) {
        const auto current_model = agent.state().model();
        if (isatty(STDIN_FILENO) == 0) {
          renderer->on_command_output("model: " + current_model.provider + "/" +
                                      current_model.id +
                                      "\nusage: /model <provider/model>");
          continue;
        }
        const auto selected = cli::run_model_selector(
            registry->search(""), current_model.provider, current_model.id,
            [auth_resolver](const core::Model &candidate) {
              switch (auth_resolver->availability(candidate.provider)) {
              case pi::auth::AuthAvailability::configured:
                return std::string("configured");
              case pi::auth::AuthAvailability::not_required:
                return std::string("not_required");
              case pi::auth::AuthAvailability::missing:
                return std::string("missing");
              case pi::auth::AuthAvailability::expired_or_refresh_needed:
                return std::string("expired_or_refresh_needed");
              }
              return std::string("unknown");
            },
            renderer->owns_status_line());
        renderer->force_full_repaint();
        if (selected.cancelled || !selected.model)
          continue;
        spec = selected.model->provider + "/" + selected.model->id;
      }

      core::ModelSelection selection{.model = spec, .source = "cli"};
      const auto resolution = registry->resolve(selection);
      if (!resolution) {
        renderer->on_command_output("model switch failed: " + resolution.error);
        continue;
      }
      if (auth_resolver->availability(resolution.model->provider) ==
          pi::auth::AuthAvailability::missing) {
        renderer->on_command_output(
            "model switch failed: missing authentication for provider '" +
            resolution.model->provider + "'");
        continue;
      }
      try {
        const auto result = runtime.set_model(*resolution.model,
                                              agent.state().thinking_level());
        if (mailbox)
          mailbox->set_model(result.current.provider, result.current.id);
        configure_hooks();
        {
          std::scoped_lock lock(effective_context_mutex);
          effective_context.reset();
        }
        (void)update_terminal_ui();
        std::string message =
            "model: " + result.current.provider + "/" + result.current.id;
        if (result.warning)
          message += "\nwarning: " + *result.warning;
        renderer->on_command_output(std::move(message));
      } catch (const std::exception &error) {
        renderer->on_command_output("model switch failed: " +
                                    std::string(error.what()));
      }
      continue;
    }
    if (line == "/models" || line.starts_with("/models ")) {
      std::string filter = line.size() > 7 ? line.substr(7) : "";
      filter.erase(0, filter.find_first_not_of(" \t"));
      renderer->on_command_output(format_model_catalog(filter, registry));
      continue;
    }
    if (line == "/usage") {
      print_usage(last_turn, session, has_current_pricing());
      continue;
    }
    if (line.starts_with("/name ") || line == "/name") {
      std::string name = line.size() > 5 ? line.substr(5) : "";
      name.erase(0, name.find_first_not_of(" \t"));
      if (name.empty()) {
        std::cerr << "usage: /name <session name>\n";
      } else {
        store->set_name(current_session_id, name);
        current_session_name = name;
        std::cerr << "[session name: " << name << "]\n";
      }
      continue;
    }
    if (line == "/tree") {
      auto tree_opt = core::build_session_tree(*store, current_session_id);
      if (!tree_opt) {
        std::cerr << "no session tree available\n";
        continue;
      }
      auto tree_lines =
          core::format_session_tree(*tree_opt, current_session_id);

      std::size_t cursor = 0;
      for (std::size_t i = 0; i < tree_lines.size(); ++i) {
        if (tree_lines[i].session_id == current_session_id) {
          cursor = i;
          break;
        }
      }

      auto result = cli::run_tree_selector(tree_lines, current_session_id,
                                           cursor, renderer->owns_status_line());
      renderer->force_full_repaint();
      if (result.cancelled || result.selected_session_id == current_session_id)
        continue;

      auto loaded = store->load(result.selected_session_id);
      if (!loaded) {
        std::cerr << "error: session not found\n";
        continue;
      }
      current_session_id = result.selected_session_id;
      current_session_name = loaded->header.name;
      if (mailbox)
        mailbox->drop_queued_delivery();
      runtime.activate_session(*loaded);
      if (mailbox)
        agent.set_runtime_identity(
            mailbox->activate_root(current_session_id, current_session_name));
      if (runtime.last_warning())
        std::cerr << "warning: " << *runtime.last_warning() << "\n";
      configure_hooks();
      {
        std::scoped_lock lock(effective_context_mutex);
        effective_context.reset();
      }
      std::cerr << "[session: " << current_session_id;
      if (loaded->header.name)
        std::cerr << "  " << *loaded->header.name;
      std::cerr << "]\n";
      continue;
    }
    if (line == "/new") {
      core::SessionHeader fresh_hdr;
      fresh_hdr.id = core::generate_session_id();
      fresh_hdr.created = std::chrono::system_clock::to_time_t(
          std::chrono::system_clock::now());
      const auto current_model = agent.state().model();
      fresh_hdr.model = current_model.id;
      fresh_hdr.provider = current_model.provider;
      if (mailbox)
        mailbox->drop_queued_delivery();
      current_session_id = runtime.create_session(fresh_hdr);
      current_session_name.reset();
      if (mailbox)
        agent.set_runtime_identity(
            mailbox->activate_root(current_session_id, current_session_name));
      {
        std::scoped_lock lock(effective_context_mutex);
        effective_context.reset();
      }
      std::cerr << "[new session: " << current_session_id << "]\n";
      continue;
    }
    if (line == "/fork") {
      core::SessionHeader child_hdr;
      child_hdr.id = core::generate_session_id();
      child_hdr.created = std::chrono::system_clock::to_time_t(
          std::chrono::system_clock::now());
      const auto current_model = agent.state().model();
      child_hdr.model = current_model.id;
      child_hdr.provider = current_model.provider;
      child_hdr.parent_id = current_session_id;
      child_hdr.parent_offset = agent.state().messages().size();
      if (mailbox)
        mailbox->drop_queued_delivery();
      current_session_id = runtime.fork_session(child_hdr);
      current_session_name.reset();
      if (mailbox)
        agent.set_runtime_identity(
            mailbox->activate_root(current_session_id, current_session_name));
      {
        std::scoped_lock lock(effective_context_mutex);
        effective_context.reset();
      }
      std::cerr << "[fork: " << current_session_id << "]\n";
      continue;
    }

    // Slash command dispatch
    if (line[0] == '/' && hooks && hooks->on_command) {
      auto space = line.find(' ');
      std::string cmd = line.substr(
          1, space == std::string::npos ? std::string::npos : space - 1);
      std::string rest =
          space == std::string::npos ? "" : line.substr(space + 1);

      core::LuaContextSnapshot context_snapshot;
      context_snapshot.raw = agent.context_snapshot();
      {
        std::scoped_lock lock(effective_context_mutex);
        context_snapshot.effective = effective_context;
      }
      auto result = hooks->on_command(cmd, rest, context_snapshot.raw.messages,
                                      context_snapshot);
      if (result.handled) {
        if (result.truncate_to) {
          runtime.truncate_active_session(*result.truncate_to);
          std::scoped_lock lock(effective_context_mutex);
          effective_context.reset();
        }
        if (result.output)
          renderer->on_command_output(*result.output);
        if (result.prompt)
          accumulate(run_and_persist(*result.prompt));
        continue;
      }
    }

    accumulate(run_and_persist(line));
  }
  return 0;
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
