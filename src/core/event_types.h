#pragma once

#include <concepts>
#include <memory>
#include <source_location>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "core/message_types.h"

namespace pi::core {

// Forward declaration — defined in stream.h

// ─── Event type enumeration ────────────────────────────────────────────────

enum class EventType {
    agent_start,
    agent_end,
    turn_start,
    turn_end,
    message_start,
    message_update,
    message_end,
    tool_execution_start,
    tool_execution_update,
    tool_execution_end,
};

std::string_view event_type_to_string(EventType type);

// ─── Event variant ─────────────────────────────────────────────────────────

// Base event — all events carry a timestamp and source location for debugging
struct EventBase {
    EventType type;
    std::int64_t timestamp;
    std::string source_file;
    std::uint32_t source_line;

    static EventBase create(EventType type,
                            std::source_location loc = std::source_location::current()) {
        return {type,
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count(),
                loc.file_name(), static_cast<std::int64_t>(loc.line())};
    }
};

struct AgentStartEvent : EventBase {
    static constexpr EventType type = EventType::agent_start;
    AgentStartEvent() : EventBase(EventType::agent_start) {}
};

struct AgentEndEvent : EventBase {
    static constexpr EventType type = EventType::agent_end;
    std::vector<Message> messages;
    AgentEndEvent(std::vector<Message> msgs,
                  std::source_location loc = std::source_location::current())
        : EventBase(EventType::agent_end), messages(std::move(msgs)) {}
};

struct TurnStartEvent : EventBase {
    static constexpr EventType type = EventType::turn_start;
    TurnStartEvent() : EventBase(EventType::turn_start) {}
};

struct TurnEndEvent : EventBase {
    static constexpr EventType type = EventType::turn_end;
    Message message;
    std::vector<ToolResultMessage> tool_results;
    TurnEndEvent(Message msg,
                 std::vector<ToolResultMessage> results,
                 std::source_location loc = std::source_location::current())
        : EventBase(EventType::turn_end),
          message(std::move(msg)),
          tool_results(std::move(results)) {}
};

struct MessageStartEvent : EventBase {
    static constexpr EventType type = EventType::message_start;
    Message message;
    MessageStartEvent(Message msg,
                      std::source_location loc = std::source_location::current())
        : EventBase(EventType::message_start), message(std::move(msg)) {}
};

struct MessageUpdateEvent : EventBase {
    static constexpr EventType type = EventType::message_update;
    Message message;
    std::string delta; // describes what changed
    MessageUpdateEvent(Message msg, std::string d,
                       std::source_location loc = std::source_location::current())
        : EventBase(EventType::message_update),
          message(std::move(msg)),
          delta(std::move(d)) {}
};

struct MessageEndEvent : EventBase {
    static constexpr EventType type = EventType::message_end;
    Message message;
    MessageEndEvent(Message msg,
                    std::source_location loc = std::source_location::current())
        : EventBase(EventType::message_end), message(std::move(msg)) {}
};

struct ToolExecutionStartEvent : EventBase {
    static constexpr EventType type = EventType::tool_execution_start;
    std::string tool_call_id;
    std::string tool_name;
    std::string args;
    ToolExecutionStartEvent(std::string id, std::string name, std::string a,
                            std::source_location loc = std::source_location::current())
        : EventBase(EventType::tool_execution_start),
          tool_call_id(std::move(id)),
          tool_name(std::move(name)),
          args(std::move(a)) {}
};

struct ToolExecutionUpdateEvent : EventBase {
    static constexpr EventType type = EventType::tool_execution_update;
    std::string tool_call_id;
    std::string tool_name;
    std::string args;
    std::string partial_result;
    ToolExecutionUpdateEvent(std::string id, std::string name, std::string a,
                             std::string pr,
                             std::source_location loc = std::source_location::current())
        : EventBase(EventType::tool_execution_update),
          tool_call_id(std::move(id)),
          tool_name(std::move(name)),
          args(std::move(a)),
          partial_result(std::move(pr)) {}
};

struct ToolExecutionEndEvent : EventBase {
    static constexpr EventType type = EventType::tool_execution_end;
    std::string tool_call_id;
    std::string tool_name;
    std::shared_ptr<ToolResult> result;
    bool is_error{false};
    ToolExecutionEndEvent(std::string id, std::string name,
                          std::shared_ptr<ToolResult> res, bool err,
                          std::source_location loc = std::source_location::current())
        : EventBase(EventType::tool_execution_end),
          tool_call_id(std::move(id)),
          tool_name(std::move(name)),
          result(std::move(res)),
          is_error(err) {}
};

// ─── Event type alias (the top-level variant) ─────────────────────────────

using AgentEvent = std::variant<AgentStartEvent,
                                AgentEndEvent,
                                TurnStartEvent,
                                TurnEndEvent,
                                MessageStartEvent,
                                MessageUpdateEvent,
                                MessageEndEvent,
                                ToolExecutionStartEvent,
                                ToolExecutionUpdateEvent,
                                ToolExecutionEndEvent>;

// ─── Visitor helpers ───────────────────────────────────────────────────────

template<typename F>
    requires(std::is_invocable_v<F, AgentStartEvent> &&
             std::is_invocable_v<F, AgentEndEvent> &&
             std::is_invocable_v<F, TurnStartEvent> &&
             std::is_invocable_v<F, TurnEndEvent> &&
             std::is_invocable_v<F, MessageStartEvent> &&
             std::is_invocable_v<F, MessageUpdateEvent> &&
             std::is_invocable_v<F, MessageEndEvent> &&
             std::is_invocable_v<F, ToolExecutionStartEvent> &&
             std::is_invocable_v<F, ToolExecutionUpdateEvent> &&
             std::is_invocable_v<F, ToolExecutionEndEvent>)
void visit_event(const AgentEvent& ev, F&& visitor) {
    std::visit(std::forward<F>(visitor), ev);
}

template<typename F>
    requires(std::is_invocable_v<F, AgentStartEvent> &&
             std::is_invocable_v<F, AgentEndEvent> &&
             std::is_invocable_v<F, TurnStartEvent> &&
             std::is_invocable_v<F, TurnEndEvent> &&
             std::is_invocable_v<F, MessageStartEvent> &&
             std::is_invocable_v<F, MessageUpdateEvent> &&
             std::is_invocable_v<F, MessageEndEvent> &&
             std::is_invocable_v<F, ToolExecutionStartEvent> &&
             std::is_invocable_v<F, ToolExecutionUpdateEvent> &&
             std::is_invocable_v<F, ToolExecutionEndEvent>)
auto map_event(const AgentEvent& ev, F&& visitor)
    -> decltype(visitor(AgentStartEvent{})) {
    return std::visit(std::forward<F>(visitor), ev);
}

// ─── Debug output ──────────────────────────────────────────────────────────

std::ostream& operator<<(std::ostream& os, const AgentEvent& event);

} // namespace pi::core
