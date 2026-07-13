#include "core/session/agent_session.h"

#include "core/message_types.h"
#include "core/session/session_id.h"
#include "core/session/session_record.h"

#include <cstddef>
#include <exception>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pi::core {

AgentSession::AgentSession(Config config)
    : agent_(config.agent_options),
      session_store_(std::move(config.session_store)) {
  agent_.set_tools(std::move(config.tools));
}

std::optional<SessionRecord>
AgentSession::load_session(const std::string &session_id) const {
  if (!session_store_)
    return std::nullopt;
  return session_store_->load(session_id);
}

void AgentSession::activate_session(const SessionRecord &record) {
  activate_session_state(record.header.id, record.messages);
}

bool AgentSession::activate_session(const std::string &session_id) {
  auto record = load_session(session_id);
  if (!record)
    return false;
  activate_session(*record);
  return true;
}

std::string AgentSession::open_session(std::string session_id,
                                       SessionHeader header) {
  if (session_id.empty())
    session_id = header.id;
  if (session_id.empty())
    session_id = generate_session_id();

  if (activate_session(session_id))
    return session_id;

  header.id = session_id;
  return create_session(std::move(header));
}

std::string AgentSession::create_session(SessionHeader header) {
  if (header.id.empty())
    header.id = generate_session_id();

  auto session_id = header.id;
  if (session_store_)
    session_id = session_store_->create(header);

  const auto active_id = session_id;
  activate_session_state(std::move(session_id), {});
  return active_id;
}

std::string AgentSession::fork_session(SessionHeader header) {
  if (header.id.empty())
    header.id = generate_session_id();

  auto session_id = header.id;
  if (session_store_)
    session_id = session_store_->create(header);

  active_session_id_ = session_id;
  agent_.state().set_session_id(session_id);
  return session_id;
}

AgentSession::RunResult
AgentSession::run_prompt(std::string prompt, const EventCallback &callback) {
  RunResult result;
  const auto previous_message_count = agent_.state().messages().size();
  bool started = false;

  try {
    auto stream = agent_.prompt(std::move(prompt));
    started = true;
    for (const auto &event : stream) {
      if (callback)
        callback(event);
    }
    agent_.wait_for_idle();
  } catch (const std::exception &e) {
    result.error = e.what();
  } catch (...) {
    result.error = "Unknown agent session error";
  }

  if (started && agent_.is_streaming()) {
    agent_.abort();
    agent_.wait_for_idle();
  }

  if (!result.error)
    result.error = agent_.state().error_message();

  persist_new_messages(previous_message_count, agent_.state().messages(),
                       result.error);
  return result;
}

void AgentSession::activate_session_state(std::string session_id,
                                          std::vector<Message> messages) {
  agent_.wait_for_idle();
  agent_.state().set_messages(std::move(messages));
  agent_.state().set_session_id(session_id);
  active_session_id_ = std::move(session_id);
}

void AgentSession::persist_new_messages(std::size_t previous_message_count,
                                        const std::vector<Message> &messages,
                                        std::optional<std::string> &error) {
  if (!session_store_ || !active_session_id_ ||
      previous_message_count >= messages.size())
    return;

  try {
    for (std::size_t i = previous_message_count; i < messages.size(); ++i)
      session_store_->append_message(*active_session_id_, messages[i]);
  } catch (const std::exception &e) {
    if (!error)
      error = e.what();
  } catch (...) {
    if (!error)
      error = "Unknown session persistence error";
  }
}

} // namespace pi::core
