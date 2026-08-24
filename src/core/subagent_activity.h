#pragma once

#include "core/agent_task.h"
#include "core/stream_renderer.h"

#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace pi::core {

// Collects the small, user-visible subset of child-agent events.  Agent task
// callbacks run on worker threads, so this class is deliberately a producer
// queue; drain() is only intended to be called by the UI thread.
class SubagentActivityBridge {
public:
  using WakeCallback = std::function<void()>;

  explicit SubagentActivityBridge(WakeCallback wake = {});

  void set_pane_wake(WakeCallback cb);

  void observe(const AgentTaskEvent &event);
  void drain(Renderer &renderer);

  std::vector<std::string> recent(const AgentTaskId &id) const;
  std::vector<SubagentPaneRow> pane_rows() const;
  std::string summary() const;

private:
  struct Activity {
    AgentTaskId id;
    std::string line;
  };

  static constexpr std::size_t kMaxActivity = 50;
  void enqueue(AgentTaskId id, std::string line);

  mutable std::mutex mutex_;
  std::deque<Activity> pending_;
  std::unordered_map<AgentTaskId, std::deque<std::string>> history_;
  std::unordered_map<AgentTaskId, std::string> names_;
  std::unordered_map<AgentTaskId, AgentTaskStatusKind> statuses_;
  WakeCallback wake_;
  WakeCallback pane_wake_;
};

} // namespace pi::core
