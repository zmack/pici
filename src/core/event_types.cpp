#include "core/event_types.h"
#include "core/message_types.h"

#include <concepts>
#include <ostream>
#include <string_view>
#include <type_traits>
#include <variant>

namespace pi::core {

std::string_view event_type_to_string(EventType type) {
  switch (type) {
  case EventType::agent_start:
    return "agent_start";
  case EventType::agent_end:
    return "agent_end";
  case EventType::turn_start:
    return "turn_start";
  case EventType::turn_end:
    return "turn_end";
  case EventType::turn_aborted:
    return "turn_aborted";
  case EventType::message_start:
    return "message_start";
  case EventType::message_update:
    return "message_update";
  case EventType::message_end:
    return "message_end";
  case EventType::tool_execution_start:
    return "tool_execution_start";
  case EventType::tool_execution_update:
    return "tool_execution_update";
  case EventType::tool_execution_end:
    return "tool_execution_end";
  case EventType::tool_presentation:
    return "tool_presentation";
  case EventType::compaction:
    return "compaction";
  }
  return "unknown";
}

std::string_view compaction_event_kind_to_string(CompactionEventKind kind) {
  switch (kind) {
  case CompactionEventKind::start:
    return "start";
  case CompactionEventKind::complete:
    return "complete";
  case CompactionEventKind::error:
    return "error";
  }
  return "unknown";
}

std::string_view turn_abort_reason_to_string(TurnAbortReason reason) {
  switch (reason) {
  case TurnAbortReason::user_interrupt:
    return "user_interrupt";
  case TurnAbortReason::parent_interrupt:
    return "parent_interrupt";
  case TurnAbortReason::replacement_task:
    return "replacement_task";
  case TurnAbortReason::shutdown:
    return "shutdown";
  case TurnAbortReason::timeout:
    return "timeout";
  case TurnAbortReason::budget:
    return "budget";
  case TurnAbortReason::unknown:
    return "unknown";
  }
  return "unknown";
}

std::string_view tool_execution_status_to_string(ToolExecutionStatus status) {
  switch (status) {
  case ToolExecutionStatus::success:
    return "success";
  case ToolExecutionStatus::blocked:
    return "blocked";
  case ToolExecutionStatus::error:
    return "error";
  case ToolExecutionStatus::cancelled:
    return "cancelled";
  }
  return "error";
}

namespace {

std::string_view msg_type(const Message &msg) {
  if (std::holds_alternative<UserMessage>(msg))
    return "user";
  if (std::holds_alternative<AssistantMessage>(msg))
    return "assistant";
  if (std::holds_alternative<ToolResultMessage>(msg))
    return "toolResult";
  return "unknown";
}

} // namespace

std::ostream &operator<<(std::ostream &os, const AgentEvent &event) {
  os << "{";
  std::visit(
      [&](const auto &ev) {
        using T = std::remove_cvref_t<decltype(ev)>;
        if constexpr (std::same_as<T, AgentStartEvent> ||
                      std::same_as<T, TurnStartEvent> ||
                      std::same_as<T, ToolPresentationEvent>) {
          os << "type:" << event_type_to_string(ev.type) << "}";
        } else if constexpr (std::same_as<T, TurnAbortedEvent>) {
          os << "type:" << event_type_to_string(ev.type)
             << ", reason: " << turn_abort_reason_to_string(ev.reason) << "}";
        } else if constexpr (std::same_as<T, AgentEndEvent>) {
          os << "type:" << event_type_to_string(ev.type)
             << ", messages: " << ev.messages.size() << "}";
        } else if constexpr (std::same_as<T, TurnEndEvent>) {
          os << "type:" << event_type_to_string(ev.type)
             << ", msg: " << msg_type(ev.message)
             << ", tools: " << ev.tool_results.size() << "}";
        } else if constexpr (std::same_as<T, MessageStartEvent> ||
                             std::same_as<T, MessageEndEvent>) {
          os << "type:" << event_type_to_string(ev.type)
             << ", msg: " << msg_type(ev.message) << "}";
        } else if constexpr (std::same_as<T, MessageUpdateEvent>) {
          os << "type:" << event_type_to_string(ev.type)
             << ", msg: " << msg_type(ev.message)
             << ", ev_idx: " << ev.assistant_message_event.index() << "}";
        } else if constexpr (std::same_as<T, ToolExecutionStartEvent>) {
          os << "type:" << event_type_to_string(ev.type)
             << ", tool: " << ev.tool_name << ", call: " << ev.tool_call_id
             << "}";
        } else if constexpr (std::same_as<T, ToolExecutionUpdateEvent>) {
          os << "type:" << event_type_to_string(ev.type)
             << ", tool: " << ev.tool_name << "}";
        } else if constexpr (std::same_as<T, ToolExecutionEndEvent>) {
          os << "type:" << event_type_to_string(ev.type)
             << ", tool: " << ev.tool_name << ", error: " << ev.is_error << "}";
        } else if constexpr (std::same_as<T, CompactionEvent>) {
          os << "type:" << event_type_to_string(ev.type)
             << ", kind: " << compaction_event_kind_to_string(ev.kind)
             << ", retained: " << ev.retained_message_count << "}";
        }
      },
      event);
  return os;
}

} // namespace pi::core
