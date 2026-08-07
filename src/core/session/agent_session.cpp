#include "core/session/agent_session.h"

#include "core/agent.h"
#include "core/event_types.h"
#include "core/message_types.h"
#include "core/models.h"
#include "core/sandbox.h"
#include "core/session/session_id.h"
#include "core/session/session_record.h"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace pi::core {

namespace {

void apply_sandbox_mode(const std::optional<std::string> &value,
                        const SandboxPolicyPtr &policy) {
  if (!value)
    return;
  const auto mode = sandbox_mode_from_string(*value);
  if (!mode)
    throw std::runtime_error("invalid session sandbox mode: " + *value);
  policy->set_mode(*mode);
}

Agent::Options
with_registry(Agent::Options options,
              const std::shared_ptr<const ModelRegistry> &registry) {
  if (registry)
    options.model_registry = registry;
  return options;
}

} // namespace

AgentSession::AgentSession(Config config)
    : agent_(with_registry(config.agent_options, config.model_registry)),
      session_store_(std::move(config.session_store)),
      sandbox_policy_(std::move(config.sandbox_policy)),
      model_registry_(config.model_registry
                          ? std::move(config.model_registry)
                          : config.agent_options.model_registry) {
  if (!sandbox_policy_)
    sandbox_policy_ = std::make_shared<SandboxPolicy>();
  agent_.set_tools(std::move(config.tools));
}

std::optional<SessionRecord>
AgentSession::load_session(const std::string &session_id) const {
  if (!session_store_)
    return std::nullopt;
  return session_store_->load(session_id);
}

void AgentSession::activate_session(const SessionRecord &record) {
  apply_sandbox_mode(record.header.sandbox_mode, sandbox_policy_);
  last_warning_.reset();
  std::string restored_provider = record.header.provider;
  std::string restored_model = record.header.model;
  if (restored_provider.empty() || restored_model.empty()) {
    for (auto it = record.messages.rbegin(); it != record.messages.rend();
         ++it) {
      const auto *assistant = std::get_if<AssistantMessage>(&*it);
      if (assistant == nullptr || assistant->provider.empty() ||
          assistant->model.empty())
        continue;
      restored_provider = assistant->provider;
      restored_model = assistant->model;
      break;
    }
  }
  auto model = agent_.state().model();
  auto thinking = agent_.state().thinking_level();
  if (model_registry_ && !restored_provider.empty() &&
      !restored_model.empty()) {
    ModelSelection selection{.provider = restored_provider,
                             .model = restored_model,
                             .source = "session"};
    auto resolution = model_registry_->resolve(selection);
    if (resolution) {
      model = resolution.model.value();
    } else {
      last_warning_ = resolution.error;
    }
  }
  const auto result =
      agent_.restore_session(std::move(model), thinking, record.messages,
                             record.header.id, record.header.name);
  if (result.warning) {
    if (last_warning_)
      *last_warning_ += "; " + *result.warning;
    else
      last_warning_ = result.warning;
  }
  active_session_id_ = record.header.id;
}

SandboxMode AgentSession::sandbox_mode() const {
  return sandbox_policy_->mode();
}

ModelResolution
AgentSession::resolve_model(const ModelSelection &selection) const {
  if (!model_registry_)
    return {.error = "model registry is unavailable"};
  return model_registry_->resolve(selection);
}

ModelSwitchResult AgentSession::set_model(Model model, ThinkingLevel thinking) {
  last_warning_.reset();
  std::function<void()> persist;
  if (session_store_ && active_session_id_) {
    const auto session_id = *active_session_id_;
    const auto provider = model.provider;
    const auto model_id = model.id;
    persist = [store = session_store_, session_id, provider, model_id] {
      store->set_model(session_id, provider, model_id);
    };
  }
  auto result = agent_.set_model(std::move(model), thinking, persist);
  last_warning_ = result.warning;
  return result;
}

void AgentSession::set_sandbox_mode(SandboxMode mode) {
  sandbox_policy_->set_mode(mode);
  if (session_store_ && active_session_id_)
    session_store_->set_sandbox_mode(*active_session_id_,
                                     std::string(sandbox_mode_to_string(mode)));
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
  if (header.model.empty() || header.provider.empty()) {
    const auto current = agent_.state().model();
    if (header.model.empty())
      header.model = current.id;
    if (header.provider.empty())
      header.provider = current.provider;
  }
  if (header.id.empty())
    header.id = generate_session_id();
  if (!header.sandbox_mode)
    header.sandbox_mode = std::string(sandbox_mode_to_string(sandbox_mode()));
  apply_sandbox_mode(header.sandbox_mode, sandbox_policy_);

  auto session_id = header.id;
  auto session_name = header.name;
  if (session_store_)
    session_id = session_store_->create(header);

  const auto active_id = session_id;
  activate_session_state(std::move(session_id), {}, std::move(session_name));
  return active_id;
}

std::string AgentSession::fork_session(SessionHeader header) {
  if (header.model.empty() || header.provider.empty()) {
    const auto current = agent_.state().model();
    if (header.model.empty())
      header.model = current.id;
    if (header.provider.empty())
      header.provider = current.provider;
  }
  if (header.id.empty())
    header.id = generate_session_id();
  if (!header.sandbox_mode)
    header.sandbox_mode = std::string(sandbox_mode_to_string(sandbox_mode()));
  apply_sandbox_mode(header.sandbox_mode, sandbox_policy_);

  auto session_id = header.id;
  if (session_store_)
    session_id = session_store_->create(header);

  agent_.set_session_identity(session_id, header.name);
  active_session_id_ = session_id;
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
  const auto model = agent_.state().model();
  const auto thinking = agent_.state().thinking_level();
  (void)agent_.restore_session(model, thinking, std::move(messages), session_id,
                               std::move(session_name));
  active_session_id_ = std::move(session_id);
}

} // namespace pi::core
