#pragma once

#include <concepts>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/agent_loop.h"
#include "core/agent_runtime_identity.h"
#include "core/agent_state.h"
#include "core/auth_types.h"
#include "core/compaction.h"
#include "core/event_types.h"
#include "core/message_types.h"
#include "core/stream.h"

namespace pi::core {

class ModelCatalog;
class TaskTree;

struct ModelSwitchResult {
  Model previous;
  Model current;
  ThinkingLevel thinking_level{ThinkingLevel::off};
  std::optional<std::string> warning;
  // Set only by SessionRuntime::set_model() when it rejects a switch for
  // missing provider authentication before ever calling Agent::set_model();
  // Agent itself never populates this.
  std::optional<std::string> error;
};

class Agent {
public:
  struct Options {
    std::string system_prompt;
    Model model;
    std::shared_ptr<const ModelCatalog> model_catalog;
    ThinkingLevel thinking_level{ThinkingLevel::off};
    ToolExecutionMode tool_execution{ToolExecutionMode::parallel};
    Options() = default;

    std::optional<double> temperature;
    std::optional<std::uint32_t> max_tokens;
    std::optional<std::string> cache_retention;
    std::optional<std::string> session_id;
    std::optional<AgentRuntimeIdentity> runtime_identity;
    Transport transport{Transport::auto_transport};
    std::map<std::string, std::string> headers;
    std::optional<std::uint32_t> timeout_ms;
    std::optional<std::uint32_t> max_retries;
    std::optional<std::uint32_t> max_retry_delay_ms;
    nlohmann::json metadata;
    decltype(StreamOptions::on_payload) on_payload;
    decltype(StreamOptions::on_response) on_response;
    std::shared_ptr<StreamDiagnostics> diagnostics;
    bool verbose{false};

    // LLM
    std::function<std::optional<RequestAuth>(std::string_view provider)>
        get_auth;

    // Legacy API-key callback retained while providers migrate to get_auth.
    std::function<std::optional<std::string>(std::string_view provider)>
        get_api_key;

    // Context transform
    std::function<std::vector<Message>(const std::vector<Message> &,
                                       std::stop_token)>
        transform_context;

    std::function<std::optional<std::vector<Message>>(
        const AgentContext &, std::size_t estimated_tokens, std::stop_token)>
        prepare_context;

    // Message conversion
    std::function<std::vector<Message>(const std::vector<Message> &)>
        convert_to_llm;

    // Called with an immutable copy of the request-ready context immediately
    // before the LLM client is invoked.
    std::function<void(const AgentContext &)> on_effective_context;

    // Observes every typed event emitted by the agent loop. Observers must
    // treat the event as immutable and should not perform blocking work.
    std::function<void(const AgentEvent &)> on_event;

    // Tool callbacks
    std::function<std::optional<BeforeToolCallResult>(
        const BeforeToolCallContext &, std::stop_token)>
        before_tool_call;

    std::function<std::optional<AfterToolCallResult>(
        const AfterToolCallContext &, std::stop_token)>
        after_tool_call;

    // Stop condition
    std::function<bool(const Message &, const std::vector<ToolResultMessage> &,
                       const AgentContext &)>
        should_stop_after_turn;

    // Steering / follow-up
    std::function<std::vector<Message>()> get_steering_messages;
    std::function<std::vector<Message>()> get_follow_up_messages;
  };

  Agent();
  explicit Agent(const Options &options);
  ~Agent();

  Agent(const Agent &) = delete;
  Agent &operator=(const Agent &) = delete;
  Agent(Agent &&) = delete;
  Agent &operator=(Agent &&) = delete;

  AgentState &state() { return state_; }
  const AgentState &state() const { return state_; }

  std::optional<AgentRuntimeIdentity> runtime_identity() const {
    return state_.runtime_identity();
  }
  void set_runtime_identity(std::optional<AgentRuntimeIdentity> identity) {
    state_.set_runtime_identity(std::move(identity));
  }

  // Return an immutable snapshot of the current raw agent context.
  AgentContext context_snapshot() const;

  void add_tool(std::shared_ptr<const ToolDefinition> tool);
  void set_tools(std::vector<std::shared_ptr<const ToolDefinition>> tools);

  // Start a new prompt from text
  EventStream<AgentEvent, std::vector<Message>>
  prompt(std::string text, std::vector<ImageContent> images = {});

  // Start a new prompt from messages
  EventStream<AgentEvent, std::vector<Message>>
  prompt(std::vector<Message> messages);

  EventStream<AgentEvent, std::vector<Message>>
  prompt(std::vector<AgentInput> messages);

  // Continue from current transcript
  EventStream<AgentEvent, std::vector<Message>> continue_();

  // Queue a message to be injected after the current turn
  void steer(std::vector<Message> messages);
  void steer_envelopes(std::vector<AgentInput> messages);
  void clear_steering_queue();
  void clear_mailbox_steering_queue();

  // Queue a message to run only after the agent would otherwise stop
  void follow_up(std::vector<Message> messages);
  void clear_follow_up_queue();

  void interrupt(TurnAbortReason reason);
  void abort();
  void reset();

  // Change the model only between turns. If persist is supplied it runs while
  // the lifecycle lock is held and before the in-memory state is committed.
  ModelSwitchResult set_model(Model model, ThinkingLevel thinking,
                              const std::function<void()> &persist = {});

  // Restore durable session state as one idle-only lifecycle transition. This
  // intentionally has no persistence callback: loading a session must never
  // append metadata to whichever session happened to be active previously.
  ModelSwitchResult restore_session(Model model, ThinkingLevel thinking,
                                    std::vector<Message> messages,
                                    std::string session_id,
                                    std::optional<std::string> session_name);
  void set_session_identity(std::string session_id,
                            std::optional<std::string> session_name = {});

  // Start a server-side compaction attempt. Shares the idle-transition guard
  // with set_model/restore_session (see with_idle_transition below), so it
  // is rejected outright while streaming, while tool calls are pending, or
  // while steering/follow-up work is queued. A prompt that starts after this
  // call's snapshot but before commit_compaction() installs the replacement
  // does not corrupt anything: commit_compaction() rechecks the transcript
  // epoch and fails closed (stale snapshot) instead of silently discarding
  // the prompt's messages. See core/compaction.h and
  // SessionRuntime::compact_active_session for the intended drain-side wiring
  // — the durable journal write and the memory install must both happen on
  // whichever thread drains this stream's `complete` CompactionEvent, never
  // from inside this call's own worker thread.
  EventStream<AgentEvent, CompactionOutcome>
  compact(CompactionTrigger trigger = CompactionTrigger::manual);

  struct CompactionCommitResult {
    bool installed{false};
    std::optional<std::string> error;
  };

  // Drain-side commit step for a compaction's `complete` CompactionEvent.
  // Rechecks `expected_epoch` against the live transcript epoch under the
  // same idle-transition lock the snapshot used; if it no longer matches,
  // installs nothing and reports a stale-snapshot error. Otherwise runs
  // `persist` (e.g. the durable journal write) while still holding the
  // lock and, only if `persist` does not throw, installs `replacement` as
  // the new transcript. Mirrors set_model's `persist`-runs-before-commit
  // contract.
  CompactionCommitResult
  commit_compaction(std::uint64_t expected_epoch,
                    std::vector<Message> replacement,
                    const std::function<void()> &persist = {});

  // Wait for the agent to become idle
  void wait_for_idle();

  // Check if agent is currently processing
  bool is_streaming() const { return state_.is_streaming(); }

  // Null until a caller (SessionRuntime::activate()) attaches one. TaskTree
  // is constructed elsewhere -- it needs a ChildSessionFactory that only
  // process/session composition can build (core/agent_task.h would have to
  // be included here to construct it directly, which would circularly
  // include this header back) -- so Agent only owns and exposes it.
  const std::shared_ptr<TaskTree> &task_tree() const { return task_tree_; }

  // Attaches this agent's delegation hierarchy. Must be called at most once;
  // calling it twice is a caller bug (throws std::logic_error).
  void set_task_tree(std::shared_ptr<TaskTree> tree);

private:
  AgentState state_;
  Options options_;

  // Queue management
  std::mutex steering_mutex_;
  std::vector<AgentInput> steering_queue_;

  std::mutex followup_mutex_;
  std::vector<Message> followup_queue_;

  std::mutex worker_mutex_;
  std::vector<std::jthread> workers_;

  mutable std::mutex interrupt_mutex_;
  std::optional<TurnAbortReason> interrupt_reason_;
  std::optional<TurnAbortReason> pending_interrupt_;

  // Internal helpers
  void join_workers();
  void begin_run();
  void begin_run_locked();
  void launch_worker(std::function<void()> worker);
  void launch_worker_locked(std::function<void()> worker);
  void run_with_lifecycle(const std::function<void(std::stop_token)> &executor);
  AgentContext create_context_snapshot() const;
  AgentLoopConfig create_loop_config();
  void process_event(const AgentEvent &event);

  // Runs `transition` while holding worker_mutex_, after verifying the
  // agent is idle: not streaming, no pending tool calls, and both
  // steering/follow-up queues empty. Shared by set_model, compact(), and
  // commit_compaction() so this four-condition guard exists in exactly one
  // place. Throws std::runtime_error (message prefixed by `operation`,
  // e.g. "model switching requires an idle agent") without invoking
  // `transition` when the agent is not idle.
  void with_idle_transition(std::string_view operation,
                            const std::function<void()> &transition);

  CompactionOptions create_compaction_options();

  // Declared last so it destructs FIRST (reverse declaration order): TaskTree
  // must close and join every child task -- each running its own worker
  // threads against its own child Agent -- before this agent's own state_ and
  // workers_ are torn down, since a child's completion path can still reach
  // back into this agent (e.g. steering the root) while it is being closed.
  // See core/session/session_runtime.h's analogous ordering comment, which
  // this mirrors one level down.
  std::shared_ptr<TaskTree> task_tree_;
};

} // namespace pi::core
