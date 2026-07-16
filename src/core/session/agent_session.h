#pragma once

#include "core/agent.h"
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
    std::vector<std::shared_ptr<const ToolDefinition>> tools;
    std::shared_ptr<SessionStore> session_store;
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

  std::optional<SessionRecord>
  load_session(const std::string &session_id) const;

  void activate_session(const SessionRecord &record);
  bool activate_session(const std::string &session_id);

  std::string open_session(std::string session_id, SessionHeader header);
  std::string create_session(SessionHeader header);
  std::string fork_session(SessionHeader header);

  RunResult run_prompt(std::string prompt, const EventCallback &callback = {});

private:
  void activate_session_state(std::string session_id,
                              std::vector<Message> messages,
                              std::optional<std::string> session_name = {});
  void persist_new_messages(std::size_t previous_message_count,
                            const std::vector<Message> &messages,
                            std::optional<std::string> &error);

  Agent agent_;
  std::shared_ptr<SessionStore> session_store_;
  std::optional<std::string> active_session_id_;
};

} // namespace pi::core
