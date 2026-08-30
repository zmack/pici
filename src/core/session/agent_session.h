#pragma once

#include "core/agent.h"
#include "core/compaction.h"
#include "core/models.h"
#include "core/sandbox.h"
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

class AgentSession {
public:
  // Phase 5 automatic compaction policy (plan §7). Off by default, matching
  // Args::remote_compaction_enabled's existing default and doc comment
  // ("automatic pre-turn compaction only fires when this is explicitly
  // enabled"). threshold_pct is only consulted when enabled is true.
  struct AutoCompactionConfig {
    bool enabled{false};
    double threshold_pct{0.85};
  };

  struct Config {
    Agent::Options agent_options;
    std::shared_ptr<const ModelRegistry> model_registry;
    std::vector<std::shared_ptr<const ToolDefinition>> tools;
    std::shared_ptr<SessionStore> session_store;
    SandboxPolicyPtr sandbox_policy;
    AutoCompactionConfig auto_compaction;
  };

  using EventCallback = std::function<void(const AgentEvent &)>;

  struct RunResult {
    std::optional<std::string> error;
  };

  explicit AgentSession(Config config);
  ~AgentSession() = default;

  AgentSession(const AgentSession &) = delete;
  AgentSession &operator=(const AgentSession &) = delete;
  AgentSession(AgentSession &&) = delete;
  AgentSession &operator=(AgentSession &&) = delete;

  Agent &agent() { return agent_; }
  const Agent &agent() const { return agent_; }

  SessionStore *session_store() { return session_store_.get(); }
  const SessionStore *session_store() const { return session_store_.get(); }

  const std::optional<std::string> &active_session_id() const {
    return active_session_id_;
  }

  SandboxMode sandbox_mode() const;
  void set_sandbox_mode(SandboxMode mode);

  const std::shared_ptr<const ModelRegistry> &model_registry() const {
    return model_registry_;
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
  std::shared_ptr<const ModelRegistry> model_registry_;
  std::optional<std::string> active_session_id_;
  std::optional<std::string> last_warning_;
  AutoCompactionConfig auto_compaction_;
};

} // namespace pi::core
