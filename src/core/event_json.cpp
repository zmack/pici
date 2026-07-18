#include "core/event_json.h"

#include "core/event_types.h"
#include "core/message_types.h"

#include <nlohmann/json.hpp>
#include <type_traits>
#include <utility>
#include <variant>

namespace pi::core {

namespace {

nlohmann::json message_json( // NOLINT(misc-include-cleaner)
    const Message &message) {
  return nlohmann::json::parse(json::to_json(message));
}

} // namespace

nlohmann::json event_to_json(const AgentEvent &event) {
  nlohmann::json data;
  std::visit(
      [&data](const auto &value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, AgentEndEvent>) {
          data["message_count"] = value.messages.size();
        } else if constexpr (std::is_same_v<T, TurnEndEvent>) {
          data["message"] = message_json(value.message);
          data["tool_result_count"] = value.tool_results.size();
        } else if constexpr (std::is_same_v<T, MessageStartEvent> ||
                             std::is_same_v<T, MessageEndEvent>) {
          data["message"] = message_json(value.message);
        } else if constexpr (std::is_same_v<T, MessageUpdateEvent>) {
          std::visit(
              [&data](const auto &update) {
                using U = std::decay_t<decltype(update)>;
                if constexpr (std::is_same_v<U,
                                             AssistantMessageTextDeltaEvent>) {
                  data["kind"] = "text_delta";
                  data["delta"] = update.delta;
                } else if constexpr (std::is_same_v<
                                         U,
                                         AssistantMessageThinkingDeltaEvent>) {
                  data["kind"] = "thinking_delta";
                  data["delta"] = update.delta;
                } else if constexpr (std::is_same_v<
                                         U,
                                         AssistantMessageToolCallDeltaEvent>) {
                  data["kind"] = "tool_call_delta";
                  data["delta"] = update.delta;
                } else if constexpr (std::is_same_v<
                                         U, AssistantMessageToolCallEndEvent>) {
                  data["kind"] = "tool_call";
                  data["tool_call"] = {
                      {"id", update.tool_call.id},
                      {"name", update.tool_call.name},
                      {"arguments", update.tool_call.arguments}};
                } else if constexpr (std::is_same_v<
                                         U, AssistantMessageDoneEvent>) {
                  data["kind"] = "done";
                  data["reason"] = stop_reason_to_string(update.reason);
                } else if constexpr (std::is_same_v<
                                         U, AssistantMessageErrorEvent>) {
                  data["kind"] = "error";
                  data["reason"] = stop_reason_to_string(update.reason);
                }
              },
              value.assistant_message_event);
        } else if constexpr (std::is_same_v<T, ToolExecutionStartEvent>) {
          data = {{"tool_call_id", value.tool_call_id},
                  {"tool_name", value.tool_name},
                  {"args", value.args}};
        } else if constexpr (std::is_same_v<T, ToolExecutionUpdateEvent>) {
          data = {{"tool_call_id", value.tool_call_id},
                  {"tool_name", value.tool_name},
                  {"partial_result", value.partial_result}};
        } else if constexpr (std::is_same_v<T, ToolExecutionEndEvent>) {
          data = {{"tool_call_id", value.tool_call_id},
                  {"tool_name", value.tool_name},
                  {"is_error", value.is_error},
                  {"status", tool_execution_status_to_string(value.status)}};
          if (value.result)
            data["result"] = value.result->content();
          if (value.result && value.result->details())
            data["details"] = *value.result->details();
        }
      },
      event);

  const auto &base = std::visit(
      [](const auto &value) -> const EventBase & { return value; }, event);
  return nlohmann::json{{"type", "event"},
                        {"event", event_type_to_string(base.type)},
                        {"sequence", base.sequence},
                        {"timestamp", base.timestamp},
                        {"data", std::move(data)}};
}

} // namespace pi::core
