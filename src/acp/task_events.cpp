#include "acp/task_events.h"

#include "core/agent_task.h"
#include "core/event_json.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <nlohmann/json.hpp> // NOLINT(misc-include-cleaner)
#include <type_traits>
#include <variant>

namespace pi::acp {

// NOLINTNEXTLINE(misc-include-cleaner)
nlohmann::json task_event_to_json(const core::AgentTaskEvent &event) {
  nlohmann::json result;
  std::visit(
      [&result](const auto &value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, core::AgentTaskSpawnedEvent>) {
          result = {{"type", "task.spawned"},
                    {"task_id", value.id},
                    {"task_path", value.task_path},
                    {"task_name", value.task_name},
                    {"parent_id", value.parent_id
                                      ? nlohmann::json(*value.parent_id)
                                      : nlohmann::json(nullptr)}};
        } else if constexpr (std::is_same_v<
                                 T, core::AgentTaskStatusChangedEvent>) {
          result = {
              {"type", "task.status_changed"},
              {"task_id", value.id},
              {"previous", core::agent_task_status_to_string(value.previous)},
              {"current", core::agent_task_status_to_string(value.current)}};
        } else if constexpr (std::is_same_v<
                                 T, core::AgentTaskMessageQueuedEvent>) {
          result = {{"type", "task.message_queued"},
                    {"task_id", value.id},
                    {"triggers_turn", value.triggers_turn}};
        } else if constexpr (std::is_same_v<T,
                                            core::AgentTaskInterruptedEvent>) {
          result = {
              {"type", "task.interrupted"},
              {"task_id", value.id},
              {"reason", core::agent_interrupt_reason_to_string(value.reason)}};
        } else if constexpr (std::is_same_v<T, core::AgentTaskClosedEvent>) {
          result = {{"type", "task.closed"}, {"task_id", value.id}};
        } else if constexpr (std::is_same_v<T, core::ChildAgentEvent>) {
          result = {{"type", "task.event"},
                    {"task_id", value.task_id},
                    {"event", core::event_to_json(value.event)}};
        }
      },
      event);
  return result;
}

void TaskEventHub::publish(const core::AgentTaskEvent &event) {
  std::scoped_lock lock(mutex_);
  events_.push_back(
      {.sequence = ++generation_, .event = task_event_to_json(event)});
  if (events_.size() > kMaxEvents)
    events_.pop_front();
  condition_.notify_all();
}

TaskEventBatch
TaskEventHub::wait_since(std::uint64_t after_generation,
                         std::chrono::milliseconds timeout) const {
  timeout = std::clamp(timeout, std::chrono::milliseconds::zero(),
                       std::chrono::milliseconds(60000));
  std::unique_lock lock(mutex_);
  const auto changed = [&] { return generation_ > after_generation; };
  const bool observed =
      changed() || condition_.wait_for(lock, timeout, changed);

  TaskEventBatch result;
  result.generation = generation_;
  result.timed_out = !observed;
  for (const auto &event : events_)
    if (event.sequence > after_generation)
      result.events.push_back(event);
  return result;
}

} // namespace pi::acp
