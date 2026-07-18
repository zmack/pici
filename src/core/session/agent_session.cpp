#include "core/session/agent_session.h"

#include "core/event_types.h"
#include "core/message_types.h"
#include "core/session/session_id.h"
#include "core/session/session_record.h"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <optional>
#include <string>
#include <utility>
#include <variant>
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
  activate_session_state(record.header.id, record.messages, record.header.name);
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
  auto session_name = header.name;
  if (session_store_)
    session_id = session_store_->create(header);

  const auto active_id = session_id;
  activate_session_state(std::move(session_id), {}, std::move(session_name));
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
  if (header.name)
    agent_.state().set_session_name(*header.name);
  else
    agent_.state().clear_session_name();
  return session_id;
}

bool AgentSession::truncate_active_session(std::size_t through) {
  if (!active_session_id_)
    return false;

  auto messages = agent_.state().messages();
  messages.resize(std::min(through, messages.size()));
  agent_.state().set_messages(messages);
  if (session_store_)
    session_store_->append_truncate(*active_session_id_, through);
  return true;
}

AgentSession::RunResult
AgentSession::run_prompt(std::string prompt, const EventCallback &callback) {
  RunResult result;
  bool started = false;
  std::optional<std::string> persistence_error;

  auto handle_event = [&](const AgentEvent &event) {
    if (session_store_ && active_session_id_ &&
        std::holds_alternative<MessageEndEvent>(event)) {
      try {
        session_store_->append_message(
            *active_session_id_, std::get<MessageEndEvent>(event).message);
      } catch (const std::exception &e) {
        if (!persistence_error)
          persistence_error = e.what();
      } catch (...) {
        if (!persistence_error)
          persistence_error = "Unknown session persistence error";
      }
    }
    if (callback)
      callback(event);
  };

  try {
    auto stream = agent_.prompt(std::move(prompt));
    started = true;
    for (const auto &event : stream)
      handle_event(event);
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

  if (!result.error && persistence_error)
    result.error = persistence_error;
  return result;
}

void AgentSession::activate_session_state(
    std::string session_id, std::vector<Message> messages,
    std::optional<std::string> session_name) {
  agent_.wait_for_idle();
  agent_.state().set_messages(std::move(messages));
  agent_.state().set_session_id(session_id);
  if (session_name)
    agent_.state().set_session_name(std::move(*session_name));
  else
    agent_.state().clear_session_name();
  active_session_id_ = std::move(session_id);
}

} // namespace pi::core
