#include "core/event_json.h"

#include "core/event_types.h"
#include "core/message_types.h"
#include "core/request_presentation.h"

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
namespace {

nlohmann::json request_json(const RequestPresentation &request) {
  const auto *const source = [&] {
    switch (request.source) {
    case RequestSource::ordinary:
      return "ordinary";
    case RequestSource::mailbox:
      return "mailbox";
    case RequestSource::follow_up:
      return "follow_up";
    }
    return "ordinary";
  }();
  nlohmann::json result = {{"source", source}};
  if (request.message_id)
    result["message_id"] = *request.message_id;
  if (request.message_kind)
    result["message_kind"] = *request.message_kind;
  if (request.sender_agent_id)
    result["sender_agent_id"] = *request.sender_agent_id;
  if (request.sender_session_id)
    result["sender_session_id"] = *request.sender_session_id;
  if (request.sender_task_path)
    result["sender_task_path"] = *request.sender_task_path;
  if (request.sender_session_name)
    result["sender_session_name"] = *request.sender_session_name;
  return result;
}

} // namespace

nlohmann::json event_to_json(const AgentEvent &event) {
  nlohmann::json data;
  std::visit(
      [&data](const auto &value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, AgentEndEvent>) {
          data["message_count"] = value.messages.size();
        } else if constexpr (std::is_same_v<T, TurnAbortedEvent>) {
          data["reason"] = turn_abort_reason_to_string(value.reason);
        } else if constexpr (std::is_same_v<T, TurnEndEvent>) {
          data["message"] = message_json(value.message);
          data["tool_result_count"] = value.tool_results.size();
        } else if constexpr (std::is_same_v<T, MessageStartEvent>) {
          data["message"] = message_json(value.message);
          if (value.request)
            data["request"] = request_json(*value.request);
        } else if constexpr (std::is_same_v<T, MessageEndEvent>) {
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
        } else if constexpr (std::is_same_v<T, ToolPresentationEvent>) {
          std::visit(
              [&data, &value](const auto &notice) {
                using N = std::decay_t<decltype(notice)>;
                if constexpr (std::is_same_v<N, MailboxReplyQueuedNotice>) {
                  data = {{"kind", "mailbox_reply_queued"},
                          {"request_message_id", notice.request_message_id},
                          {"tool_call_id", value.tool_call_id},
                          {"recipient_session_id", notice.recipient_session_id},
                          {"recipient_agent_id",
                           notice.recipient_agent_id
                               ? nlohmann::json(*notice.recipient_agent_id)
                               : nlohmann::json(nullptr)},
                          {"reply_text", notice.reply_text},
                          {"state", "queued"}};
                }
              },
              value.notice);
        } else if constexpr (std::is_same_v<T, ToolExecutionEndEvent>) {
          data = {{"tool_call_id", value.tool_call_id},
                  {"tool_name", value.tool_name},
                  {"is_error", value.is_error},
                  {"status", tool_execution_status_to_string(value.status)}};
          if (value.result)
            data["result"] = value.result->content();
          if (value.result && value.result->details())
            data["details"] = *value.result->details();
        } else if constexpr (std::is_same_v<T, CompactionEvent>) {
          // Deliberately metadata-only: never serialize replacement_messages
          // here. That would risk round-tripping opaque provider-encrypted
          // compaction payloads (or plain transcript text) into a
          // machine-facing event log; counts/timing/status are sufficient
          // for RPC/ACP observers per the plan's privacy requirements.
          data = {{"kind", compaction_event_kind_to_string(value.kind)},
                  {"provider", value.provider},
                  {"model", value.model},
                  {"summary", value.summary},
                  {"retained_message_count", value.retained_message_count}};
          if (value.response_id)
            data["response_id"] = *value.response_id;
          if (value.error_message)
            data["error"] = *value.error_message;
          data["cancelled"] = value.cancelled;
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
