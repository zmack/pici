#pragma once

#include "core/agent_task.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <nlohmann/json.hpp>
#include <vector>

namespace pi::acp {

struct TaskEventRecord {
  std::uint64_t sequence{0};
  nlohmann::json event;
};

struct TaskEventBatch {
  std::uint64_t generation{0};
  bool timed_out{false};
  std::vector<TaskEventRecord> events;
};

nlohmann::json task_event_to_json(const core::AgentTaskEvent &event);

class TaskEventHub {
public:
  void publish(const core::AgentTaskEvent &event);

  TaskEventBatch wait_since(
      std::uint64_t after_generation,
      std::chrono::milliseconds timeout = std::chrono::seconds(30)) const;

private:
  static constexpr std::size_t kMaxEvents = 1024;

  mutable std::mutex mutex_;
  mutable std::condition_variable condition_;
  std::deque<TaskEventRecord> events_;
  std::uint64_t generation_{0};
};

} // namespace pi::acp
