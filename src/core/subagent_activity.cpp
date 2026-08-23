#include "core/subagent_activity.h"
#include "core/agent_task.h"
#include "core/event_types.h"
#include "core/message_types.h"
#include "core/stream_renderer.h"

#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace pi::core {
namespace {
std::string short_line(std::string_view value, std::size_t limit = 160) {
  if (value.size() <= limit)
    return std::string(value);
  return std::string(value.substr(0, limit - 3)) + "...";
}

std::string status_word(AgentTaskStatusKind status) {
  switch (status) {
  case AgentTaskStatusKind::completed:
    return "idle";
  case AgentTaskStatusKind::errored:
    return "errored";
  case AgentTaskStatusKind::interrupted:
    return "interrupted";
  default:
    return std::string(agent_task_status_to_string(status));
  }
}
} // namespace

SubagentActivityBridge::SubagentActivityBridge(WakeCallback wake)
    : wake_(std::move(wake)) {}

void SubagentActivityBridge::enqueue(AgentTaskId id, std::string line) {
  WakeCallback wake;
  {
    std::scoped_lock lock(mutex_);
    auto &history = history_[id];
    history.push_back(line);
    while (history.size() > kMaxActivity)
      history.pop_front();
    pending_.push_back(Activity{std::move(id), std::move(line)});
    wake = wake_;
  }
  if (wake)
    wake();
}

void SubagentActivityBridge::observe(const AgentTaskEvent &event) {
  std::visit(
      [this](const auto &value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, AgentTaskSpawnedEvent>) {
          {
            std::scoped_lock lock(mutex_);
            names_[value.id] = value.task_name;
            statuses_[value.id] = AgentTaskStatusKind::pending_init;
          }
          enqueue(value.id, "→ spawned " + value.task_name);
        } else if constexpr (std::is_same_v<T, AgentTaskStatusChangedEvent>) {
          {
            std::scoped_lock lock(mutex_);
            statuses_[value.id] = value.current;
          }
          if (value.current == AgentTaskStatusKind::completed ||
              value.current == AgentTaskStatusKind::errored ||
              value.current == AgentTaskStatusKind::interrupted) {
            std::string name;
            {
              std::scoped_lock lock(mutex_);
              name = names_[value.id];
            }
            enqueue(value.id, "→ " + name + " " + status_word(value.current));
          }
        } else if constexpr (std::is_same_v<T, AgentTaskClosedEvent>) {
          std::string name;
          {
            std::scoped_lock lock(mutex_);
            name = names_[value.id];
            statuses_.erase(value.id);
          }
          enqueue(value.id, "→ " + name + " closed");
        } else if constexpr (std::is_same_v<T, ChildAgentEvent>) {
          std::visit(
              [this, &value](const auto &child) {
                using E = std::decay_t<decltype(child)>;
                if constexpr (std::is_same_v<E, ToolExecutionStartEvent>)
                  enqueue(value.task_id, "→ " + child.tool_name + " started");
                else if constexpr (std::is_same_v<E, ToolExecutionEndEvent>)
                  enqueue(value.task_id,
                          "→ " + child.tool_name +
                              (child.is_error ? " failed" : " done"));
                else if constexpr (std::is_same_v<E, MessageEndEvent>) {
                  if (const auto *assistant =
                          std::get_if<AssistantMessage>(&child.message)) {
                    std::string text;
                    for (const auto &part : assistant->content) {
                      if (const auto *block = std::get_if<TextContent>(&part)) {
                        text = block->text;
                        break;
                      }
                    }
                    if (!text.empty())
                      enqueue(value.task_id, "→ " + short_line(text));
                  }
                }
              },
              value.event);
        }
      },
      event);
}

void SubagentActivityBridge::drain(Renderer &renderer) {
  std::deque<Activity> pending;
  {
    std::scoped_lock lock(mutex_);
    pending.swap(pending_);
  }
  for (const auto &activity : pending) {
    std::string name;
    {
      std::scoped_lock lock(mutex_);
      name = names_[activity.id];
    }
    renderer.on_command_output(name.empty() ? activity.line
                                            : activity.line + "\n");
  }
}

std::vector<std::string>
SubagentActivityBridge::recent(const AgentTaskId &id) const {
  std::scoped_lock lock(mutex_);
  const auto it = history_.find(id);
  if (it == history_.end())
    return {};
  return {it->second.begin(), it->second.end()};
}

std::string SubagentActivityBridge::summary() const {
  std::scoped_lock lock(mutex_);
  std::size_t running = 0;
  std::size_t idle = 0;
  for (const auto &[id, status] : statuses_) {
    (void)id;
    if (status == AgentTaskStatusKind::running)
      ++running;
    else if (status == AgentTaskStatusKind::completed ||
             status == AgentTaskStatusKind::idle)
      ++idle;
  }
  if (running == 0 && idle == 0)
    return {};
  return std::to_string(running) + " running, " + std::to_string(idle) +
         " idle";
}

} // namespace pi::core
