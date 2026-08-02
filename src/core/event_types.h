#pragma once

#include <chrono>
#include <concepts>
#include <cstdint>
#include <memory>
#include <source_location>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "core/message_types.h"

namespace pi::core {

// Forward declaration — defined in stream.h

struct AssistantMessageStartEvent {
  AssistantMessage partial;
};
struct AssistantMessageTextStartEvent {
  std::size_t content_index;
  AssistantMessage partial;
};
struct AssistantMessageTextDeltaEvent {
  std::size_t content_index;
  std::string delta;
  AssistantMessage partial;
};
struct AssistantMessageTextEndEvent {
  std::size_t content_index;
  std::string content;
  AssistantMessage partial;
};
struct AssistantMessageThinkingStartEvent {
  std::size_t content_index;
  AssistantMessage partial;
};
struct AssistantMessageThinkingDeltaEvent {
  std::size_t content_index;
  std::string delta;
  AssistantMessage partial;
};
struct AssistantMessageThinkingEndEvent {
  std::size_t content_index;
  std::string content;
  AssistantMessage partial;
};
struct AssistantMessageToolCallStartEvent {
  std::size_t content_index;
  AssistantMessage partial;
};
struct AssistantMessageToolCallDeltaEvent {
  std::size_t content_index;
  std::string delta;
  AssistantMessage partial;
};
struct AssistantMessageToolCallEndEvent {
  std::size_t content_index;
  ToolCall tool_call;
  AssistantMessage partial;
};
struct AssistantMessageDoneEvent {
  StopReason reason;
  AssistantMessage message;
};
struct AssistantMessageErrorEvent {
  StopReason reason;
  AssistantMessage error;
};

using AssistantMessageEvent = std::variant<
    AssistantMessageStartEvent, AssistantMessageTextStartEvent,
    AssistantMessageTextDeltaEvent, AssistantMessageTextEndEvent,
    AssistantMessageThinkingStartEvent, AssistantMessageThinkingDeltaEvent,
    AssistantMessageThinkingEndEvent, AssistantMessageToolCallStartEvent,
    AssistantMessageToolCallDeltaEvent, AssistantMessageToolCallEndEvent,
    AssistantMessageDoneEvent, AssistantMessageErrorEvent>;

enum class EventType {
  agent_start,
  agent_end,
  turn_start,
  turn_end,
  turn_aborted,
  message_start,
  message_update,
  message_end,
  tool_execution_start,
  tool_execution_update,
  tool_execution_end,
};

std::string_view event_type_to_string(EventType type);

enum class TurnAbortReason {
  user_interrupt,
  parent_interrupt,
  replacement_task,
  shutdown,
  timeout,
  budget,
  unknown,
};

std::string_view turn_abort_reason_to_string(TurnAbortReason reason);
// Machine-readable outcome for a tool execution. The legacy is_error field on
// ToolExecutionEndEvent remains for callers that only need success/failure.
enum class ToolExecutionStatus {
  success,
  blocked,
  error,
  cancelled,
};

std::string_view tool_execution_status_to_string(ToolExecutionStatus status);

// Base event — all events carry a timestamp and source location for debugging
struct EventBase {
  EventType type;
  std::int64_t timestamp{0};
  // Monotonic within one agent run. Zero means the event was constructed
  // outside the loop (for example by a unit test).
  std::uint64_t sequence{0};
  std::string source_file;
  std::uint32_t source_line{0};

  explicit EventBase(EventType event_type,
                     std::source_location loc = std::source_location::current())
      : type(event_type),
        timestamp(std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count()),
        source_file(loc.file_name()), source_line(loc.line()) {}

  static EventBase
  create(EventType type,
         std::source_location loc = std::source_location::current()) {
    return EventBase(type, loc);
  }
};

struct AgentStartEvent : EventBase {
  static constexpr EventType type = EventType::agent_start;
  explicit AgentStartEvent(
      std::source_location loc = std::source_location::current())
      : EventBase(EventType::agent_start, loc) {}
};

struct AgentEndEvent : EventBase {
  static constexpr EventType type = EventType::agent_end;
  std::vector<Message> messages;
  explicit AgentEndEvent(
      std::vector<Message> msgs,
      std::source_location loc = std::source_location::current())
      : EventBase(EventType::agent_end, loc), messages(std::move(msgs)) {}
};

struct TurnStartEvent : EventBase {
  static constexpr EventType type = EventType::turn_start;
  explicit TurnStartEvent(
      std::source_location loc = std::source_location::current())
      : EventBase(EventType::turn_start, loc) {}
};

struct TurnEndEvent : EventBase {
  static constexpr EventType type = EventType::turn_end;
  Message message;
  std::vector<ToolResultMessage> tool_results;
  TurnEndEvent(Message msg, std::vector<ToolResultMessage> results,
               std::source_location loc = std::source_location::current())
      : EventBase(EventType::turn_end, loc), message(std::move(msg)),
        tool_results(std::move(results)) {}
};

struct TurnAbortedEvent : EventBase {
  static constexpr EventType type = EventType::turn_aborted;
  TurnAbortReason reason{TurnAbortReason::unknown};

  explicit TurnAbortedEvent(
      TurnAbortReason abort_reason = TurnAbortReason::unknown,
      std::source_location loc = std::source_location::current())
      : EventBase(EventType::turn_aborted, loc), reason(abort_reason) {}
};

struct MessageStartEvent : EventBase {
  static constexpr EventType type = EventType::message_start;
  Message message;
  explicit MessageStartEvent(
      Message msg, std::source_location loc = std::source_location::current())
      : EventBase(EventType::message_start, loc), message(std::move(msg)) {}
};

struct MessageUpdateEvent : EventBase {
  static constexpr EventType type = EventType::message_update;
  Message message;
  AssistantMessageEvent assistant_message_event;
  MessageUpdateEvent(Message msg, AssistantMessageEvent ev,
                     std::source_location loc = std::source_location::current())
      : EventBase(EventType::message_update, loc), message(std::move(msg)),
        assistant_message_event(std::move(ev)) {}
};

struct MessageEndEvent : EventBase {
  static constexpr EventType type = EventType::message_end;
  Message message;
  explicit MessageEndEvent(
      Message msg, std::source_location loc = std::source_location::current())
      : EventBase(EventType::message_end, loc), message(std::move(msg)) {}
};

struct ToolExecutionStartEvent : EventBase {
  static constexpr EventType type = EventType::tool_execution_start;
  std::string tool_call_id;
  std::string tool_name;
  std::string args;
  ToolExecutionStartEvent(
      std::string id, std::string name, std::string a,
      std::source_location loc = std::source_location::current())
      : EventBase(EventType::tool_execution_start, loc),
        tool_call_id(std::move(id)), tool_name(std::move(name)),
        args(std::move(a)) {}
};

struct ToolExecutionUpdateEvent : EventBase {
  static constexpr EventType type = EventType::tool_execution_update;
  std::string tool_call_id;
  std::string tool_name;
  std::string args;
  std::string partial_result;
  ToolExecutionUpdateEvent(
      std::string id, std::string name, std::string a, std::string pr,
      std::source_location loc = std::source_location::current())
      : EventBase(EventType::tool_execution_update, loc),
        tool_call_id(std::move(id)), tool_name(std::move(name)),
        args(std::move(a)), partial_result(std::move(pr)) {}
};

struct ToolExecutionEndEvent : EventBase {
  static constexpr EventType type = EventType::tool_execution_end;
  std::string tool_call_id;
  std::string tool_name;
  std::shared_ptr<ToolResult> result;
  bool is_error{false};
  ToolExecutionStatus status{ToolExecutionStatus::success};
  ToolExecutionEndEvent(
      std::string id, std::string name, std::shared_ptr<ToolResult> res,
      bool err, std::source_location loc = std::source_location::current())
      : EventBase(EventType::tool_execution_end, loc),
        tool_call_id(std::move(id)), tool_name(std::move(name)),
        result(std::move(res)), is_error(err),
        status(err ? ToolExecutionStatus::error
                   : ToolExecutionStatus::success) {}
  ToolExecutionEndEvent(
      std::string id, std::string name, std::shared_ptr<ToolResult> res,
      bool err, ToolExecutionStatus outcome,
      std::source_location loc = std::source_location::current())
      : EventBase(EventType::tool_execution_end, loc),
        tool_call_id(std::move(id)), tool_name(std::move(name)),
        result(std::move(res)), is_error(err), status(outcome) {}
};

using AgentEvent =
    std::variant<AgentStartEvent, AgentEndEvent, TurnStartEvent, TurnEndEvent,
                 TurnAbortedEvent, MessageStartEvent, MessageUpdateEvent,
                 MessageEndEvent, ToolExecutionStartEvent,
                 ToolExecutionUpdateEvent, ToolExecutionEndEvent>;

template <typename F>
  requires(std::is_invocable_v<F, AgentStartEvent> &&
           std::is_invocable_v<F, AgentEndEvent> &&
           std::is_invocable_v<F, TurnStartEvent> &&
           std::is_invocable_v<F, TurnEndEvent> &&
           std::is_invocable_v<F, TurnAbortedEvent> &&
           std::is_invocable_v<F, MessageStartEvent> &&
           std::is_invocable_v<F, MessageUpdateEvent> &&
           std::is_invocable_v<F, MessageEndEvent> &&
           std::is_invocable_v<F, ToolExecutionStartEvent> &&
           std::is_invocable_v<F, ToolExecutionUpdateEvent> &&
           std::is_invocable_v<F, ToolExecutionEndEvent>)
void visit_event(const AgentEvent &ev, F &&visitor) {
  std::visit(std::forward<F>(visitor), ev);
}

template <typename F>
  requires(std::is_invocable_v<F, AgentStartEvent> &&
           std::is_invocable_v<F, AgentEndEvent> &&
           std::is_invocable_v<F, TurnStartEvent> &&
           std::is_invocable_v<F, TurnEndEvent> &&
           std::is_invocable_v<F, TurnAbortedEvent> &&
           std::is_invocable_v<F, MessageStartEvent> &&
           std::is_invocable_v<F, MessageUpdateEvent> &&
           std::is_invocable_v<F, MessageEndEvent> &&
           std::is_invocable_v<F, ToolExecutionStartEvent> &&
           std::is_invocable_v<F, ToolExecutionUpdateEvent> &&
           std::is_invocable_v<F, ToolExecutionEndEvent>)
auto map_event(const AgentEvent &ev, F &&visitor)
    -> decltype(visitor(AgentStartEvent{})) {
  return std::visit(std::forward<F>(visitor), ev);
}

std::ostream &operator<<(std::ostream &os, const AgentEvent &event);

} // namespace pi::core
