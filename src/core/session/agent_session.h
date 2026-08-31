#pragma once

// File kept at its historical name (agent_session.h) even though the class
// it declares was renamed to SessionRuntime by
// plans/session-runtime-migration.md Phase 6 -- avoids colliding on
// "session_runtime.h" with the unrelated, CLI-facing
// cli::SessionRuntimeConfig/Bundle/Capabilities factory types declared in
// cli/session_runtime.h (that file builds the Config below; this file is
// what it builds it for).

#include "core/agent.h"
#include "core/agent_task.h"
#include "core/compaction.h"
#include "core/models.h"
#include "core/sandbox.h"
#include "core/session/mailbox_runtime.h"
#include "core/session/session_record.h"
#include "core/session/session_store.h"
#include "core/stream.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pi::core {

// Owns the active session identity, one Agent activation, its
// AgentTaskManager, and its mailbox attachment -- the bundle Phase 2's
// factory used to construct and hand back as three-plus separate pieces
// (see cli/session_runtime.h's SessionRuntimeBundle, which now holds a
// single shared_ptr<SessionRuntime> instead) is, as of Phase 6, one class.
//
// Construction is still two-phase, matching AgentTaskManager's own
// constraint: it takes `SessionRuntime &root` by reference and a
// child-agent Agent::Options whose system_prompt typically isn't finalized
// until the frontend has registered tools on the live agent. The
// constructor builds the Agent and the (unconnected, possibly disabled)
// MailboxRuntime -- neither needs finalized options -- and activate()
// builds AgentTaskManager and connects the mailbox runtime to it once the
// caller is ready. A SessionRuntime that never calls activate() simply has
// no task manager (task_manager() returns null) and an unconnected mailbox
// runtime -- that's the correct, unchanged shape for e.g. ACP's per-run
// sessions, which have never supported subagent delegation or mailbox
// delivery (SessionRuntimeCapabilities keeps those off for ACP).
class SessionRuntime {
public:
  // Phase 5 automatic compaction policy (plan §7 of
  // plans/server-side-compaction.md -- unrelated numbering to this
  // migration plan's own phases). Off by default, matching
  // Args::remote_compaction_enabled's existing default and doc comment
  // ("automatic pre-turn compaction only fires when this is explicitly
  // enabled"). threshold_pct is only consulted when enabled is true.
  struct AutoCompactionConfig {
    bool enabled{false};
    double threshold_pct{0.85};
  };

  struct Config {
    Agent::Options agent_options;
    std::shared_ptr<const ModelCatalog> model_catalog;
    std::vector<std::shared_ptr<const ToolDefinition>> tools;
    std::shared_ptr<SessionStore> session_store;
    SandboxPolicyPtr sandbox_policy;
    AutoCompactionConfig auto_compaction;
    // Optional: constructs the owned (unconnected until activate())
    // MailboxRuntime. Null for every capability-gated caller (ACP always
    // passes null here; see SessionRuntimeCapabilities::enable_mailbox).
    std::shared_ptr<MailboxCoordinator> mailbox;
  };

  using EventCallback = std::function<void(const AgentEvent &)>;

  struct RunResult {
    std::optional<std::string> error;
  };

  explicit SessionRuntime(Config config);
  ~SessionRuntime();

  SessionRuntime(const SessionRuntime &) = delete;
  SessionRuntime &operator=(const SessionRuntime &) = delete;
  SessionRuntime(SessionRuntime &&) = delete;
  SessionRuntime &operator=(SessionRuntime &&) = delete;

  Agent &agent() { return agent_; }
  const Agent &agent() const { return agent_; }

  SessionStore *session_store() { return session_store_.get(); }
  const SessionStore *session_store() const { return session_store_.get(); }

  const std::optional<std::string> &active_session_id() const {
    return active_session_id_;
  }

  SandboxMode sandbox_mode() const;
  void set_sandbox_mode(SandboxMode mode);

  const std::shared_ptr<const ModelCatalog> &model_catalog() const {
    return model_catalog_;
  }
  ModelResolution resolve_model(const ModelSelection &selection) const;
  ModelSwitchResult set_model(Model model, ThinkingLevel thinking);
  const std::optional<std::string> &last_warning() const {
    return last_warning_;
  }

  std::optional<SessionRecord>
  load_session(const std::string &session_id) const;

  void activate_session(const SessionRecord &record);
  bool activate_session(const std::string &session_id);

  std::string open_session(std::string session_id, SessionHeader header);
  std::string create_session(SessionHeader header);
  std::string fork_session(SessionHeader header);

  // Apply a transcript truncation to the active session and persist it as a
  // replayable journal operation.
  bool truncate_active_session(std::size_t through);

  RunResult run_prompt(std::string prompt, const EventCallback &callback = {});
  RunResult run_messages(std::vector<AgentInput> messages,
                         const EventCallback &callback = {});

  struct CompactionRunResult {
    bool success{false};
    bool unsupported{false};
    bool cancelled{false};
    std::optional<std::string> error;
    std::size_t retained_message_count{0};
  };

  // Drains agent_.compact(trigger), and on the `complete` CompactionEvent
  // performs the durable-write-then-install step: writes the replayable
  // compaction journal record via SessionStore::append_compaction, and only
  // if that succeeds, installs the replacement transcript via
  // agent_.commit_compaction(). This is the drain-thread persistence path
  // required by plans/server-side-compaction.md §5 — the durable write
  // happens here (on whichever thread calls this method), never inside
  // agent_.compact()'s own worker thread.
  CompactionRunResult
  compact_active_session(CompactionTrigger trigger = CompactionTrigger::manual,
                         const EventCallback &callback = {});

  // Phase B of construction (plans/session-runtime-migration.md Phase 6):
  // builds AgentTaskManager (fanning out extra_task_event_callback alongside
  // the owned mailbox runtime's own task-event callback -- both must be
  // known before AgentTaskManager's single construction-time callback
  // parameter is set) and connects the mailbox runtime to it, forwarding
  // wake_root. Populates task_manager() and finishes wiring
  // mailbox_runtime() in place. Must be called at most once; calling it
  // twice on the same SessionRuntime is a caller bug (asserted).
  void activate(Agent::Options child_options,
                AgentTaskManager::Limits limits = {},
                AgentTaskManager::ChildWriteTools child_write_tools =
                    AgentTaskManager::ChildWriteTools::none,
                AgentTaskEventCallback extra_task_event_callback = {},
                std::function<void()> wake_root = {});

  MailboxRuntime &mailbox_runtime() { return mailbox_runtime_; }
  const MailboxRuntime &mailbox_runtime() const { return mailbox_runtime_; }

  // Null until activate() is called.
  const std::shared_ptr<AgentTaskManager> &task_manager() const {
    return task_manager_;
  }

private:
  void activate_session_state(std::string session_id,
                              std::vector<Message> messages,
                              std::optional<std::string> session_name = {});

  // Shared drain loop for agent_.prompt()/agent_.continue_(): persists
  // MessageEndEvent to the session store, forwards every event to
  // `callback`, waits for idle, and aborts a still-streaming agent on an
  // exception (mirrors the pre-Phase-5 body of run_messages exactly).
  RunResult
  drain_agent_stream(EventStream<AgentEvent, std::vector<Message>> stream,
                     const EventCallback &callback);

  // Phase 5 context-window-error retry (plan §7): if `result` carries an
  // error that looks like a provider context-window failure, automatic
  // compaction is enabled, and the model supports remote compaction, runs
  // at most one compact-then-continue retry and returns its result instead.
  // Sets `retried` to true when a retry was attempted (successfully or not)
  // so the caller does not also run the ordinary post-turn threshold check
  // immediately afterward. The degenerate case (no prior assistant/tool
  // turn to discard) fails with a distinct, actionable error instead of
  // spending a network round-trip on a no-op compaction.
  RunResult maybe_retry_after_context_window_error(
      RunResult result, const EventCallback &callback, bool &retried);

  // Phase 5 automatic pre-turn trigger (plan §7): after a turn completes
  // cleanly, compacts once if the estimated context usage has crossed the
  // configured threshold. Never throws; a failed automatic attempt is
  // reported through `callback`'s CompactionEvent but does not fail the
  // turn that already completed successfully.
  void maybe_auto_compact_after_turn(const EventCallback &callback);

  Agent agent_;
  std::shared_ptr<SessionStore> session_store_;
  SandboxPolicyPtr sandbox_policy_;
  std::shared_ptr<const ModelCatalog> model_catalog_;
  std::optional<std::string> active_session_id_;
  std::optional<std::string> last_warning_;
  AutoCompactionConfig auto_compaction_;

  // Declaration order below is load-bearing: members destruct in reverse
  // declaration order, so task_manager_ (child tasks) is torn down first,
  // then mailbox_runtime_ (detaching its observer) second, then agent_/the
  // rest of this session's own state last -- see
  // core/session/mailbox_runtime.h's "declare it after SessionRuntime and
  // before AgentTaskManager" comment and the "Destruction follows inverse
  // dependency order" note in docs/architecture-lexicon.md. Do not reorder
  // these two fields without re-checking that constraint: getting it wrong
  // fails silently at teardown, not at compile time.
  MailboxRuntime mailbox_runtime_;
  std::shared_ptr<AgentTaskManager> task_manager_;
};

} // namespace pi::core
