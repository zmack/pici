#include "core/task_event_json.h"

#include "core/agent_task.h"
#include "core/child_completion.h"
#include "core/event_json.h"
#include "core/message_types.h"

#include <nlohmann/json.hpp> // NOLINT(misc-include-cleaner)
#include <type_traits>
#include <variant>

namespace pi::core {

// NOLINTNEXTLINE(misc-include-cleaner)
nlohmann::json task_event_to_json(const AgentTaskEvent &event) {
  nlohmann::json result;
  std::visit(
      [&result](const auto &value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, AgentTaskSpawnedEvent>) {
          result = {{"type", "task.spawned"},
                    {"task_id", value.id},
                    {"task_path", value.task_path},
                    {"task_name", value.task_name},
                    {"parent_id", value.parent_id
                                      ? nlohmann::json(*value.parent_id)
                                      : nlohmann::json(nullptr)}};
        } else if constexpr (std::is_same_v<T, AgentTaskStatusChangedEvent>) {
          result = {{"type", "task.status_changed"},
                    {"task_id", value.id},
                    {"previous", agent_task_status_to_string(value.previous)},
                    {"current", agent_task_status_to_string(value.current)}};
        } else if constexpr (std::is_same_v<T, AgentTaskMessageQueuedEvent>) {
          result = {{"type", "task.message_queued"},
                    {"task_id", value.id},
                    {"triggers_turn", value.triggers_turn}};
        } else if constexpr (std::is_same_v<T, AgentTaskInterruptedEvent>) {
          result = {{"type", "task.interrupted"},
                    {"task_id", value.id},
                    {"reason", agent_interrupt_reason_to_string(value.reason)}};
        } else if constexpr (std::is_same_v<T, AgentTaskClosedEvent>) {
          result = {{"type", "task.closed"}, {"task_id", value.id}};
        } else if constexpr (std::is_same_v<T, AgentTaskCompletionEvent>) {
          const auto &c = value.completion;
          result = {{"type", "task.completion"},
                    {"task_id", c.task_id},
                    {"task_path", c.task_path},
                    {"task_name", c.task_name},
                    {"run_generation", c.run_generation},
                    {"status", child_completion_status_to_string(c.status)},
                    {"stop_reason", stop_reason_to_string(c.stop_reason)},
                    {"truncated", c.truncated},
                    {"text", c.text},
                    {"error", c.error ? nlohmann::json(*c.error)
                                      : nlohmann::json(nullptr)}};
        } else if constexpr (std::is_same_v<T, ChildAgentEvent>) {
          result = {{"type", "task.event"},
                    {"task_id", value.task_id},
                    {"event", event_to_json(value.event)}};
        }
      },
      event);
  return result;
}

} // namespace pi::core
