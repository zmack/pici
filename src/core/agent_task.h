#pragma once

#include "core/agent.h"
#include "core/event_types.h"
#include "core/memory_stats.h"
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
#include <utility>
#include <variant>
#include <vector>

namespace pi::core {

// Per-content-block-kind JSON wire-size split of a session transcript
// (plan: session-memory-stats.md §Design 3). These are escaped-JSON wire
// bytes — what the provider request serializes and what tokens are charged
// for — NOT heap bytes. See the /memory command's composition panel.
struct SessionCompositionReport {
  std::size_t message_count{0};
  std::size_t transcript_bytes{0};
  std::size_t text_bytes{0};
  std::size_t tool_use_bytes{0};
  std::size_t tool_result_bytes{0};
  std::size_t other_bytes{0};
};

// Allocator-level heap attribution for one session (plan §Design 2/4):
// bytes currently allocated live out of the session's jemalloc arena.
// nullopt when memory stats are unavailable (glibc allocator, no
// PI_CPP_MEMSTATS build). Independent of SessionCompositionReport — wire
// bytes vs. heap bytes, never nested (§Design 3).
struct SessionHeapReport {
  std::string label;
  std::optional<ArenaStats> arena;
};

// Single-pass JSON-wire-size composition of a transcript. Shared by
// AgentTaskManager::composition_report[s]() and the root session's report
// built in main.cpp.
SessionCompositionReport
composition_report_for_messages(const std::vector<Message> &messages);

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

// Live context-size observability for a child task. Populated by
// AgentTaskManager::snapshot() so a parent can watch a child's context grow
// (message bytes, last assistant-turn token usage, declared model window)
// and decide when the child should be closed/discarded.
struct AgentTaskContextInfo {
  std::size_t message_count{0};
  std::size_t context_bytes{0};
  std::uint64_t last_input_tokens{0};
  std::uint64_t last_output_tokens{0};
  std::uint64_t total_tokens{0};
  std::optional<std::uint64_t> context_window;
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
  // Live context-size info; nullopt only if task state is unavailable.
  std::optional<AgentTaskContextInfo> context_info;
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
  AgentTaskError(AgentTaskErrorKind kind, const std::string &message)
      : std::runtime_error(message), kind_(kind) {}

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
    std::size_t max_message_bytes{static_cast<std::size_t>(256 * 1024)};
    std::size_t max_output_bytes{static_cast<std::size_t>(256 * 1024)};
    std::size_t max_context_bytes{static_cast<std::size_t>(2 * 1024 * 1024)};
    std::chrono::milliseconds max_wait{std::chrono::seconds(60)};
  };

  using EventCallback = std::function<void(const AgentTaskEvent &)>;
  using RegisterEndpointCallback = std::function<AgentRuntimeIdentity(
      const AgentTaskId &, const std::string &,
      const std::optional<AgentTaskId> &)>;
  using UnregisterEndpointCallback = std::function<void(const AgentTaskId &)>;

  AgentTaskManager(AgentSession &root, Agent::Options child_options);
  AgentTaskManager(AgentSession &root, Agent::Options child_options,
                   Limits limits, EventCallback on_event = {});
  ~AgentTaskManager() noexcept;

  AgentTaskManager(const AgentTaskManager &) = delete;
  AgentTaskManager &operator=(const AgentTaskManager &) = delete;

  AgentTaskSnapshot spawn(const SpawnAgentRequest &request);

  // Endpoint registration is called without the task-manager mutex and must
  // complete before a child session or runner is made visible.
  void
  set_endpoint_registration(RegisterEndpointCallback register_endpoint,
                            UnregisterEndpointCallback unregister_endpoint);

  std::optional<AgentTaskSnapshot> get(const AgentTaskId &target) const;
  std::vector<AgentTaskSnapshot>
  list(std::optional<std::string_view> task_path_prefix = {}) const;

  AgentTaskSnapshot send_message(const AgentTaskId &target, Message message);
  AgentTaskSnapshot steer_envelopes(const AgentTaskId &target,
                                    std::vector<AgentMessageEnvelope> messages);
  void drop_mailbox_envelopes();
  AgentTaskSnapshot follow_up(const AgentTaskId &target,
                              const Message &message);
  AgentTaskSnapshot interrupt(const AgentTaskId &target,
                              AgentInterruptReason reason);
  AgentTaskSnapshot close(const AgentTaskId &target);

  // Content-composition report for one live task (§Design 3). Reads the
  // task's transcript in a single pass; safe for any live task id including
  // "root". Throws AgentTaskError(not_found) for unknown ids.
  SessionCompositionReport composition_report(const AgentTaskId &target) const;

  // Content-composition reports for every live task (root included), ordered
  // by task_path like list(). Never throws.
  std::vector<std::pair<std::string, SessionCompositionReport>>
  composition_reports() const;

  // Allocator-level per-session accounting. The root arena is acquired once
  // and lives for the whole process; child tasks each acquire an arena
  // before their AgentSession is constructed and release it back to the
  // recycle pool after close. Empty when memory stats are unavailable.
  std::vector<SessionHeapReport> heap_reports() const;

  // Acquire (first call only) the root arena and bind it to the calling
  // thread — invoke on the main thread before the interactive loop so all
  // its later allocations land in "root" instead of "shared" (§Design 2).
  // Idempotent; no-op when memory stats are unavailable.
  void bind_root_arena();

  AgentWaitResult wait(const AgentWaitRequest &request,
                       std::stop_token stop_token = {}) const;

  void shutdown();
  bool is_shutting_down() const;
  std::size_t active_executions() const;
  std::size_t resident_tasks() const;

private:
  struct Task;

  // ─── Per-session memory accounting (plan: session-memory-stats.md) ──────
  //
  // Each session owns a dedicated jemalloc arena so allocations made by the
  // threads actually running its turns are attributed to it rather than
  // smeared into a process-wide "shared" bucket. Three wiring rules, all
  // from §Design 2:
  //
  // 1. Bind BEFORE construction. make_task() acquires the child's arena and
  //    binds it on the spawning thread before owned_session is constructed,
  //    so inherit_arena() captures it for every thread spawned downstream.
  // 2. Inherit across EVERY spawn point in the turn path — the runner jthread
  //    here, Agent::launch_worker_locked, and EventStream::start_worker. A
  //    turn does not run on the thread that requested it; a missed wrap site
  //    silently drops that work into "shared" instead of erroring.
  // 3. No destroy, only purge-and-recycle. release_task_arena() runs strictly
  //    after the runner thread is joined in close_tasks(); jemalloc's
  //    arena.<i>.destroy contract is unsatisfiable here because parents read
  //    child results after close.
  static void release_task_arena(Task &task);
  struct WorkItem {
    std::vector<AgentMessageEnvelope> messages;
    bool mailbox_delivery{false};
    std::optional<AgentTaskStatusKind> previous_status;
    std::optional<AgentTaskResult> previous_result;
  };

  AgentSession &root_;
  Agent::Options child_options_;
  Limits limits_;
  EventCallback on_event_;
  RegisterEndpointCallback register_endpoint_;
  UnregisterEndpointCallback unregister_endpoint_;

  mutable std::mutex mutex_;
  mutable std::condition_variable_any changed_;
  std::unordered_map<AgentTaskId, std::shared_ptr<Task>> tasks_;
  std::set<std::string> pending_task_paths_;
  std::unordered_map<AgentTaskId, std::size_t> pending_children_;
  std::uint64_t generation_{0};
  std::uint64_t next_id_{1};
  std::size_t active_executions_{0};
  std::size_t pending_spawns_{0};
  bool shutting_down_{false};
  std::stop_source shutdown_source_;

  // Root session's dedicated arena (§Design 2/4). Acquired exactly once —
  // by bind_root_arena() or lazily by heap_reports(), whichever comes first
  // — and bound on the main thread for the life of the process. mutable so
  // the const heap_reports() can perform lazy acquisition under mutex_;
  // nullopt when memory stats are unavailable.
  mutable std::optional<SessionArena> root_arena_;

  std::shared_ptr<Task> find_task_locked(const AgentTaskId &target) const;
  static AgentTaskSnapshot snapshot(const std::shared_ptr<Task> &task);
  void touch_locked(const std::shared_ptr<Task> &task);
  void emit(const AgentTaskEvent &event) const;
  void run_task(const std::shared_ptr<Task> &task,
                const std::stop_token &stop_token);
  void execute_work(const std::shared_ptr<Task> &task,
                    std::vector<AgentMessageEnvelope> messages,
                    AgentTaskResult &result, bool &aborted);
  std::vector<Message> inherit_context(const AgentContext &parent,
                                       const ContextInheritance &request) const;
  static std::vector<std::shared_ptr<const ToolDefinition>>
  inherit_tools(const AgentContext &parent,
                const std::vector<std::string> &requested);
  std::shared_ptr<Task> make_task(const SpawnAgentRequest &request,
                                  const std::shared_ptr<Task> &parent,
                                  std::vector<Message> context,
                                  AgentTaskId task_id, std::string task_path,
                                  std::optional<AgentRuntimeIdentity> identity);
  AgentTaskSnapshot close_tasks(std::vector<std::shared_ptr<Task>> tasks);
  static bool valid_task_name(std::string_view name);
};

} // namespace pi::core
