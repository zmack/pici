#pragma once

// The CLI's interactive-session machinery: CmdRunSession (the whole
// cmd_run() entry point -- bootstrap, dispatch, and the REPL loop) plus the
// renderer/turn/formatting helpers it uses. Header-only (every free
// function below is `inline`, and the two classes define their methods
// inline in the class body, as this codebase already does for
// similarly-shaped classes like VerboseRenderer) so it can be included by
// both src/main.cpp and test/test_cmd_run_repl.cpp without a separate
// translation unit -- the only two places that need it. See
// CmdRunSession's own comment below for why it exists as a class rather
// than staying a single free function.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <functional>
#include <iomanip>
#include <iostream>
#include <locale>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <sstream>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <filesystem>
#include <unistd.h>

#include "cli/args.h"
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
#include "core/auth/authentication.h"
#include "core/builtin_tools.h"
#include "core/compaction.h"
#include "core/event_types.h"
#include "core/lua_tool.h"
#include "core/mailbox/mailbox_bindings.h"
#include "core/mailbox/mailbox_coordinator.h"
#include "core/memory_stats.h"
#include "core/message_types.h"
#include "core/models.h"
#include "core/providers/faux_control.h"
#include "core/sandbox.h"
#include "core/session/session_id.h"
#include "core/session/session_record.h"
#include "core/session/session_runtime.h"
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

// Forward-declared so CmdRunSession can grant it friend access below; only
// test/test_cmd_run_repl.cpp actually defines this class.
class CmdRunSessionTest;

namespace pi {

using cli::HookRuntime;

inline std::string format_tools(
    const std::vector<std::shared_ptr<const core::ToolDefinition>> &tools) {
  if (tools.empty()) {
    return "(no tools loaded)\n";
  }
  std::ostringstream ss;
  for (const auto &t : tools) {
    ss << t->name() << "\n";
    if (!t->source_path().empty()) {
      ss << "  source: " << t->source_path() << "\n";
    }
    if (!t->description().empty()) {
      ss << "  " << t->description() << "\n";
    }
    ss << "\n";
  }
  return ss.str();
}

inline std::string format_skill_catalog(const core::SkillCatalog &catalog) {
  if (catalog.skills.empty()) {
    return "(no skills discovered)\n";
  }
  std::ostringstream ss;
  for (const auto &s : catalog.skills) {
    ss << s.name << "\n"
       << "  scope: " << s.scope << "\n"
       << "  path: " << s.path << "\n";
    if (!s.description.empty()) {
      ss << "  " << s.description << "\n";
    }
    ss << "\n";
  }
  if (!catalog.diagnostics.empty()) {
    ss << "diagnostics:\n";
    for (const auto &d : catalog.diagnostics) {
      ss << "  - " << d << "\n";
    }
  }
  return ss.str();
}

inline std::string
format_addons(const std::vector<std::shared_ptr<core::LuaHooks>> &hooks_list) {
  if (hooks_list.empty()) {
    return "(no add-ons loaded)\n";
  }
  std::ostringstream ss;
  for (const auto &h : hooks_list) {
    ss << (h->source_path.empty() ? "<composed>" : h->source_path) << "\n";
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
      ss << "  hooks:";
      for (const auto &a : active)
        ss << "  " << a;
      ss << "\n";
    }
    if (!h->commands.empty()) {
      for (const auto &cmd : h->commands) {
        ss << "  /" << cmd.name;
        if (!cmd.args_hint.empty())
          ss << " " << cmd.args_hint;
        if (!cmd.description.empty())
          ss << "  — " << cmd.description;
        ss << "\n";
      }
    }
  }
  return ss.str();
}

// Returns "$0.0023" for 0.002341928, "$1.23" for 1.234, "$12.34" for 12.345
inline std::string format_cost(double usd) {
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

inline std::string format_tokens(std::uint64_t n) {
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

inline std::string format_model_catalog(
    std::string_view filter,
    const std::shared_ptr<const core::ModelCatalog> &registry) {
  auto hits = registry->search_models(filter);
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

inline std::unique_ptr<core::Renderer> make_renderer(const cli::Args &args) {
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

inline std::string format_tool_result(std::string_view content) {
  return core::truncate_tool_result(content);
}

// A renderer adapter that adds verbose tool/usage output on top of any base
// renderer.

class VerboseRenderer final : public core::Renderer {
public:
  VerboseRenderer(core::Renderer &base, bool verbose,
                  std::shared_ptr<core::StreamDiagnostics> diagnostics,
                  std::shared_ptr<HookRuntime> hook_runtime = nullptr,
                  std::function<core::LuaUiContext()> context_builder = nullptr)
      : base_(base), verbose_(verbose), diagnostics_(std::move(diagnostics)),
        hook_runtime_(std::move(hook_runtime)),
        context_builder_(std::move(context_builder)) {}

  // Fired mid-turn whenever the provider reports new usage counts (input
  // tokens at message start, output tokens once the trailing usage update
  // arrives). Lets the status_line addon show live cost as a turn streams
  // instead of only once the whole turn (all messages + tool calls) ends —
  // see update_terminal_ui(), which covers the post-turn case.
  void on_usage_update(const core::TokenUsage &u) override {
    if (u.input == last_reported_usage_.input &&
        u.output == last_reported_usage_.output &&
        u.cache_read == last_reported_usage_.cache_read &&
        u.cache_write == last_reported_usage_.cache_write)
      return;
    last_reported_usage_ = u;
    if (!hook_runtime_ || !context_builder_)
      return;
    std::shared_ptr<core::LuaHooks> hooks;
    {
      std::scoped_lock lock(hook_runtime_->mutex);
      hooks = hook_runtime_->hooks;
    }
    if (!hooks || !hooks->status_line)
      return;
    auto ctx = context_builder_();
    ctx.last = u;
    base_.set_status_line(hooks->status_line(ctx));
  }

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

  void on_tool_call_streaming(std::size_t content_index,
                              std::string_view call_id,
                              std::string_view tool_name,
                              std::string_view partial_args_json) override {
    if (diagnostics_)
      diagnostics_->record_renderer_event("tool_call_streaming",
                                          partial_args_json.size());
    base_.on_tool_call_streaming(content_index, call_id, tool_name,
                                 partial_args_json);
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
  std::function<core::LuaUiContext()> context_builder_;
  std::unordered_map<std::string, nlohmann::json> pending_tool_args_;
  core::TokenUsage last_usage_;
  core::TokenUsage last_reported_usage_;
};

template <typename Invoke>

inline core::TokenUsage run_turn_impl(
    core::SessionRuntime &session, core::Renderer &renderer, bool verbose,
    std::shared_ptr<core::StreamDiagnostics> diagnostics,
    std::shared_ptr<HookRuntime> hook_runtime, Invoke &&invoke,
    std::function<core::LuaUiContext()> context_builder = nullptr,
    std::shared_ptr<core::SubagentActivityBridge> activity = nullptr) {
  VerboseRenderer vr(renderer, verbose, std::move(diagnostics),
                     std::move(hook_runtime), std::move(context_builder));
  std::jthread interrupt_watcher([&session](const std::stop_token &stop_token) {
    while (!stop_token.stop_requested()) {
      if (core::consume_sigint()) {
        session.cancel(core::TurnAbortReason::user_interrupt);
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  });
  auto result = std::forward<Invoke>(invoke)(
      [&vr, &activity](const core::AgentEvent &event) {
        core::dispatch_event(event, vr);
        if (activity)
          activity->drain(vr);
      });
  interrupt_watcher.request_stop();
  if (result.error && !session.agent().state().error_message())
    renderer.on_error(core::RendererErrorKind::unknown, *result.error);
  return vr.last_usage();
}

inline core::TokenUsage
run_turn(core::SessionRuntime &session, const std::string &input,
         core::Renderer &renderer, bool verbose,
         std::shared_ptr<core::StreamDiagnostics> diagnostics,
         std::shared_ptr<HookRuntime> hook_runtime = nullptr,
         std::function<core::LuaUiContext()> context_builder = nullptr,
         std::shared_ptr<core::SubagentActivityBridge> activity = nullptr) {
  return run_turn_impl(
      session, renderer, verbose, std::move(diagnostics),
      std::move(hook_runtime),
      [&session, &input](const auto &callback) {
        return session.run_prompt(input, callback);
      },
      std::move(context_builder), std::move(activity));
}

inline core::TokenUsage run_message_turn(
    core::SessionRuntime &session, std::vector<core::AgentInput> messages,
    core::Renderer &renderer, bool verbose,
    std::shared_ptr<core::StreamDiagnostics> diagnostics,
    std::shared_ptr<HookRuntime> hook_runtime = nullptr,
    std::function<core::LuaUiContext()> context_builder = nullptr,
    std::shared_ptr<core::SubagentActivityBridge> activity = nullptr) {
  return run_turn_impl(
      session, renderer, verbose, std::move(diagnostics),
      std::move(hook_runtime),
      [&session, messages = std::move(messages)](const auto &callback) mutable {
        return session.run_messages(std::move(messages), callback);
      },
      std::move(context_builder), std::move(activity));
}

// Drives SessionRuntime::compact_active_session to completion, reusing the
// exact interrupt-watcher pattern run_turn_impl uses for prompts: Ctrl-C is
// polled on a jthread and forwarded to Agent::interrupt(), which stops the
// shared stop_token a running compaction request observes. Unlike
// run_turn_impl, the result type here (CompactionRunResult) carries
// success/unsupported/cancelled/error directly, so callers do not need to
// re-derive status from renderer state.
inline core::SessionRuntime::CompactionRunResult
run_compaction_command(core::SessionRuntime &session, core::Renderer &renderer,
                       bool verbose,
                       std::shared_ptr<core::StreamDiagnostics> diagnostics,
                       std::shared_ptr<HookRuntime> hook_runtime,
                       core::CompactionTrigger trigger) {
  VerboseRenderer vr(renderer, verbose, std::move(diagnostics),
                     std::move(hook_runtime));
  std::jthread interrupt_watcher([&session](const std::stop_token &stop_token) {
    while (!stop_token.stop_requested()) {
      if (core::consume_sigint()) {
        session.cancel(core::TurnAbortReason::user_interrupt);
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

inline std::string format_usage(const CostAccumulator &last,
                                const CostAccumulator &session,
                                bool has_pricing) {
  std::ostringstream ss;
  auto row = [&](std::string_view label, const CostAccumulator &acc) {
    ss << std::left << std::setw(10) << label << "  in=" << std::setw(8)
       << format_tokens(acc.input_tokens) << "  out=" << std::setw(8)
       << format_tokens(acc.output_tokens) << "  cache_r=" << std::setw(8)
       << format_tokens(acc.cache_read_tokens) << "  cache_w=" << std::setw(8)
       << format_tokens(acc.cache_write_tokens);
    if (has_pricing)
      ss << "  " << format_cost(acc.total_cost);
    ss << "\n";
  };
  ss << "\n";
  row("last turn:", last);
  row("session:", session);
  ss << "  turns: " << session.turns;
  if (!has_pricing)
    ss << "  (cost unknown)";
  ss << "\n";
  return ss.str();
}

inline std::string format_memory_bytes(std::uint64_t bytes) {
  // Same shape as format_tokens: fixed width, human-scaled units.
  std::ostringstream ss;
  ss.imbue(std::locale::classic());
  if (bytes >= static_cast<std::uint64_t>(1024 * 1024))
    ss << std::fixed << std::setprecision(1)
       << static_cast<double>(bytes) / (1024.0 * 1024.0) << " MB";
  else if (bytes >= 1024)
    ss << std::fixed << std::setprecision(1)
       << static_cast<double>(bytes) / 1024.0 << " KB";
  else
    ss << bytes << " B";
  return ss.str();
}

inline std::string
format_ascii_table(const std::vector<std::string> &headers,
                   const std::vector<std::vector<std::string>> &rows,
                   const std::vector<bool> &right_aligned, int max_width = 0) {
  std::vector<std::size_t> widths(headers.size());
  for (std::size_t i = 0; i < headers.size(); ++i)
    widths[i] = headers[i].size();
  for (const auto &row : rows)
    for (std::size_t i = 0; i < widths.size() && i < row.size(); ++i)
      widths[i] = std::max(widths[i], row[i].size());

  // Keep the box intact instead of relying on the terminal to wrap long
  // rows.  The latter makes every subsequent row appear misaligned.  The
  // table overhead is three columns per cell plus the two border edges.
  if (max_width > 0 && !widths.empty()) {
    const auto overhead = 3 * widths.size() + 1;
    const auto available = max_width > static_cast<int>(overhead)
                               ? static_cast<std::size_t>(max_width) - overhead
                               : widths.size();
    while (std::accumulate(widths.begin(), widths.end(), std::size_t{0}) >
           available) {
      auto widest = std::max_element(widths.begin(), widths.end());
      if (*widest <= 1)
        break;
      --*widest;
    }
  }

  const auto fit = [](std::string value, std::size_t width) {
    if (value.size() <= width)
      return value;
    if (width <= 3)
      return value.substr(0, width);
    value.resize(width - 3);
    value += "...";
    return value;
  };

  std::ostringstream ss;
  const auto border = [&] {
    ss << '+';
    for (const auto width : widths)
      ss << std::string(width + 2, '-') << '+';
    ss << '\n';
  };
  const auto line = [&](const std::vector<std::string> &cells) {
    ss << '|';
    for (std::size_t i = 0; i < widths.size(); ++i) {
      const std::string value =
          fit(i < cells.size() ? cells[i] : std::string{}, widths[i]);
      ss << ' ';
      if (i < right_aligned.size() && right_aligned[i])
        ss << std::right << std::setw(static_cast<int>(widths[i])) << value;
      else
        ss << std::left << std::setw(static_cast<int>(widths[i])) << value;
      ss << '|';
    }
    ss << '\n';
  };

  border();
  line(headers);
  border();
  for (const auto &row : rows)
    line(row);
  border();
  return ss.str();
}

// Content-composition panel (§Design 3): escaped-JSON wire sizes split by
// content-block kind, for the root session plus every live child task.
inline std::string
format_memory_composition(const core::SessionRuntime &session,
                          const core::AgentTaskManager &tasks) {
  struct Row {
    std::string label;
    core::SessionCompositionReport report;
  };
  std::vector<Row> rows;
  rows.emplace_back("root", core::composition_report_for_messages(
                                session.agent().state().messages()));
  for (auto &[path, report] : tasks.composition_reports()) {
    if (path == "/root")
      continue; // root already reported from the live SessionRuntime above
    rows.emplace_back(path, report);
  }

  std::vector<std::vector<std::string>> table_rows;
  table_rows.reserve(rows.size());
  for (const auto &row : rows)
    table_rows.push_back({row.label,
                          format_memory_bytes(row.report.transcript_bytes),
                          format_memory_bytes(row.report.text_bytes),
                          format_memory_bytes(row.report.tool_result_bytes),
                          format_memory_bytes(row.report.tool_use_bytes),
                          format_memory_bytes(row.report.other_bytes)});
  std::ostringstream ss;
  ss << "Context composition (JSON wire bytes; estimated input size)\n";
  ss << format_ascii_table(
      {"session", "transcript", "text", "tool result", "tool use", "other"},
      table_rows, {false, true, true, true, true, true},
      core::term_width(STDOUT_FILENO));
  ss << '\n';
  return ss.str();
}

// Heap panel (§Design 4): real allocator bytes per session arena, plus the
// unattributed "shared" remainder and process-level totals. Requires
// jemalloc as the active global allocator; callers check
// memory_stats_available() first.
inline std::string format_memory_heap(const core::AgentTaskManager &tasks) {
  const auto heaps = tasks.heap_reports();
  if (heaps.empty())
    return {};
  const auto snapshot = core::read_process_snapshot();

  std::vector<std::vector<std::string>> table_rows;
  table_rows.reserve(heaps.size() + 3);
  std::ostringstream ss;
  ss << "Heap (live allocator bytes)\n";
  std::uint64_t arena_sum = 0;
  for (const auto &report : heaps) {
    std::string allocated = "n/a";
    if (report.arena) {
      arena_sum += report.arena->allocated_bytes;
      allocated = format_memory_bytes(report.arena->allocated_bytes);
    }
    table_rows.push_back({report.label, std::move(allocated)});
  }
  // §Design 4's internal-consistency identity: shared is DEFINED as
  // stats.allocated − Σ(session arenas); both are live-malloc-byte
  // quantities in the same units. Clamp tiny negative drift from reading
  // different arenas across separate epoch refreshes.
  std::string shared = "n/a";
  if (snapshot.allocator_allocated_bytes)
    shared = format_memory_bytes(snapshot.allocator_allocated_bytes > arena_sum
                                     ? *snapshot.allocator_allocated_bytes -
                                           arena_sum
                                     : 0);
  table_rows.push_back({"shared (unattributed)", std::move(shared)});
  if (snapshot.allocator_resident_bytes)
    table_rows.push_back(
        {"allocator resident",
         format_memory_bytes(*snapshot.allocator_resident_bytes)});
  table_rows.push_back(
      {"process RSS", format_memory_bytes(snapshot.rss_bytes)});
  ss << format_ascii_table({"session", "allocated"}, table_rows, {false, true},
                           core::term_width(STDOUT_FILENO));
  ss << '\n';
  return ss.str();
}

inline std::string format_memory(const core::SessionRuntime &session,
                                 const core::AgentTaskManager &tasks) {
  std::string out = "Memory\n======\n";
  if (core::memory_stats_available())
    out += format_memory_heap(tasks);
  else
    out += "Heap: unavailable (jemalloc is not the active allocator)\n"
           "  Enable with LD_PRELOAD=libjemalloc.so.2 or build with "
           "-DPI_CPP_MEMSTATS=ON -DPI_CPP_MEMSTATS_LINK_JEMALLOC=ON\n\n";
  out += format_memory_composition(session, tasks);
  return out;
}

inline nlohmann::json task_error_json(const core::AgentTaskError &error) {
  return nlohmann::json{
      {"error", {{"code", error.code()}, {"message", error.what()}}}};
}

inline nlohmann::json exception_json(const std::exception &error) {
  return nlohmann::json{
      {"error", {{"code", "internal"}, {"message", error.what()}}}};
}

inline nlohmann::json snapshot_json(const core::AgentTaskSnapshot &snapshot) {
  nlohmann::json value = {
      {"id", snapshot.id},
      {"task_path", snapshot.task_path},
      {"task_name", snapshot.task_name},
      {"model_provider", snapshot.model_provider},
      {"model", snapshot.model_id},
      {"status", core::agent_task_status_to_string(snapshot.status)},
      {"child_count", snapshot.child_count},
      {"queued_message_count", snapshot.queued_message_count},
      {"generation", snapshot.generation},
  };
  if (snapshot.parent_id)
    value["parent_id"] = *snapshot.parent_id;
  else
    value["parent_id"] = nullptr;
  if (snapshot.context_info) {
    const auto &info = *snapshot.context_info;
    nlohmann::json context = {
        {"message_count", info.message_count},
        {"context_bytes", info.context_bytes},
        {"last_input_tokens", info.last_input_tokens},
        {"last_output_tokens", info.last_output_tokens},
        {"total_tokens", info.total_tokens},
    };
    context["context_window"] = info.context_window
                                    ? nlohmann::json(*info.context_window)
                                    : nlohmann::json(nullptr);
    value["context"] = std::move(context);
  }
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
}

inline core::Message make_agent_message(const nlohmann::json &value) {
  core::UserMessage message;
  message.content.emplace_back(
      core::TextContent{.text = value.value("message", std::string{})});
  return core::Message{std::move(message)};
}

inline void parse_agent_context(const nlohmann::json &value,
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
}

// The CLI's whole interactive-session entry point: argument handling,
// renderer selection, REPL command dispatch, and the terminal I/O loop.
// Construction of the runtime itself (model/auth resolution, session
// store, sandbox policy, hooks, mailbox, task manager) is shared with
// pi-acp via cli::open_runtime_bundle()/activate_runtime_bundle(); see
// cli/session_runtime.h and plans/session-runtime-migration.md Phase 2.
//
// Orchestrates cmd_run(): model/session/tool/hook bootstrap, then dispatch
// into faux-control, RPC, or the interactive REPL. Extracted from a single
// 1176-line, CCN-233 free function (see test/test_cmd_run.cpp for the
// characterization tests taken before this refactor) into a class --
// mirroring RpcMode/FauxControlMode, the same "stateful command loop" shape
// this function already had -- so each bootstrap step and each REPL slash
// command is its own small, named method instead of one continuous function
// body. Behavior is unchanged; only structure.

class CmdRunSession {
public:
  CmdRunSession(cli::Args args,
                std::shared_ptr<const core::ModelCatalog> registry,
                std::shared_ptr<pi::auth::Authentication> authentication = {},
                std::shared_ptr<core::SessionStore> session_store = {})
      : args_(std::move(args)), registry_(std::move(registry)),
        injected_authentication_(std::move(authentication)),
        injected_session_store_(std::move(session_store)) {}

  int run() {
    if (!resolve_model())
      return 1;
    if (!init_diagnostics())
      return 1;
    init_authentication();
    if (!open_runtime())
      return 1;

    load_tools();
    build_system_prompt();

    // --list-tools / --list-addons (exit immediately after printing)
    if (args_.list_tools) {
      std::cout << format_tools(agent().state().tools());
      return 0;
    }
    if (args_.list_addons) {
      std::cout << format_addons(bundle_.hooks_list_saved);
      return 0;
    }

    activate_runtime();
    compat_counter_ = std::make_shared<std::atomic_uint64_t>(0);
    configure_hooks();

    resolve_or_create_session();
    if (!reapply_explicit_model_if_needed())
      return 1;
    activate_mailbox_root();
    configure_hooks();

    if (remote_client_)
      return run_faux_control();

    if (args_.rpc_mode)
      return cli::run_rpc_mode(runtime(), std::cin, std::cout,
                               runtime().task_manager().get(), authentication_);

    setup_interactive_session();
    build_completion_and_control_fns();
    if (const auto rc = run_initial_message())
      return *rc;
    return run_repl();
  }

private:
  // Grants test/test_cmd_run_repl.cpp's CmdRunSessionTest fixture direct
  // access to bootstrap_for_repl(), dispatch_line(), and the individual
  // handle_*_command() methods below, so REPL slash-command behavior can be
  // exercised with a mocked core::Renderer instead of a real terminal.
  friend class ::CmdRunSessionTest;

  core::SessionRuntime &runtime() { return *bundle_.runtime; }
  core::Agent &agent() { return runtime().agent(); }

  bool resolve_model() {
    effective_registry_ = registry_;
    if (args_.faux_control_socket.empty()) {
      const auto resolution = cli::resolve_model_selection(args_, registry_);
      if (!resolution) {
        std::cerr << "error: " << resolution.error << "\n";
        return false;
      }
      // resolution's operator bool() is defined as model.has_value(), so
      // the !resolution check above already guarantees model is set here.
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      model_ = *resolution.model;
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
      effective_registry_ = std::make_shared<const core::ModelCatalog>(
          std::map<std::string, core::ProviderConfig>{
              {"faux-control", provider}});
      model_.id = "faux-control";
      model_.name = "faux-control";
      model_.api = "faux-control";
      model_.provider = "faux-control";
      model_.base_url = "http://faux-control";
      model_.input_capabilities = {"text"};
      model_.context_window = 128000;
      model_.max_tokens = 4096;
      remote_client_ = std::make_shared<core::RemoteFauxClient>();
      scripted_registry_ = std::make_shared<core::ScriptedToolRegistry>();
      core::LLMClientRegistry::instance().register_client(
          "faux-control", [client = remote_client_] { return client; });
    }
    if (model_.provider == "openai-codex" && !args_.api_key.empty()) {
      std::cerr << "error: --api-key cannot be used with openai-codex; run "
                   "pi-cli auth login openai-codex\n";
      return false;
    }
    return true;
  }

  bool init_diagnostics() {
    if (args_.stream_trace.empty())
      return true;
    try {
      stream_diagnostics_ =
          std::make_shared<core::StreamDiagnostics>(args_.stream_trace);
    } catch (const std::exception &e) {
      std::cerr << "error: " << e.what() << "\n";
      return false;
    }
    return true;
  }

  void init_authentication() {
    // injected_authentication_ (when supplied, e.g. by PiciProcess) is only
    // valid for effective_registry_ == registry_: the faux-control-socket
    // branch of resolve_model() replaces effective_registry_ with a
    // one-off scripted catalog the injected Authentication was never built
    // against, so that path keeps building its own exactly as before.
    if (injected_authentication_ && effective_registry_ == registry_) {
      authentication_ = injected_authentication_;
    } else {
      authentication_ =
          std::make_shared<pi::auth::Authentication>(effective_registry_);
    }
    if (!args_.api_key.empty())
      authentication_->set_runtime_api_key(model_.provider, args_.api_key);
  }

  bool open_runtime() {
    cli::RuntimeBuildConfig runtime_config{
        .args = args_,
        .model = model_,
        .model_catalog = effective_registry_,
        .authentication = authentication_,
        .diagnostics = stream_diagnostics_,
        .on_effective_context =
            [this](const core::AgentContext &context) {
              std::scoped_lock lock(effective_context_mutex_);
              effective_context_ = context;
            },
        .session_store = injected_session_store_,
    };
    bundle_ = cli::open_runtime_bundle(runtime_config);
    if (bundle_.error) {
      std::cerr << "error: " << *bundle_.error << "\n";
      return false;
    }
    if (bundle_.warning)
      std::cerr << "warning: " << *bundle_.warning << "\n";
    sandbox_mode_ = bundle_.sandbox_policy->mode();
    return true;
  }

  void apply_hook_tools() {
    auto tools = base_tools_;
    if (bundle_.hooks)
      tools.insert(tools.end(), bundle_.hooks->registered_tools.begin(),
                   bundle_.hooks->registered_tools.end());
    agent().set_tools(std::move(tools));
  }

  void load_tools() {
    if (scripted_registry_) {
      faux_tool_registrar_ = [this](const std::string &name) {
        agent().add_tool(
            std::make_shared<core::ScriptedTool>(name, scripted_registry_));
      };
    }

    if (!args_.no_tools && !args_.no_builtin_tools &&
        args_.faux_control_socket.empty()) {
      if (args_.tools.empty()) {
        agent().set_tools(core::create_all_tools(
            std::filesystem::current_path(), bundle_.sandbox_policy,
            bundle_.skill_catalog));
      } else {
        // Allowlist filter
        for (auto &t : core::create_all_tools(std::filesystem::current_path(),
                                              bundle_.sandbox_policy,
                                              bundle_.skill_catalog)) {
          for (const auto &name : args_.tools) {
            if (t->name() == name) {
              agent().add_tool(t);
              break;
            }
          }
        }
      }
    }

    if (!args_.no_tools && !args_.tools_dir.empty() &&
        args_.faux_control_socket.empty()) {
      for (auto &t : core::load_lua_tools(args_.tools_dir)) {
        if (args_.tools.empty()) {
          agent().add_tool(t);
        } else {
          for (const auto &name : args_.tools)
            if (t->name() == name) {
              agent().add_tool(t);
              break;
            }
        }
      }
    }

    base_tools_ = agent().state().tools();
    if (args_.faux_control_socket.empty())
      apply_hook_tools();
  }

  void build_system_prompt() {
    std::vector<std::string> tool_names;
    for (const auto &tool : agent().state().tools())
      tool_names.emplace_back(tool->name());
    const auto system = cli::build_system_prompt(
        args_.system_prompt, args_.append_system_prompts, bundle_.context_files,
        tool_names, std::filesystem::current_path(), bundle_.skill_catalog_ptr);
    agent().state().set_system_prompt(system);
    bundle_.agent_options.system_prompt = system;
  }

  void activate_runtime() {
    repl_wake_ = std::make_shared<cli::ReadlineWake>();
    activity_ = std::make_shared<core::SubagentActivityBridge>(
        [wake = repl_wake_] { static_cast<void>(wake->notify()); });
    cli::activate_runtime_bundle(
        bundle_,
        [activity = activity_](const core::AgentTaskEvent &event) {
          activity->observe(event);
        },
        [wake = repl_wake_] { static_cast<void>(wake->notify()); });
  }

  // Recomputes hooks->configure()'s AgentInfo (model, tools, mailbox
  // bindings, sub-agent spawn/get/list/send/interrupt/wait/close bindings)
  // and invokes it. Called once after bootstrap and again whenever the
  // model or active session changes.
  // The bind_*() methods below each wire one core::LuaHooks::AgentInfo
  // field; split out of configure_hooks() purely to shrink that function's
  // branch count, with no behavior change.

  void
  bind_run_agent(core::LuaHooks::AgentInfo &info,
                 const std::shared_ptr<core::AgentTaskManager> &task_manager) {
    info.run_agent = [task_manager,
                      this](const core::LuaHooks::AgentRunConfig &cfg)
        -> core::LuaHooks::AgentRunResult {
      core::LuaHooks::AgentRunResult result;
      try {
        core::SpawnAgentRequest request;
        request.task_name =
            "compat_" + std::to_string(compat_counter_->fetch_add(1) + 1);
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
  }

  static void bind_agent_spawn(
      core::LuaHooks::AgentInfo &info,
      const std::shared_ptr<core::AgentTaskManager> &task_manager) {
    info.agents.spawn = [task_manager](const nlohmann::json &v) {
      try {
        core::SpawnAgentRequest request;
        request.parent_id = v.value("parent_id", std::string{});
        request.task_name = v.value("task_name", std::string{});
        request.prompt = v.value("message", v.value("prompt", std::string{}));
        if (v.contains("context"))
          parse_agent_context(v.at("context"), request.context);
        if (v.contains("system_prompt") && !v.at("system_prompt").is_null())
          request.system_prompt = v.at("system_prompt").get<std::string>();
        if (v.contains("model") && !v.at("model").is_null())
          request.model_spec = v.at("model").get<std::string>();
        if (v.contains("tools"))
          request.requested_tools =
              v.at("tools").get<std::vector<std::string>>();
        request.allow_subagents = v.value("allow_subagents", false);
        request.allow_write_tools = v.value("allow_write_tools", false);
        return snapshot_json(task_manager->spawn(request));
      } catch (const core::AgentTaskError &error) {
        return task_error_json(error);
      } catch (const std::exception &error) {
        return exception_json(error);
      }
    };
  }

  static void
  bind_agent_get(core::LuaHooks::AgentInfo &info,
                 const std::shared_ptr<core::AgentTaskManager> &task_manager) {
    info.agents.get = [task_manager](const nlohmann::json &v) {
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
  }

  static void
  bind_agent_list(core::LuaHooks::AgentInfo &info,
                  const std::shared_ptr<core::AgentTaskManager> &task_manager) {
    info.agents.list = [task_manager](const nlohmann::json &v) {
      nlohmann::json values = nlohmann::json::array();
      const auto prefix = v.value("path_prefix", std::string{});
      for (const auto &snapshot : task_manager->list(
               prefix.empty() ? std::optional<std::string_view>{}
                              : std::optional<std::string_view>{prefix}))
        values.push_back(snapshot_json(snapshot));
      return values;
    };
  }

  static void bind_agent_queue(
      core::LuaHooks::AgentInfo &info,
      const std::shared_ptr<core::AgentTaskManager> &task_manager) {
    auto queue_binding = [task_manager](const nlohmann::json &v,
                                        bool follow_up) {
      try {
        const auto target = v.value("target", std::string{});
        const auto message = make_agent_message(v);
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
  }

  static void bind_agent_interrupt(
      core::LuaHooks::AgentInfo &info,
      const std::shared_ptr<core::AgentTaskManager> &task_manager) {
    info.agents.interrupt = [task_manager](const nlohmann::json &v) {
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
  }

  static void
  bind_agent_wait(core::LuaHooks::AgentInfo &info,
                  const std::shared_ptr<core::AgentTaskManager> &task_manager) {
    info.agents.wait = [task_manager](const nlohmann::json &v) {
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
  }

  static void bind_agent_close(
      core::LuaHooks::AgentInfo &info,
      const std::shared_ptr<core::AgentTaskManager> &task_manager) {
    info.agents.close = [task_manager](const nlohmann::json &v) {
      try {
        return snapshot_json(
            task_manager->close(v.value("target", std::string{})));
      } catch (const core::AgentTaskError &error) {
        return task_error_json(error);
      } catch (const std::exception &error) {
        return exception_json(error);
      }
    };
  }

  void configure_hooks() {
    if (!bundle_.hooks || !bundle_.hooks->configure)
      return;

    auto task_manager = runtime().task_manager();
    auto mailbox = runtime().mailbox_runtime().coordinator();

    std::vector<std::string> tool_names;
    for (const auto &t : agent().state().tools())
      tool_names.emplace_back(t->name());

    std::filesystem::path storage_path;
    if (!args_.hooks_files.empty())
      storage_path = std::filesystem::path(args_.hooks_files[0]).string() +
                     ".storage.json";

    const auto current_model = agent().state().model();
    core::LuaHooks::AgentInfo info;
    info.model_id = current_model.id;
    info.model_provider = current_model.provider;
    info.model_api = current_model.api;
    info.tool_names = std::move(tool_names);
    info.cwd = std::filesystem::current_path().string();
    info.storage_path = std::move(storage_path);
    info.mailbox = core::make_mailbox_bindings(mailbox);

    bind_run_agent(info, task_manager);
    bind_agent_spawn(info, task_manager);
    bind_agent_get(info, task_manager);
    bind_agent_list(info, task_manager);
    bind_agent_queue(info, task_manager);
    bind_agent_interrupt(info, task_manager);
    bind_agent_wait(info, task_manager);
    bind_agent_close(info, task_manager);

    bundle_.hooks->configure(info);
  }

  void reload_addons() {
    auto mailbox = runtime().mailbox_runtime().coordinator();
    bundle_.hooks = cli::reload_hooks(args_, static_cast<bool>(mailbox),
                                      bundle_.hooks_list_saved);
    {
      std::scoped_lock lock(bundle_.hook_runtime->mutex);
      bundle_.hook_runtime->hooks = bundle_.hooks;
    }
    if (args_.faux_control_socket.empty())
      apply_hook_tools();
    configure_hooks();
  }

  void resolve_or_create_session() {
    if (bundle_.loaded_session) {
      current_session_id_ = bundle_.loaded_session->header.id;
      current_session_name_ = bundle_.loaded_session->header.name;
      runtime().activate_session(*bundle_.loaded_session);
      if (const auto &warning = runtime().last_warning())
        std::cerr << "warning: " << *warning << "\n";
      if (args_.sandbox_mode_explicit)
        runtime().set_sandbox_mode(sandbox_mode_);
      std::cerr << "[session: " << current_session_id_;
      if (bundle_.loaded_session->header.name)
        std::cerr << "  " << *bundle_.loaded_session->header.name;
      std::cerr << "]\n";
    } else {
      core::SessionHeader hdr;
      hdr.id = core::generate_session_id();
      hdr.created = std::chrono::system_clock::to_time_t(
          std::chrono::system_clock::now());
      const auto current_model = agent().state().model();
      hdr.model = current_model.id;
      hdr.provider = current_model.provider;
      hdr.sandbox_mode =
          std::string(core::sandbox_mode_to_string(sandbox_mode_));
      current_session_id_ = runtime().create_session(hdr);
    }
  }

  bool reapply_explicit_model_if_needed() {
    if (!bundle_.loaded_session ||
        !(args_.model_explicit || args_.provider_explicit ||
          args_.base_url_explicit))
      return true;
    try {
      const auto result =
          runtime().set_model(model_, cli::to_core_thinking(args_.thinking));
      if (result.warning)
        std::cerr << "warning: " << *result.warning << "\n";
    } catch (const std::exception &error) {
      std::cerr << "error: unable to apply explicit model selection: "
                << error.what() << "\n";
      return false;
    }
    return true;
  }

  void activate_mailbox_root() {
    auto &mailbox_runtime = runtime().mailbox_runtime();
    if (const auto identity = mailbox_runtime.activate_root(
            current_session_id_, current_session_name_)) {
      agent().set_runtime_identity(identity);
      const auto current_model = agent().state().model();
      mailbox_runtime.set_model(current_model.provider, current_model.id);
    }
  }

  int run_faux_control() {
    auto faux_renderer = make_renderer(args_);
    VerboseRenderer renderer_adapter(*faux_renderer, args_.verbose,
                                     stream_diagnostics_, bundle_.hook_runtime);
    return cli::run_faux_control_socket(
        runtime(), *remote_client_, scripted_registry_,
        args_.faux_control_socket, faux_tool_registrar_,
        [&renderer_adapter](const core::AgentEvent &event) {
          core::dispatch_event(event, renderer_adapter);
        });
  }

  // Test-only entry point for CmdRunSessionTest: runs the same bootstrap
  // sequence as run() (model/session/tool/hook setup through session
  // creation/activation), but stops before dispatching into faux-control,
  // RPC, or the blocking REPL loop -- letting a test call dispatch_line()
  // directly against a real (temp-directory-isolated) SessionRuntime with
  // a mocked Renderer, instead of a real terminal. Must be kept in sync
  // with run()'s bootstrap steps; a test_renderer of nullptr falls back to
  // make_renderer(args_), matching setup_interactive_session()'s default.
  bool
  bootstrap_for_repl(std::unique_ptr<core::Renderer> test_renderer = nullptr) {
    if (!resolve_model())
      return false;
    if (!init_diagnostics())
      return false;
    init_authentication();
    if (!open_runtime())
      return false;

    load_tools();
    build_system_prompt();

    activate_runtime();
    compat_counter_ = std::make_shared<std::atomic_uint64_t>(0);
    configure_hooks();

    resolve_or_create_session();
    if (!reapply_explicit_model_if_needed())
      return false;
    activate_mailbox_root();
    configure_hooks();

    title_controller_.emplace(STDOUT_FILENO, "test");
    renderer_ = test_renderer ? std::move(test_renderer) : make_renderer(args_);
    subagent_panel_.emplace(*activity_);
    panel_mounted_ = renderer_->owns_subagent_pane();
    subagent_panel_->mount(*renderer_);

    build_completion_and_control_fns();
    return true;
  }

  void setup_interactive_session() {
    std::string startup_cwd;
    try {
      startup_cwd = std::filesystem::current_path().string();
    } catch (...) {
      startup_cwd = "";
    }
    const std::string initial_project_label =
        core::terminal_project_label(startup_cwd);
    title_controller_.emplace(STDOUT_FILENO, initial_project_label);

    renderer_ = make_renderer(args_);
    subagent_panel_.emplace(*activity_);
    panel_mounted_ = renderer_->owns_subagent_pane();
    subagent_panel_->mount(*renderer_);

    // §Design 2: bind the root session's arena on this (main) thread before
    // the interactive loop; everything the root session allocates from here
    // on lands in "root" rather than "shared". No-op without jemalloc.
    runtime().task_manager()->bind_root_arena();

    if (sandbox_mode_ == core::SandboxMode::disabled)
      std::cerr << "[sandbox: disabled; bash runs without bubblewrap]\n";
    else if (args_.verbose)
      std::cerr << "[sandbox: " << core::sandbox_mode_to_string(sandbox_mode_)
                << "]\n";

    if (args_.verbose) {
      const auto current_model = agent().state().model();
      std::cerr << "[model: " << current_model.provider << "/"
                << current_model.id << "]\n";
      std::cerr << "[tools: " << agent().state().tools().size() << "]\n";
    }
  }

  // Build completion function.
  // Command-name completion (/... with no space) is handled here from the
  // declared commands list — no Lua needed.  Argument completion (/cmd ...
  // with a space) is delegated to hooks->complete.
  void build_completion_and_control_fns() {
    complete_fn_ =
        [this](std::string_view partial) -> std::vector<std::string> {
      std::vector<std::string> result;
      const bool is_slash = !partial.empty() && partial[0] == '/';
      const bool has_space = partial.contains(' ');

      if (is_slash && !has_space) {
        // Complete command names: builtins + declared add-on commands
        for (std::string_view b :
             {std::string_view("/exit"), std::string_view("/quit"),
              std::string_view("/tools"), std::string_view("/addons"),
              std::string_view("/reload-addons"), std::string_view("/usage"),
              std::string_view("/memory"), std::string_view("/model"),
              std::string_view("/models"), std::string_view("/name"),
              std::string_view("/fork"), std::string_view("/tree"),
              std::string_view("/compact"), std::string_view("/skills")}) {
          if (b.starts_with(partial))
            result.emplace_back(b);
        }
        if (bundle_.hooks) {
          for (const auto &cmd : bundle_.hooks->commands) {
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
          for (const auto &candidate : registry_->models()) {
            const std::string canonical =
                candidate.provider + "/" + candidate.id;
            if (std::string_view(canonical).starts_with(prefix))
              result.push_back(canonical);
          }
          return result;
        }
      }

      // Argument completion — delegate to hook
      if (bundle_.hooks && bundle_.hooks->complete)
        return bundle_.hooks->complete(partial, agent().state().messages());
      return {};
    };

    control_fn_ = [this](cli::ControlAction action) {
      switch (action) {
      case cli::ControlAction::scroll_line_up:
        renderer_->on_scroll(core::RendererScrollCommand::line_up);
        break;
      case cli::ControlAction::scroll_line_down:
        renderer_->on_scroll(core::RendererScrollCommand::line_down);
        break;
      case cli::ControlAction::scroll_page_up:
        renderer_->on_scroll(core::RendererScrollCommand::page_up);
        break;
      case cli::ControlAction::scroll_page_down:
        renderer_->on_scroll(core::RendererScrollCommand::page_down);
        break;
      case cli::ControlAction::scroll_top:
        renderer_->on_scroll(core::RendererScrollCommand::top);
        break;
      case cli::ControlAction::scroll_bottom:
        renderer_->on_scroll(core::RendererScrollCommand::bottom);
        break;
      }
    };
  }

  core::TokenUsage build_session_usage() const {
    core::TokenUsage u;
    u.input = session_cost_.input_tokens;
    u.output = session_cost_.output_tokens;
    u.cache_read = session_cost_.cache_read_tokens;
    u.cache_write = session_cost_.cache_write_tokens;
    u.total_tokens = session_cost_.total_tokens;
    u.cost.total = session_cost_.total_cost;
    return u;
  }

  core::LuaUiContext build_ui_context() {
    const auto current_model = agent().state().model();
    core::LuaUiContext context;
    context.model = current_model.id;
    context.tools = agent().state().tools().size();
    context.last = last_usage_for_prompt_;
    context.session = session_usage_for_prompt_;
    context.session_id = current_session_id_;
    context.session_name = current_session_name_;
    for (const auto &message : agent().state().messages()) {
      if (std::holds_alternative<core::AssistantMessage>(message))
        ++context.turn;
    }
    return context;
  }

  bool has_current_pricing() {
    const auto current_model = agent().state().model();
    return current_model.cost.input_per_mtok != 0 ||
           current_model.cost.output_per_mtok != 0;
  }

  void accumulate(const core::TokenUsage &u) {
    last_turn_ = CostAccumulator{};
    last_turn_.add(u);
    session_cost_.add(u);
    last_usage_for_prompt_ = u;
    session_usage_for_prompt_ = build_session_usage();
  }

  // Run a turn and persist all new messages to the session file.
  core::TokenUsage run_and_persist(const std::string &input) {
    // title_controller_ is always populated before this method is reachable
    // -- by setup_interactive_session() on the real run() path, or
    // bootstrap_for_repl() in tests -- so this is safe despite not being
    // locally checked.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    core::TerminalTitleActivityGuard title_activity(*title_controller_);
    auto mailbox_turn = runtime().mailbox_runtime().begin_root_turn();
    runtime().mailbox_runtime().pump_inbox();
    auto result = run_turn(
        runtime(), input, *renderer_, args_.verbose, stream_diagnostics_,
        bundle_.hook_runtime, [this] { return build_ui_context(); },
        (isatty(STDIN_FILENO) != 0 && !args_.print_mode && !panel_mounted_)
            ? activity_
            : nullptr);
    return result;
  }

  void run_idle_mailbox_turns() {
    constexpr std::size_t kAutonomousBatchLimit = 16;
    auto &mailbox_runtime = runtime().mailbox_runtime();
    if (!mailbox_runtime.enabled() || autonomous_budget_exhausted_)
      return;
    while (autonomous_budget_.can_run()) {
      std::vector<core::AgentInput> messages;
      try {
        messages = mailbox_runtime.claim_idle_root_turn(kAutonomousBatchLimit);
      } catch (const std::exception &error) {
        if (args_.verbose)
          std::cerr << "[mailbox autonomous turn unavailable: " << error.what()
                    << "]\n";
        return;
      }
      if (messages.empty())
        return;
      {
        // See run_and_persist()'s comment: title_controller_ is always set
        // by this point.
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        core::TerminalTitleActivityGuard title_activity(*title_controller_);
        auto mailbox_turn = mailbox_runtime.adopt_root_turn();
        accumulate(run_message_turn(
            runtime(), std::move(messages), *renderer_, args_.verbose,
            stream_diagnostics_, bundle_.hook_runtime,
            [this] { return build_ui_context(); },
            (isatty(STDIN_FILENO) != 0 && !args_.print_mode && !panel_mounted_)
                ? activity_
                : nullptr));
      }
      autonomous_budget_.record();
    }
    autonomous_budget_exhausted_ = autonomous_budget_.exhausted();
  }

  std::optional<std::string> update_terminal_ui() {
    const auto context = build_ui_context();
    std::optional<std::string> status_line;
    if (bundle_.hooks && bundle_.hooks->status_line)
      status_line = bundle_.hooks->status_line(context);
    if (autonomous_budget_exhausted_) {
      constexpr std::string_view paused =
          "mailbox autonomous turns paused; submit input to resume";
      if (status_line)
        *status_line += " | " + std::string(paused);
      else
        status_line = std::string(paused);
    }
    const auto subagent_summary = activity_->summary();
    if (!subagent_summary.empty()) {
      if (status_line)
        *status_line += " | " + subagent_summary;
      else
        status_line = subagent_summary;
    }
    renderer_->set_status_line(status_line);
    if (bundle_.hooks && bundle_.hooks->tab_title) {
      if (auto title = bundle_.hooks->tab_title(context))
        // See run_and_persist()'s comment: title_controller_ is always set
        // by this point.
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        title_controller_->set_base_title(*title);
    }
    return status_line;
  }

  // Print mode / initial message. Returns an exit code if cmd_run() should
  // return immediately (print mode always does, with or without a prompt);
  // nullopt to continue into the interactive REPL.
  std::optional<int> run_initial_message() {
    if (!args_.print_mode && args_.messages.empty())
      return std::nullopt;
    std::string prompt;
    for (const auto &msg : args_.messages) {
      if (!prompt.empty())
        prompt += '\n';
      prompt += msg;
    }
    if (!prompt.empty()) {
      accumulate(run_and_persist(prompt));
      // A full-screen renderer (currently only --render region) owns its
      // own fixed alt-screen layout, cursor anchoring, and reserved
      // composer rows — an untracked write straight through std::cout
      // bypasses all of that bookkeeping and desyncs the very first
      // interactive prompt's on-screen position from what position_prompt_
      // cursor() assumes, which used to surface as a doubled footer hint
      // starting with the session's second prompt.
      if (!renderer_->owns_status_line())
        std::cout << "\n";
    }
    if (args_.print_mode)
      return 0;
    return std::nullopt;
  }

  int run_repl() {
    // A full-screen renderer (RegionRenderer) owns a persistent alt-screen
    // compositor for the whole session, not just while readline() is
    // blocking. readline() normally enters/leaves raw mode around each
    // call, which left the terminal in cooked/echo mode for the whole span
    // of a turn (no readline() call in flight) -- long enough for real
    // turns that keystrokes typed then got echoed by the tty driver
    // straight into the compositor's fixed layout, then silently dropped
    // when the next readline() call re-entered raw mode (TCSAFLUSH
    // discards unread input on a termios switch). Owning raw mode here for
    // the whole session instead means those keystrokes stay queued,
    // unechoed, in the kernel's raw input buffer and simply show up as
    // type-ahead once readline() resumes.
    owns_full_screen_ = renderer_->owns_status_line();
    if (owns_full_screen_)
      session_raw_mode_.enter(STDIN_FILENO);
    while (true) {
      // Build the prompt — let add-ons customise it. The chevron uses the
      // same bold-cyan accent as the region renderer's REQUEST heading so
      // the "this is user input" color reads consistently end to end;
      // \033[22;39m clears only weight/foreground so the input box's
      // background tint survives.
      std::string prompt = "\n\033[1;36m›\033[22;39m ";
      if (bundle_.hooks && bundle_.hooks->prompt_line) {
        const auto &msgs = agent().state().messages();
        std::size_t turns = 0;
        for (const auto &m : msgs)
          if (std::holds_alternative<core::AssistantMessage>(m))
            ++turns;
        auto custom = bundle_.hooks->prompt_line(
            turns, agent().state().model().id, agent().state().tools().size(),
            last_usage_for_prompt_, session_usage_for_prompt_);
        if (custom)
          prompt = "\n" + *custom;
      }
      const auto status_line = update_terminal_ui();
      const std::string_view readline_status =
          renderer_->owns_status_line() || !status_line ? std::string_view{}
                                                        : *status_line;
      const int readline_wake_fd = repl_wake_ && !autonomous_budget_exhausted_
                                       ? repl_wake_->read_fd()
                                       : -1;
      // Full-screen renderers (currently only --render region) echo the
      // submitted request in their own scrolling history, so the readline
      // box should empty immediately on submit instead of leaving the
      // typed text on screen — duplicated — for the whole turn. Such
      // renderers also own a fixed alt-screen layout that needs repainting
      // on a terminal resize.
      const bool full_screen_prompt = renderer_->owns_status_line();
      const auto on_prompt_resize = [this] { renderer_->on_resize(); };
      renderer_->prepare_for_prompt();
      auto readline_result =
          cli::readline(prompt, complete_fn_, control_fn_, readline_status,
                        readline_draft_, readline_cursor_, readline_wake_fd,
                        full_screen_prompt, on_prompt_resize, args_.vim_mode,
                        owns_full_screen_ ? &session_raw_mode_ : nullptr);
      if (readline_result.reason == cli::ReadlineExit::eof)
        break;
      if (readline_result.reason == cli::ReadlineExit::mailbox_wake) {
        readline_draft_ = std::move(readline_result.text);
        readline_cursor_ = readline_result.cursor;
        activity_->drain(*renderer_);
        run_idle_mailbox_turns();
        continue;
      }
      if (autonomous_budget_exhausted_ && repl_wake_)
        repl_wake_->drain();
      activity_->drain(*renderer_);
      autonomous_budget_.reset();
      autonomous_budget_exhausted_ = false;
      readline_draft_.clear();
      readline_cursor_ = 0;
      const std::string &line = readline_result.text;
      if (line.empty())
        continue;
      if (dispatch_line(line))
        break;
    }
    return 0;
  }

  // Returns true iff the REPL should exit (the user typed /exit or /quit).
  bool dispatch_line(const std::string &line) {
    if (line == "/exit" || line == "/quit")
      return true;
    if (line == "/tools") {
      handle_tools_command();
      return false;
    }
    if (line == "/skills") {
      handle_skills_command();
      return false;
    }
    if (line == "/addons") {
      handle_addons_command();
      return false;
    }
    if (line == "/reload-addons") {
      handle_reload_addons_command();
      return false;
    }
    if (line == "/compact") {
      handle_compact_command();
      return false;
    }
    if (line == "/model" || line.starts_with("/model ")) {
      handle_model_command(line);
      return false;
    }
    if (line == "/models" || line.starts_with("/models ")) {
      handle_models_command(line);
      return false;
    }
    if (line == "/usage") {
      handle_usage_command();
      return false;
    }
    if (line == "/memory") {
      handle_memory_command();
      return false;
    }
    if (line.starts_with("/name ") || line == "/name") {
      handle_name_command(line);
      return false;
    }
    if (line == "/tree") {
      handle_tree_command();
      return false;
    }
    if (line == "/new") {
      handle_new_command();
      return false;
    }
    if (line == "/fork") {
      handle_fork_command();
      return false;
    }

    // Slash command dispatch
    if (line[0] == '/' && bundle_.hooks && bundle_.hooks->on_command) {
      if (handle_hook_command(line))
        return false;
    }

    accumulate(run_and_persist(line));
    return false;
  }

  void handle_tools_command() {
    renderer_->on_command_output(format_tools(agent().state().tools()));
  }

  void handle_skills_command() {
    if (bundle_.skill_catalog) {
      renderer_->on_command_output(
          format_skill_catalog(*bundle_.skill_catalog));
    } else if (args_.no_skills) {
      renderer_->on_command_output("skills disabled (--no-skills or "
                                   "[skills] disabled = true)\n");
    } else {
      renderer_->on_command_output(
          "(no skills discovered)\n"
          "Add SKILL.md files under .pici/skills/, skills/ or "
          ".agents/skills/ in the workspace, or skills/ under the config "
          "directory.\n");
    }
  }

  void handle_addons_command() {
    renderer_->on_command_output(format_addons(bundle_.hooks_list_saved));
  }

  void handle_reload_addons_command() {
    try {
      reload_addons();
      renderer_->on_command_output(
          "reloaded " + std::to_string(bundle_.hooks_list_saved.size()) +
          " add-on(s)");
    } catch (const std::exception &e) {
      renderer_->on_command_output("add-on reload failed: " +
                                   std::string(e.what()));
    }
  }

  void handle_compact_command() {
    // Manual compaction is interactive-only: it drives a live status hook
    // and an interruptible network request, neither of which has a
    // meaningful non-TTY/piped equivalent. The interactive command loop
    // does run with piped stdin — see /model's isatty() branch below for
    // the established pattern of a slash command explicitly degrading for
    // non-interactive input rather than silently misbehaving — so this
    // guard is reachable and load-bearing, not defensive dead code.
    if (isatty(STDIN_FILENO) == 0) {
      renderer_->on_command_output(
          "/compact is only available in an interactive terminal session");
      return;
    }
    // See run_and_persist()'s comment: title_controller_ is always set by
    // this point.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    core::TerminalTitleActivityGuard activity(*title_controller_);
    auto result = run_compaction_command(
        runtime(), *renderer_, args_.verbose, stream_diagnostics_,
        bundle_.hook_runtime, core::CompactionTrigger::manual);
    if (result.success) {
      renderer_->on_command_output(
          "compacted context: " +
          std::to_string(result.retained_message_count) +
          " message(s) retained");
    } else if (result.unsupported) {
      renderer_->on_command_output(
          "compaction is not supported by the active provider/model");
    } else if (result.cancelled) {
      renderer_->on_command_output("compaction cancelled");
    } else {
      renderer_->on_command_output("compaction failed: " +
                                   result.error.value_or("unknown error"));
    }
  }

  void handle_model_command(const std::string &line) {
    std::string spec = line.size() > 6 ? line.substr(6) : "";
    spec.erase(0, spec.find_first_not_of(" \t"));
    if (spec.empty()) {
      const auto current_model = agent().state().model();
      if (isatty(STDIN_FILENO) == 0) {
        renderer_->on_command_output("model: " + current_model.provider + "/" +
                                     current_model.id +
                                     "\nusage: /model <provider/model>");
        return;
      }
      const auto selected = cli::run_model_selector(
          registry_->search_models(""), current_model.provider,
          current_model.id,
          [this](const core::Model &candidate) {
            switch (authentication_->availability(candidate.provider)) {
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
          renderer_->owns_status_line());
      renderer_->force_full_repaint();
      if (selected.cancelled || !selected.model)
        return;
      spec = selected.model->provider + "/" + selected.model->id;
    }

    core::ModelSelection selection{.model = spec, .source = "cli"};
    const auto resolution = registry_->resolve(selection);
    if (!resolution) {
      renderer_->on_command_output("model switch failed: " + resolution.error);
      return;
    }
    // resolution's operator bool() is defined as model.has_value(), so the
    // !resolution check above already guarantees model is set here.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    const auto &selected_model = resolution.model.value();
    try {
      const auto result =
          runtime().set_model(selected_model, agent().state().thinking_level());
      if (result.error) {
        renderer_->on_command_output("model switch failed: " + *result.error);
        return;
      }
      runtime().mailbox_runtime().set_model(result.current.provider,
                                            result.current.id);
      configure_hooks();
      {
        std::scoped_lock lock(effective_context_mutex_);
        effective_context_.reset();
      }
      (void)update_terminal_ui();
      std::string message =
          "model: " + result.current.provider + "/" + result.current.id;
      if (result.warning)
        message += "\nwarning: " + *result.warning;
      renderer_->on_command_output(std::move(message));
    } catch (const std::exception &error) {
      renderer_->on_command_output("model switch failed: " +
                                   std::string(error.what()));
    }
  }

  void handle_models_command(const std::string &line) {
    std::string filter = line.size() > 7 ? line.substr(7) : "";
    filter.erase(0, filter.find_first_not_of(" \t"));
    renderer_->on_command_output(format_model_catalog(filter, registry_));
  }

  void handle_usage_command() {
    renderer_->on_command_output(
        format_usage(last_turn_, session_cost_, has_current_pricing()));
  }

  void handle_memory_command() {
    renderer_->on_command_output(
        format_memory(runtime(), *runtime().task_manager()));
  }

  void handle_name_command(const std::string &line) {
    std::string name = line.size() > 5 ? line.substr(5) : "";
    name.erase(0, name.find_first_not_of(" \t"));
    if (name.empty()) {
      std::cerr << "usage: /name <session name>\n";
    } else {
      bundle_.session_store->set_name(current_session_id_, name);
      current_session_name_ = name;
      runtime().mailbox_runtime().set_session_name(name);
      std::cerr << "[session name: " << name << "]\n";
    }
  }

  void handle_tree_command() {
    auto tree_opt =
        core::build_session_tree(*bundle_.session_store, current_session_id_);
    if (!tree_opt) {
      std::cerr << "no session tree available\n";
      return;
    }
    auto tree_lines = core::format_session_tree(*tree_opt, current_session_id_);

    std::size_t cursor = 0;
    for (std::size_t i = 0; i < tree_lines.size(); ++i) {
      if (tree_lines[i].session_id == current_session_id_) {
        cursor = i;
        break;
      }
    }

    auto result = cli::run_tree_selector(tree_lines, current_session_id_,
                                         cursor, renderer_->owns_status_line());
    renderer_->force_full_repaint();
    if (result.cancelled || result.selected_session_id == current_session_id_)
      return;

    auto loaded = bundle_.session_store->load(result.selected_session_id);
    if (!loaded) {
      std::cerr << "error: session not found\n";
      return;
    }
    current_session_id_ = result.selected_session_id;
    current_session_name_ = loaded->header.name;
    runtime().mailbox_runtime().drop_queued_delivery();
    runtime().activate_session(*loaded);
    if (const auto identity = runtime().mailbox_runtime().activate_root(
            current_session_id_, current_session_name_))
      agent().set_runtime_identity(identity);
    if (const auto &warning = runtime().last_warning())
      std::cerr << "warning: " << *warning << "\n";
    configure_hooks();
    {
      std::scoped_lock lock(effective_context_mutex_);
      effective_context_.reset();
    }
    std::cerr << "[session: " << current_session_id_;
    if (loaded->header.name)
      std::cerr << "  " << *loaded->header.name;
    std::cerr << "]\n";
  }

  void handle_new_command() {
    core::SessionHeader fresh_hdr;
    fresh_hdr.id = core::generate_session_id();
    fresh_hdr.created =
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    const auto current_model = agent().state().model();
    fresh_hdr.model = current_model.id;
    fresh_hdr.provider = current_model.provider;
    runtime().mailbox_runtime().drop_queued_delivery();
    current_session_id_ = runtime().create_session(fresh_hdr);
    current_session_name_.reset();
    if (const auto identity = runtime().mailbox_runtime().activate_root(
            current_session_id_, current_session_name_))
      agent().set_runtime_identity(identity);
    {
      std::scoped_lock lock(effective_context_mutex_);
      effective_context_.reset();
    }
    std::cerr << "[new session: " << current_session_id_ << "]\n";
  }

  void handle_fork_command() {
    core::SessionHeader child_hdr;
    child_hdr.id = core::generate_session_id();
    child_hdr.created =
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    const auto current_model = agent().state().model();
    child_hdr.model = current_model.id;
    child_hdr.provider = current_model.provider;
    child_hdr.parent_id = current_session_id_;
    child_hdr.parent_offset = agent().state().messages().size();
    runtime().mailbox_runtime().drop_queued_delivery();
    current_session_id_ = runtime().fork_session(child_hdr);
    current_session_name_.reset();
    if (const auto identity = runtime().mailbox_runtime().activate_root(
            current_session_id_, current_session_name_))
      agent().set_runtime_identity(identity);
    {
      std::scoped_lock lock(effective_context_mutex_);
      effective_context_.reset();
    }
    std::cerr << "[fork: " << current_session_id_ << "]\n";
  }

  // Returns true iff an add-on's on_command handled the line.
  bool handle_hook_command(const std::string &line) {
    auto space = line.find(' ');
    std::string cmd = line.substr(
        1, space == std::string::npos ? std::string::npos : space - 1);
    std::string rest = space == std::string::npos ? "" : line.substr(space + 1);

    core::LuaContextSnapshot context_snapshot;
    context_snapshot.raw = agent().context_snapshot();
    {
      std::scoped_lock lock(effective_context_mutex_);
      context_snapshot.effective = effective_context_;
    }
    auto result = bundle_.hooks->on_command(
        cmd, rest, context_snapshot.raw.messages, context_snapshot);
    if (!result.handled)
      return false;
    if (result.truncate_to) {
      runtime().truncate_active_session(*result.truncate_to);
      std::scoped_lock lock(effective_context_mutex_);
      effective_context_.reset();
    }
    if (result.output)
      renderer_->on_command_output(*result.output);
    if (result.prompt)
      accumulate(run_and_persist(*result.prompt));
    return true;
  }

  cli::Args args_;
  std::shared_ptr<const core::ModelCatalog> registry_;
  std::shared_ptr<const core::ModelCatalog> effective_registry_;
  std::shared_ptr<core::RemoteFauxClient> remote_client_;
  std::shared_ptr<core::ScriptedToolRegistry> scripted_registry_;
  core::Model model_;
  std::shared_ptr<core::StreamDiagnostics> stream_diagnostics_;
  std::shared_ptr<pi::auth::Authentication> authentication_;
  std::shared_ptr<pi::auth::Authentication> injected_authentication_;
  std::shared_ptr<core::SessionStore> injected_session_store_;
  std::mutex effective_context_mutex_;
  std::optional<core::AgentContext> effective_context_;
  cli::RuntimeBundle bundle_;
  core::SandboxMode sandbox_mode_{core::SandboxMode::auto_mode};
  std::function<void(const std::string &)> faux_tool_registrar_;
  std::vector<std::shared_ptr<const core::ToolDefinition>> base_tools_;
  std::shared_ptr<cli::ReadlineWake> repl_wake_;
  std::shared_ptr<core::SubagentActivityBridge> activity_;
  std::shared_ptr<std::atomic_uint64_t> compat_counter_;
  std::string current_session_id_;
  std::optional<std::string> current_session_name_;
  std::unique_ptr<core::Renderer> renderer_;
  std::optional<core::SubagentPanel> subagent_panel_;
  bool panel_mounted_{false};
  std::optional<core::TerminalTitleController> title_controller_;
  cli::TerminalRawMode session_raw_mode_;
  bool owns_full_screen_{false};
  cli::CompleteFn complete_fn_;
  cli::ControlFn control_fn_;
  CostAccumulator last_turn_;
  CostAccumulator session_cost_;
  core::TokenUsage last_usage_for_prompt_{};
  core::TokenUsage session_usage_for_prompt_{};
  core::MailboxAutonomousTurnBudget autonomous_budget_;
  bool autonomous_budget_exhausted_{false};
  std::string readline_draft_;
  std::size_t readline_cursor_{0};
};

inline int
cmd_run(const cli::Args &args,
        const std::shared_ptr<const core::ModelCatalog> &registry,
        std::shared_ptr<pi::auth::Authentication> authentication = {},
        std::shared_ptr<core::SessionStore> session_store = {}) {
  CmdRunSession session(args, registry, std::move(authentication),
                        std::move(session_store));
  return session.run();
}

} // namespace pi
