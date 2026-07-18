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
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <filesystem>
#include <fstream>
#include <unistd.h>

#include "cli/args.h"
#include "cli/config.h"
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
#include "core/env_api_keys.h"
#include "core/event_types.h"
#include "core/lua_tool.h"
#include "core/message_types.h"
#include "core/models.h"
#include "core/otel_init.h"
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

namespace pi {

namespace {

void print_version() { std::cout << "pi-cpp " PI_CPP_VERSION "\n"; }

struct HookRuntime {
  std::mutex mutex;
  std::shared_ptr<core::LuaHooks> hooks;
};

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

int cmd_list_models(const cli::Args &args) {
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

std::optional<core::Model> resolve_model(const cli::Args &args) {
  if (!args.model.empty()) {
    auto m = core::find_model(args.model, args.provider);
    if (m) {
      // Provider hint wins: user explicitly asked for it, so the id prefix
      // should be treated as part of the model id, not a separate provider
      // when the prefix alone doesn't resolve to a known base_url.
      if (!args.provider.empty() && m->base_url.empty()) {
        // find_model already tried hint+parsed; if still empty, force hint
        m->provider = args.provider;
        for (const auto &known : core::all_models()) {
          if (known.provider == args.provider) {
            m->base_url = known.base_url;
            m->api = known.api;
            break;
          }
        }
      }
      if (!args.base_url.empty())
        m->base_url = args.base_url;
      if (!args.provider.empty() && m->provider == "custom")
        m->provider = args.provider;
    }
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
  while (!content.empty() && (content.back() == '\n' || content.back() == '\r'))
    content.remove_suffix(1);

  std::vector<std::string_view> lines;
  std::size_t pos = 0;
  while (pos <= content.size()) {
    const std::size_t next = content.find('\n', pos);
    if (next == std::string_view::npos) {
      lines.emplace_back(content.substr(pos));
      break;
    }
    lines.emplace_back(content.substr(pos, next - pos));
    pos = next + 1;
  }
  if (lines.empty())
    lines.emplace_back();

  std::vector<std::string_view> visible;
  std::string omitted;
  if (lines.size() > 5) {
    visible.emplace_back(lines[0]);
    visible.emplace_back(lines[1]);
    omitted = "… +" + std::to_string(lines.size() - 4) + " lines omitted";
    visible.push_back(omitted);
    visible.push_back(lines[lines.size() - 2]);
    visible.push_back(lines[lines.size() - 1]);
  } else {
    visible = std::move(lines);
  }

  std::string out;
  for (std::size_t i = 0; i < visible.size(); ++i) {
    out += (i == 0) ? " -> " : "    ";
    out += visible[i];
    if (i + 1 < visible.size())
      out += '\n';
  }
  return out;
}

// A renderer adapter that adds verbose tool/usage output on top of any base
// renderer.
class VerboseRenderer final : public core::Renderer {
public:
  VerboseRenderer(core::Renderer &base, bool verbose,
                  std::shared_ptr<core::StreamDiagnostics> diagnostics)
      : base_(base), verbose_(verbose), diagnostics_(std::move(diagnostics)) {}

  void on_turn_start() override {
    if (diagnostics_)
      diagnostics_->record_renderer_event("turn_start");
    base_.on_turn_start();
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

  void on_tool_start(std::string_view call_id, std::string_view name,
                     std::string_view args_json) override {
    if (diagnostics_)
      diagnostics_->record_renderer_event("tool_start", args_json.size());
    base_.on_tool_start(call_id, name, args_json);
    std::cout << "\n[tool: " << name << "("
              << "\033[38;5;214m" << args_json << "\033[0m" << ")]\n"
              << std::flush;
  }
  void on_tool_end(std::string_view call_id, std::string_view name,
                   const core::ToolResult &result, bool is_error) override {
    if (diagnostics_)
      diagnostics_->record_renderer_event("tool_end", result.content().size());
    base_.on_tool_end(call_id, name, result, is_error);
    std::cout << "\033[38;5;245m" << "  [" << name << "] "
              << format_tool_result(result.content()) << "\033[0m\n"
              << std::flush;
  }

  void on_message_end(const core::TokenUsage &u) override {
    if (diagnostics_)
      diagnostics_->record_renderer_event("message_end");
    base_.on_message_end(u);
    if (verbose_) {
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

  void on_error(core::RendererErrorKind, std::string_view msg) override {
    if (diagnostics_)
      diagnostics_->record_renderer_event("error", msg.size());
    std::cerr << "\nerror: " << msg << "\n";
  }

  void on_scroll(core::RendererScrollCommand command) override {
    base_.on_scroll(command);
  }

  bool owns_status_line() const override { return base_.owns_status_line(); }

  void set_status_line(const std::optional<std::string> &text) override {
    base_.set_status_line(text);
  }

  const core::TokenUsage &last_usage() const { return last_usage_; }

private:
  core::Renderer &base_;
  bool verbose_;
  std::shared_ptr<core::StreamDiagnostics> diagnostics_;
  core::TokenUsage last_usage_;
};

core::TokenUsage
run_turn(core::AgentSession &session, const std::string &input,
         core::Renderer &renderer, bool verbose,
         std::shared_ptr<core::StreamDiagnostics> diagnostics) {
  VerboseRenderer vr(renderer, verbose, std::move(diagnostics));
  std::jthread interrupt_watcher([&session](const std::stop_token &stop_token) {
    while (!stop_token.stop_requested()) {
      if (core::consume_sigint()) {
        session.agent().interrupt(core::TurnAbortReason::user_interrupt);
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  });
  auto result = session.run_prompt(input, [&vr](const core::AgentEvent &event) {
    core::dispatch_event(event, vr);
  });
  interrupt_watcher.request_stop();
  if (result.error && !session.agent().state().error_message())
    renderer.on_error(core::RendererErrorKind::unknown, *result.error);
  return vr.last_usage();
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

int cmd_run(const cli::Args &args) {
  auto model_opt = resolve_model(args);
  if (!model_opt) {
    std::cerr << "error: could not resolve model\n";
    return 1;
  }
  core::Model model = *model_opt;
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
  auto auth_resolver = std::make_shared<pi::auth::AuthResolver>();
  opts.get_auth = [&args, &model, auth_resolver](
                      std::string_view p) -> std::optional<core::RequestAuth> {
    return auth_resolver->resolve(p.empty() ? model.provider : p, args.api_key);
  };
  opts.get_api_key =
      [&args, &model](std::string_view p) -> std::optional<std::string> {
    if (!args.api_key.empty())
      return args.api_key;
    return core::get_env_api_key(p.empty() ? model.provider : p);
  };
  opts.verbose = args.verbose;

  // Load and compose Lua hooks
  std::vector<std::shared_ptr<core::LuaHooks>> hooks_list_saved;
  auto load_hooks = [&]() -> std::shared_ptr<core::LuaHooks> {
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

    hooks_list_saved = hooks_list;
    return core::compose_hooks(std::move(hooks_list));
  };
  auto hooks = load_hooks();
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

  core::AgentSession runtime({.agent_options = opts,
                              .session_store = store,
                              .sandbox_policy = sandbox_policy});
  auto &agent = runtime.agent();

  if (!args.no_tools && !args.no_builtin_tools) {
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

  std::vector<std::string> tool_names;
  for (const auto &tool : agent.state().tools())
    tool_names.emplace_back(tool->name());
  const auto system = cli::build_system_prompt(
      args.system_prompt, args.append_system_prompts, context_files, tool_names,
      std::filesystem::current_path());
  agent.state().set_system_prompt(system);
  opts.system_prompt = system;

  const auto base_tools = agent.state().tools();

  // --list-tools / --list-addons (exit immediately after printing)
  if (args.list_tools) {
    print_tools(agent.state().tools());
    return 0;
  }
  if (args.list_addons) {
    print_addons(hooks_list_saved);
    return 0;
  }

  auto apply_hook_tools = [&]() {
    auto tools = base_tools;
    if (hooks)
      tools.insert(tools.end(), hooks->registered_tools.begin(),
                   hooks->registered_tools.end());
    agent.set_tools(std::move(tools));
  };

  auto task_manager = std::make_shared<core::AgentTaskManager>(runtime, opts);
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

    core::LuaHooks::AgentInfo info;
    info.model_id = model.id;
    info.model_provider = model.provider;
    info.model_api = model.api;
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

  apply_hook_tools();
  configure_hooks();

  std::string current_session_id;
  std::optional<std::string> current_session_name;

  auto reload_addons = [&]() {
    hooks = load_hooks();
    {
      std::scoped_lock lock(hook_runtime->mutex);
      hook_runtime->hooks = hooks;
    }
    apply_hook_tools();
    configure_hooks();
  };

  if (loaded_session) {
    current_session_id = loaded_session->header.id;
    current_session_name = loaded_session->header.name;
    runtime.activate_session(*loaded_session);
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
    hdr.model = model.id;
    hdr.provider = model.provider;
    hdr.sandbox_mode = std::string(core::sandbox_mode_to_string(sandbox_mode));
    current_session_id = runtime.create_session(hdr);
  }

  if (args.rpc_mode)
    return cli::run_rpc_mode(runtime, std::cin, std::cout, task_manager.get());

  auto renderer = make_renderer(args);

  if (sandbox_mode == core::SandboxMode::disabled)
    std::cerr << "[sandbox: disabled; bash runs without bubblewrap]\n";
  else if (args.verbose)
    std::cerr << "[sandbox: " << core::sandbox_mode_to_string(sandbox_mode)
              << "]\n";

  if (args.verbose) {
    std::cerr << "[model: " << model.provider << "/" << model.id << "]\n";
    std::cerr << "[tools: " << agent.state().tools().size() << "]\n";
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
            std::string_view("/reload-addons"), std::string_view("/usage"),
            std::string_view("/name"), std::string_view("/fork"),
            std::string_view("/tree")}) {
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
    core::LuaUiContext context;
    context.model = model.id;
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
  bool has_pricing =
      model.cost.input_per_mtok != 0 || model.cost.output_per_mtok != 0;

  auto accumulate = [&](const core::TokenUsage &u) {
    last_turn = CostAccumulator{};
    last_turn.add(u);
    session.add(u);
    last_usage_for_prompt = u;
    session_usage_for_prompt = build_session_usage();
  };

  // Run a turn and persist all new messages to the session file.
  auto run_and_persist = [&](const std::string &input) {
    return run_turn(runtime, input, *renderer, args.verbose,
                    stream_diagnostics);
  };

  auto update_terminal_ui = [&]() -> std::optional<std::string> {
    const auto context = build_ui_context();
    std::optional<std::string> status_line;
    if (hooks && hooks->status_line)
      status_line = hooks->status_line(context);
    renderer->set_status_line(status_line);
    if (hooks && hooks->tab_title) {
      if (auto title = hooks->tab_title(context))
        core::set_terminal_title(STDOUT_FILENO, *title);
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
          hooks->prompt_line(turns, model.id, agent.state().tools().size(),
                             last_usage_for_prompt, session_usage_for_prompt);
      if (custom)
        prompt = "\n" + *custom;
    }
    const auto status_line = update_terminal_ui();
    const std::string_view readline_status =
        renderer->owns_status_line() || !status_line ? std::string_view{}
                                                     : *status_line;
    auto maybe_line =
        cli::readline(prompt, complete_fn, control_fn, readline_status);
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
    if (line == "/usage") {
      print_usage(last_turn, session, has_pricing);
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

      auto result =
          cli::run_tree_selector(tree_lines, current_session_id, cursor);
      if (result.cancelled || result.selected_session_id == current_session_id)
        continue;

      auto loaded = store->load(result.selected_session_id);
      if (!loaded) {
        std::cerr << "error: session not found\n";
        continue;
      }
      current_session_id = result.selected_session_id;
      current_session_name = loaded->header.name;
      runtime.activate_session(*loaded);
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
      fresh_hdr.model = model.id;
      fresh_hdr.provider = model.provider;
      current_session_id = runtime.create_session(fresh_hdr);
      current_session_name.reset();
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
      child_hdr.model = model.id;
      child_hdr.provider = model.provider;
      child_hdr.parent_id = current_session_id;
      child_hdr.parent_offset = agent.state().messages().size();
      current_session_id = runtime.fork_session(child_hdr);
      current_session_name.reset();
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
