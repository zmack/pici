#pragma once

#include "core/agent.h"
#include "core/event_types.h"
#include "core/session/agent_session.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

namespace pi::core {

using AgentTaskId = std::string;

enum class AgentTaskStatusKind {
  pending_init,
  idle,
  running,
  completed,
  errored,
  interrupted,
  closing,
  shutdown,
};

std::string_view agent_task_status_to_string(AgentTaskStatusKind status);

enum class AgentInterruptReason {
  user,
  parent,
  replacement_task,
  shutdown,
  budget,
  timeout,
};

std::string_view agent_interrupt_reason_to_string(AgentInterruptReason reason);
TurnAbortReason turn_abort_reason(AgentInterruptReason reason);

enum class ContextInheritanceMode {
  none,
  full,
  through_message,
  recent_messages,
};

struct ContextInheritance {
  ContextInheritanceMode mode{ContextInheritanceMode::none};
  std::optional<std::size_t> through;
  std::optional<std::size_t> recent_count;
};

struct SpawnAgentRequest {
  AgentTaskId parent_id;
  std::string task_name;
  std::string prompt;
  ContextInheritance context;
  std::optional<std::string> system_prompt;
  std::optional<std::string> model_spec;
  std::vector<std::string> requested_tools;
  bool allow_subagents{false};
};

struct AgentTaskResult {
  std::string text;
  std::optional<std::string> error;
  StopReason stop_reason{StopReason::stop};
  TokenUsage usage;
  bool truncated{false};
};

struct AgentTaskSnapshot {
  AgentTaskId id;
  std::string task_path;
  std::optional<AgentTaskId> parent_id;
  std::string task_name;
  AgentTaskStatusKind status{AgentTaskStatusKind::pending_init};
  std::optional<AgentTaskResult> result;
  std::size_t child_count{0};
  std::size_t queued_message_count{0};
  std::uint64_t generation{0};
};

enum class AgentTaskErrorKind {
  not_found,
  invalid_state,
  duplicate_name,
  depth_limit,
  execution_limit,
  residency_limit,
  invalid_context,
  invalid_model,
  invalid_tool,
  permission_denied,
  shutting_down,
  internal,
};

std::string_view agent_task_error_code(AgentTaskErrorKind kind);

class AgentTaskError : public std::runtime_error {
public:
  AgentTaskError(AgentTaskErrorKind kind, std::string message)
      : std::runtime_error(std::move(message)), kind_(kind) {}

  AgentTaskErrorKind kind() const noexcept { return kind_; }
  std::string_view code() const noexcept {
    return agent_task_error_code(kind_);
  }

private:
  AgentTaskErrorKind kind_;
};

struct AgentWaitRequest {
  std::vector<AgentTaskId> targets;
  std::uint64_t after_generation{0};
  std::chrono::milliseconds timeout{30000};
};

struct AgentWaitResult {
  bool timed_out{false};
  bool caller_interrupted{false};
  std::uint64_t generation{0};
  std::vector<AgentTaskSnapshot> changed;
};

struct AgentTaskSpawnedEvent {
  AgentTaskId id;
  std::string task_path;
  std::optional<AgentTaskId> parent_id;
  std::string task_name;
};

struct AgentTaskStatusChangedEvent {
  AgentTaskId id;
  AgentTaskStatusKind previous{AgentTaskStatusKind::pending_init};
  AgentTaskStatusKind current{AgentTaskStatusKind::pending_init};
};

struct AgentTaskMessageQueuedEvent {
  AgentTaskId id;
  bool triggers_turn{false};
};

struct AgentTaskInterruptedEvent {
  AgentTaskId id;
  AgentInterruptReason reason{AgentInterruptReason::user};
};

struct AgentTaskClosedEvent {
  AgentTaskId id;
};

struct ChildAgentEvent {
  AgentTaskId task_id;
  AgentEvent event;
};

using AgentTaskEvent =
    std::variant<AgentTaskSpawnedEvent, AgentTaskStatusChangedEvent,
                 AgentTaskMessageQueuedEvent, AgentTaskInterruptedEvent,
                 AgentTaskClosedEvent, ChildAgentEvent>;

class AgentTaskManager {
public:
  struct Limits {
    std::size_t max_active_executions{4};
    std::size_t max_resident_tasks{8};
    std::size_t max_nesting_depth{2};
    std::size_t max_direct_children{4};
    std::size_t max_mailbox_items{32};
    std::size_t max_message_bytes{256 * 1024};
    std::size_t max_output_bytes{256 * 1024};
    std::size_t max_context_bytes{2 * 1024 * 1024};
    std::chrono::milliseconds max_wait{std::chrono::seconds(60)};
  };

  using EventCallback = std::function<void(const AgentTaskEvent &)>;

  AgentTaskManager(AgentSession &root, Agent::Options child_options);
  AgentTaskManager(AgentSession &root, Agent::Options child_options,
                   Limits limits, EventCallback on_event = {});
  ~AgentTaskManager() noexcept;

  AgentTaskManager(const AgentTaskManager &) = delete;
  AgentTaskManager &operator=(const AgentTaskManager &) = delete;

  AgentTaskSnapshot spawn(const SpawnAgentRequest &request);

  std::optional<AgentTaskSnapshot> get(const AgentTaskId &target) const;
  std::vector<AgentTaskSnapshot>
  list(std::optional<std::string_view> task_path_prefix = {}) const;

  AgentTaskSnapshot send_message(const AgentTaskId &target, Message message);
  AgentTaskSnapshot follow_up(const AgentTaskId &target,
                              const Message &message);
  AgentTaskSnapshot interrupt(const AgentTaskId &target,
                              AgentInterruptReason reason);
  AgentTaskSnapshot close(const AgentTaskId &target);

  AgentWaitResult wait(const AgentWaitRequest &request,
                       std::stop_token stop_token = {}) const;

  void shutdown();
  std::size_t active_executions() const;
  std::size_t resident_tasks() const;

private:
  struct Task;
  struct WorkItem {
    std::string prompt;
  };

  AgentSession &root_;
  Agent::Options child_options_;
  Limits limits_;
  EventCallback on_event_;

  mutable std::mutex mutex_;
  mutable std::condition_variable_any changed_;
  std::unordered_map<AgentTaskId, std::shared_ptr<Task>> tasks_;
  std::uint64_t generation_{0};
  std::uint64_t next_id_{1};
  std::size_t active_executions_{0};
  bool shutting_down_{false};
  std::stop_source shutdown_source_;

  std::shared_ptr<Task> find_task_locked(const AgentTaskId &target) const;
  static AgentTaskSnapshot snapshot(const std::shared_ptr<Task> &task);
  void touch_locked(const std::shared_ptr<Task> &task);
  void emit(const AgentTaskEvent &event) const;
  void run_task(const std::shared_ptr<Task> &task,
                const std::stop_token &stop_token);
  void execute_work(const std::shared_ptr<Task> &task, std::string prompt,
                    AgentTaskResult &result, bool &aborted);
  std::vector<Message> inherit_context(const AgentContext &parent,
                                       const ContextInheritance &request) const;
  static std::vector<std::shared_ptr<const ToolDefinition>>
  inherit_tools(const AgentContext &parent,
                const std::vector<std::string> &requested);
  std::shared_ptr<Task> make_task(const SpawnAgentRequest &request,
                                  const std::shared_ptr<Task> &parent,
                                  std::vector<Message> context);
  AgentTaskSnapshot close_tasks(std::vector<std::shared_ptr<Task>> tasks);
  static bool valid_task_name(std::string_view name);
};

} // namespace pi::core
