#include "acp/task_events.h"

#include "core/agent_task.h"
#include "core/task_event_json.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <nlohmann/json.hpp> // NOLINT(misc-include-cleaner)

namespace pi::acp {

void TaskEventHub::publish(const core::AgentTaskEvent &event) {
  std::scoped_lock lock(mutex_);
  events_.push_back(
      {.sequence = ++generation_, .event = core::task_event_to_json(event)});
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
