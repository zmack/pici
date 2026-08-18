#pragma once

#include "core/agent.h"
#include "core/compaction.h"
#include "core/models.h"
#include "core/sandbox.h"
#include "core/session/session_record.h"
#include "core/session/session_store.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pi::core {

class AgentSession {
public:
  struct Config {
    Agent::Options agent_options;
    std::shared_ptr<const ModelRegistry> model_registry;
    std::vector<std::shared_ptr<const ToolDefinition>> tools;
    std::shared_ptr<SessionStore> session_store;
    SandboxPolicyPtr sandbox_policy;
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
  RunResult run_messages(std::vector<AgentMessageEnvelope> messages,
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
  Agent agent_;
  std::shared_ptr<SessionStore> session_store_;
  SandboxPolicyPtr sandbox_policy_;
  std::shared_ptr<const ModelRegistry> model_registry_;
  std::optional<std::string> active_session_id_;
  std::optional<std::string> last_warning_;
};

} // namespace pi::core
