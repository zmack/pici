#pragma once

#include "core/agent_loop.h"
#include "core/message_types.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pi::core {

enum class ChildCompletionStatus { completed, errored, interrupted };

std::string_view
child_completion_status_to_string(ChildCompletionStatus status);

// Value (Delegation, TaskTree -> parent Agent): an immutable snapshot of one
// child run's terminal outcome. TaskTree stays the authority for child status
// and the full result; this carries bounded text only. Identity is
// (task_id, run_generation): run_generation counts terminal run transitions
// of that task, so the n-th run of a followed-up child never aliases the
// (n-1)-th.
struct ChildCompletion {
  std::string task_id;
  std::string task_path;
  std::string task_name;
  std::uint64_t run_generation{0};
  ChildCompletionStatus status{ChildCompletionStatus::completed};
  std::string text;
  std::optional<std::string> error;
  StopReason stop_reason{StopReason::stop};
  bool truncated{false};
};

// Cuts `text` to at most `max_bytes` without splitting a UTF-8 sequence.
// Returns true when anything was removed.
bool truncate_utf8(std::string &text, std::size_t max_bytes);

// Attributed user-input envelope for one completion (source
// child_completion). The transcript only ever sees a completion through this.
AgentInput child_completion_input(const ChildCompletion &completion);

// Attributed note that `dropped` completions were rejected as overflow.
AgentInput child_completion_dropped_input(std::size_t dropped);

// Agent-owned input state (not a peer object): the pending completions for
// one Agent plus the bounded dedup/cancel records. Not thread-safe; Agent
// guards it with its own mutex.
class ChildCompletionInbox {
public:
  static constexpr std::size_t kMaxPending = 16;
  static constexpr std::size_t kMaxRemembered = 256;

  enum class Admission { admitted, duplicate, cancelled, closed, overflow };

  struct Batch {
    std::vector<ChildCompletion> completions;
    std::size_t dropped{0};
  };

  Admission admit(ChildCompletion completion);
  Batch drain();
  // Drops pending entries for `task_id` and rejects later arrivals for it.
  std::size_t cancel(const std::string &task_id);
  // Drops everything and forgets all records; reopens admission.
  void clear();
  // Rejects all further admission (shutdown).
  void close() { closed_ = true; }

  std::size_t pending() const { return pending_.size(); }

private:
  using Key = std::pair<std::string, std::uint64_t>;

  std::deque<ChildCompletion> pending_;
  std::deque<Key> seen_order_;
  std::set<Key> seen_;
  std::deque<std::string> cancelled_order_;
  std::set<std::string> cancelled_;
  std::size_t dropped_{0};
  bool closed_{false};
};

} // namespace pi::core
