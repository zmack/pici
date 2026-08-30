#include "core/providers/faux_control.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "core/agent_state.h"
#include "core/event_types.h"
#include "core/llm_client.h"
#include "core/message_types.h"
#include "core/providers/faux.h"

namespace pi::core {

namespace {

bool require_string(const nlohmann::json &value, std::string_view field,
                    std::string &result, std::string &error) {
  if (!value.contains(field) || !value.at(field).is_string()) {
    error = "round field '" + std::string(field) + "' must be a string";
    return false;
  }
  result = value.at(field).get<std::string>();
  return true;
}

bool require_nonnegative_int(const nlohmann::json &value,
                             std::string_view field, int &result,
                             std::string &error) {
  if (!value.contains(field) || !value.at(field).is_number_integer()) {
    error = "round field '" + std::string(field) +
            "' must be a non-negative integer";
    return false;
  }
  result = value.at(field).get<int>();
  if (result < 0) {
    error = "round field '" + std::string(field) +
            "' must be a non-negative integer";
    return false;
  }
  return true;
}

void set_model_id(AssistantMessageEvent &event, std::string_view model_id) {
  std::visit(
      [model_id](auto &value) {
        using Event = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Event, AssistantMessageDoneEvent>) {
          value.message.model = model_id;
        } else if constexpr (std::is_same_v<Event,
                                            AssistantMessageErrorEvent>) {
          value.error.model = model_id;
        } else {
          value.partial.model = model_id;
        }
      },
      event);
}

} // namespace

namespace {

class ScriptedToolResult final : public ToolResult {
public:
  ScriptedToolResult(std::string content, bool is_error)
      : content_(std::move(content)), is_error_(is_error) {}

  bool is_error() const override { return is_error_; }
  std::string content() const override { return content_; }
  std::optional<std::string> details() const override { return std::nullopt; }

private:
  std::string content_;
  bool is_error_;
};

class PermissiveToolSchema final : public ToolSchema {
public:
  std::string serialize() const override { return R"({"type":"object"})"; }
  std::map<std::string, std::string> to_definition() const override {
    return {};
  }
};

bool wait_until(std::chrono::steady_clock::time_point target,
                const std::stop_token &stop_tok) {
  if (stop_tok.stop_requested())
    return false;
  std::mutex mutex;
  std::condition_variable_any condition;
  std::unique_lock lock(mutex);
  condition.wait_until(lock, stop_tok, target, [] { return false; });
  return !stop_tok.stop_requested() &&
         std::chrono::steady_clock::now() >= target;
}

} // namespace

ScriptedTool::ScriptedTool(std::string name,
                           std::shared_ptr<ScriptedToolRegistry> registry)
    : name_(std::move(name)), registry_(std::move(registry)) {}

ToolSchema &ScriptedTool::schema() const {
  static PermissiveToolSchema schema;
  return schema;
}

std::shared_ptr<ToolResult>
ScriptedTool::execute(std::string_view call_id, std::string_view args_json,
                      std::stop_token stop_tok,
                      ToolUpdateCallback on_update) const {
  return execute(args_json, ToolExecutionContext{
                                .call_id = call_id,
                                .stop_token = stop_tok,
                                .on_update = std::move(on_update),
                            });
}

std::shared_ptr<ToolResult>
ScriptedTool::execute(std::string_view, ToolExecutionContext context) const {
  const auto missing_behavior = [&] {
    return std::make_shared<ScriptedToolResult>(
        "faux-control: no scripted behavior registered for call_id \"" +
            std::string(context.call_id) + "\"",
        true);
  };
  if (!registry_)
    return missing_behavior();
  auto behavior = registry_->take_behavior(std::string(context.call_id));
  if (!behavior)
    return missing_behavior();

  const auto start = std::chrono::steady_clock::now();
  for (const auto &update : behavior->updates) {
    if (!wait_until(start + std::chrono::milliseconds(update.after_ms),
                    context.stop_token))
      return std::make_shared<ScriptedToolResult>("cancelled", true);
    if (context.on_update)
      context.on_update(
          std::make_shared<ScriptedToolResult>(update.partial, false));
  }

  if (!wait_until(start + std::chrono::milliseconds(behavior->finish_after_ms),
                  context.stop_token))
    return std::make_shared<ScriptedToolResult>("cancelled", true);
  if (!behavior->is_error && behavior->presentation_notice &&
      context.on_presentation)
    context.on_presentation(*behavior->presentation_notice);
  return std::make_shared<ScriptedToolResult>(behavior->result_content,
                                              behavior->is_error);
}

void ScriptedToolRegistry::register_behavior(std::string call_id,
                                             ScriptedToolBehavior behavior) {
  std::scoped_lock lock(mutex_);
  behaviors_[std::move(call_id)] = std::move(behavior);
}

std::optional<ScriptedToolBehavior>
ScriptedToolRegistry::take_behavior(const std::string &call_id) {
  std::scoped_lock lock(mutex_);
  auto it = behaviors_.find(call_id);
  if (it == behaviors_.end())
    return std::nullopt;
  auto behavior = std::move(it->second);
  behaviors_.erase(it);
  return behavior;
}

void RemoteFauxClient::push_round(FauxClient::Script script) {
  {
    std::scoped_lock lock(mutex_);
    queue_.push_back(std::move(script));
  }
  cv_.notify_all();
}

void RemoteFauxClient::close() {
  {
    std::scoped_lock lock(mutex_);
    closed_ = true;
  }
  cv_.notify_all();
}

std::shared_ptr<AssistantMessage>
RemoteFauxClient::stream(const Model &model, const AgentContext &context,
                         const StreamOptions &options,
                         AssistantEventCallback on_event,
                         std::stop_token stop_tok) {
  (void)context;
  (void)options;

  FauxClient::Script script;
  {
    std::unique_lock lock(mutex_);
    const bool got_one =
        cv_.wait(lock, stop_tok, [&] { return !queue_.empty() || closed_; });
    if (!got_one || queue_.empty()) {
      auto message = std::make_shared<AssistantMessage>();
      message->api = "faux-control";
      message->provider = "faux-control";
      message->model = model.id;
      message->stop_reason = StopReason::error;
      message->error_message = "No more faux-control rounds queued";
      if (on_event) {
        on_event(AssistantMessageErrorEvent{.reason = StopReason::error,
                                            .error = *message});
      }
      return message;
    }
    script = std::move(queue_.front());
    queue_.pop_front();
  }

  std::shared_ptr<AssistantMessage> final_message;
  for (auto event : script.events) {
    if (stop_tok.stop_requested())
      break;

    set_model_id(event, model.id);
    if (on_event)
      on_event(event);

    std::visit(
        [&final_message](const auto &value) {
          using Event = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<Event, AssistantMessageDoneEvent>) {
            final_message = std::make_shared<AssistantMessage>(value.message);
          } else if constexpr (std::is_same_v<Event,
                                              AssistantMessageErrorEvent>) {
            final_message = std::make_shared<AssistantMessage>(value.error);
          }
        },
        event);

    if (script.delay_between && !stop_tok.stop_requested())
      wait_until(std::chrono::steady_clock::now() + *script.delay_between,
                 stop_tok);
  }

  if (!final_message) {
    final_message = std::make_shared<AssistantMessage>();
    final_message->api = "faux-control";
    final_message->provider = "faux-control";
    final_message->model = model.id;
    final_message->stop_reason = StopReason::error;
    final_message->error_message = "Faux script had no terminal event";
  }
  return final_message;
}

// The block-kind and tool-call-field helpers below were formerly inlined
// directly in compile_round()'s content loop; each is factored out purely
// to shrink that function's branch count, with no behavior change.

bool compile_text_block(const nlohmann::json &block, std::size_t index,
                        AssistantMessage &partial, FauxClient::Script &script,
                        std::string &error) {
  std::string text;
  if (!require_string(block, "text", text, error))
    return false;
  script.events.emplace_back(AssistantMessageTextStartEvent{index, partial});
  partial.content.emplace_back(TextContent{});
  std::get<TextContent>(partial.content.back()).text = text;
  script.events.emplace_back(
      AssistantMessageTextDeltaEvent{index, text, partial});
  script.events.emplace_back(
      AssistantMessageTextEndEvent{index, text, partial});
  return true;
}

bool compile_thinking_block(const nlohmann::json &block, std::size_t index,
                            AssistantMessage &partial,
                            FauxClient::Script &script, std::string &error) {
  std::string text;
  if (!require_string(block, "text", text, error))
    return false;
  script.events.emplace_back(
      AssistantMessageThinkingStartEvent{index, partial});
  partial.content.emplace_back(ThinkingContent{});
  std::get<ThinkingContent>(partial.content.back()).thinking = text;
  script.events.emplace_back(
      AssistantMessageThinkingDeltaEvent{index, text, partial});
  script.events.emplace_back(
      AssistantMessageThinkingEndEvent{index, text, partial});
  return true;
}

bool parse_tool_updates(const nlohmann::json &block,
                        ScriptedToolBehavior &behavior, std::string &error) {
  if (!block.contains("updates"))
    return true;
  if (!block.at("updates").is_array()) {
    error = "tool_call field 'updates' must be an array";
    return false;
  }
  int previous_after_ms = 0;
  bool first_update = true;
  for (const auto &update : block.at("updates")) {
    if (!update.is_object()) {
      error = "tool_call updates must be JSON objects";
      return false;
    }
    int after_ms = 0;
    if (!require_nonnegative_int(update, "after_ms", after_ms, error))
      return false;
    if (!first_update && after_ms < previous_after_ms) {
      error = "tool_call updates must be in ascending after_ms order";
      return false;
    }
    std::string partial_result;
    if (!require_string(update, "partial", partial_result, error))
      return false;
    behavior.updates.push_back(
        ScriptedToolUpdate{after_ms, std::move(partial_result)});
    previous_after_ms = after_ms;
    first_update = false;
  }
  return true;
}

bool parse_tool_result(const nlohmann::json &block,
                       ScriptedToolBehavior &behavior, std::string &error) {
  if (!block.contains("result"))
    return true;
  if (!block.at("result").is_object()) {
    error = "tool_call field 'result' must be an object";
    return false;
  }
  const auto &result = block.at("result");
  if (result.contains("content")) {
    if (!result.at("content").is_string()) {
      error = "tool_call result field 'content' must be a string";
      return false;
    }
    behavior.result_content = result.at("content").get<std::string>();
  }
  if (result.contains("is_error")) {
    if (!result.at("is_error").is_boolean()) {
      error = "tool_call result field 'is_error' must be boolean";
      return false;
    }
    behavior.is_error = result.at("is_error").get<bool>();
  }
  return true;
}

bool parse_tool_presentation(const nlohmann::json &block,
                             ScriptedToolBehavior &behavior,
                             std::string &error) {
  if (!block.contains("presentation"))
    return true;
  const auto &presentation = block.at("presentation");
  if (!presentation.is_object()) {
    error = "tool_call field 'presentation' must be an object";
    return false;
  }
  std::string kind;
  if (!require_string(presentation, "kind", kind, error))
    return false;
  if (kind != "mailbox_reply_queued") {
    error = "tool_call presentation field 'kind' must be "
            "'mailbox_reply_queued'";
    return false;
  }
  MailboxReplyQueuedNotice notice;
  if (!require_string(presentation, "request_message_id",
                      notice.request_message_id, error))
    return false;
  if (!require_string(presentation, "text", notice.reply_text, error))
    return false;
  if (!require_string(presentation, "recipient_session_id",
                      notice.recipient_session_id, error))
    return false;
  if (presentation.contains("recipient_agent_id") &&
      !presentation.at("recipient_agent_id").is_null() &&
      !presentation.at("recipient_agent_id").is_string()) {
    error = "tool_call presentation field 'recipient_agent_id' must be a "
            "string or null";
    return false;
  }
  if (presentation.contains("recipient_agent_id") &&
      presentation.at("recipient_agent_id").is_string())
    notice.recipient_agent_id =
        presentation.at("recipient_agent_id").get<std::string>();
  behavior.presentation_notice = std::move(notice);
  return true;
}

bool compile_tool_call_block(
    const nlohmann::json &block, std::size_t index, AssistantMessage &partial,
    FauxClient::Script &script,
    std::vector<std::pair<std::string, ScriptedToolBehavior>> &behaviors,
    std::string &error) {
  std::string call_id;
  if (!require_string(block, "call_id", call_id, error))
    return false;
  if (call_id.empty()) {
    error = "tool_call field 'call_id' must not be empty";
    return false;
  }
  std::string name;
  if (!require_string(block, "name", name, error))
    return false;
  if (name.empty()) {
    error = "tool_call field 'name' must not be empty";
    return false;
  }

  nlohmann::json arguments = nlohmann::json::object();
  if (block.contains("args")) {
    if (!block.at("args").is_object()) {
      error = "tool_call field 'args' must be a JSON object";
      return false;
    }
    arguments = block.at("args");
  }

  ScriptedToolBehavior behavior;
  if (!parse_tool_updates(block, behavior, error))
    return false;
  if (!parse_tool_result(block, behavior, error))
    return false;
  if (!parse_tool_presentation(block, behavior, error))
    return false;
  if (block.contains("finish_after_ms") &&
      !require_nonnegative_int(block, "finish_after_ms",
                               behavior.finish_after_ms, error))
    return false;

  ToolCall call{.id = call_id,
                .name = name,
                .arguments = arguments,
                .partial_json = arguments.dump()};
  script.events.emplace_back(
      AssistantMessageToolCallStartEvent{index, partial});
  partial.content.emplace_back(call);
  script.events.emplace_back(
      AssistantMessageToolCallDeltaEvent{index, call.partial_json, partial});
  script.events.emplace_back(
      AssistantMessageToolCallEndEvent{index, call, partial});
  behaviors.emplace_back(std::move(call_id), std::move(behavior));
  return true;
}

std::optional<FauxClient::Script> compile_round(const nlohmann::json &round,
                                                ScriptedToolRegistry &registry,
                                                std::string &error) {
  error.clear();
  try {
    if (!round.is_object()) {
      error = "round must be a JSON object";
      return std::nullopt;
    }
    std::string type;
    if (!require_string(round, "type", type, error))
      return std::nullopt;
    if (type != "round") {
      error = "round field 'type' must be 'round'";
      return std::nullopt;
    }

    std::string stop_reason;
    if (!require_string(round, "stop_reason", stop_reason, error))
      return std::nullopt;
    StopReason terminal_reason = StopReason::error;
    if (stop_reason == "tool_calls") {
      terminal_reason = StopReason::tool_use;
    } else if (stop_reason == "end_turn") {
      terminal_reason = StopReason::stop;
    } else {
      error = "round field 'stop_reason' must be 'tool_calls' or 'end_turn'";
      return std::nullopt;
    }

    if (!round.contains("content") || !round.at("content").is_array()) {
      error = "round field 'content' must be an array";
      return std::nullopt;
    }

    std::chrono::milliseconds delay_between{0};
    if (round.contains("delay_between_ms")) {
      int delay_ms = 0;
      if (!require_nonnegative_int(round, "delay_between_ms", delay_ms, error))
        return std::nullopt;
      delay_between = std::chrono::milliseconds(delay_ms);
    }

    AssistantMessage partial;
    partial.api = "faux-control";
    partial.provider = "faux-control";
    partial.stop_reason = terminal_reason;

    FauxClient::Script script;
    script.events.emplace_back(AssistantMessageStartEvent{partial});
    if (delay_between.count() > 0)
      script.delay_between = delay_between;

    std::vector<std::pair<std::string, ScriptedToolBehavior>> behaviors;
    for (std::size_t index = 0; index < round.at("content").size(); ++index) {
      const auto &block = round.at("content").at(index);
      if (!block.is_object()) {
        error = "round content entries must be JSON objects";
        return std::nullopt;
      }
      std::string block_type;
      if (!require_string(block, "type", block_type, error))
        return std::nullopt;

      if (block_type == "text") {
        if (!compile_text_block(block, index, partial, script, error))
          return std::nullopt;
        continue;
      }

      if (block_type == "thinking") {
        if (!compile_thinking_block(block, index, partial, script, error))
          return std::nullopt;
        continue;
      }

      if (block_type != "tool_call") {
        error = "unknown round content type '" + block_type + "'";
        return std::nullopt;
      }

      if (!compile_tool_call_block(block, index, partial, script, behaviors,
                                   error))
        return std::nullopt;
    }

    partial.stop_reason = terminal_reason;
    script.events.emplace_back(
        AssistantMessageDoneEvent{terminal_reason, partial});
    for (auto &[call_id, behavior] : behaviors)
      registry.register_behavior(std::move(call_id), std::move(behavior));
    return script;
  } catch (const std::exception &exception) {
    error = std::string("invalid round: ") + exception.what();
    return std::nullopt;
  }
}

} // namespace pi::core
