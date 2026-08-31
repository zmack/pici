#include "core/session/session_runtime.h"

#include "core/agent.h"
#include "core/agent_loop.h"
#include "core/agent_task.h"
#include "core/auth/authentication_adapter.h"
#include "core/compaction.h"
#include "core/event_types.h"
#include "core/mailbox/mailbox_coordinator.h"
#include "core/message_types.h"
#include "core/models.h"
#include "core/sandbox.h"
#include "core/session/session_id.h"
#include "core/session/session_record.h"
#include "core/stream.h"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
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
              const std::shared_ptr<const ModelCatalog> &registry) {
  if (registry)
    options.model_catalog = registry;
  return options;
}

} // namespace

SessionRuntime::SessionRuntime(Config config)
    : agent_(with_registry(config.agent_options, config.model_catalog)),
      session_store_(std::move(config.session_store)),
      sandbox_policy_(std::move(config.sandbox_policy)),
      model_catalog_(config.model_catalog ? std::move(config.model_catalog)
                                          : config.agent_options.model_catalog),
      auth_availability_(std::move(config.auth_availability)),
      auto_compaction_(config.auto_compaction),
      mailbox_runtime_(std::move(config.mailbox)) {
  if (!sandbox_policy_)
    sandbox_policy_ = std::make_shared<SandboxPolicy>();
  agent_.set_tools(std::move(config.tools));
}

SessionRuntime::~SessionRuntime() {
  // Explicit, not implicit member order: agent_ is declared before
  // mailbox_runtime_ (so it would ordinarily destruct AFTER it), but its
  // owned TaskTree must close and join every child before mailbox_runtime_
  // tears down -- see the declaration-order comment on mailbox_runtime_.
  // TaskTree::shutdown() is idempotent, so agent_'s own destruction later
  // redundantly (and safely) shutting it down again is harmless.
  if (const auto &tree = agent_.task_tree())
    tree->shutdown();
}

void SessionRuntime::activate(
    Agent::Options child_options, AgentTaskManager::Limits limits,
    AgentTaskManager::ChildWriteTools child_write_tools,
    AgentTaskEventCallback extra_task_event_callback,
    std::function<void()> wake_root) {
  if (agent_.task_tree())
    throw std::logic_error("SessionRuntime::activate called more than once");

  std::vector<AgentTaskEventCallback> task_callbacks;
  if (extra_task_event_callback)
    task_callbacks.emplace_back(std::move(extra_task_event_callback));
  if (auto callback = mailbox_runtime_.task_event_callback())
    task_callbacks.emplace_back(std::move(callback));

  // Only this class knows how to complete a child's SessionRuntime::Config
  // (TaskTree never assembles one itself). Mirrors make_task()'s pre-Phase-7
  // Config exactly: children get agent_options/tools only, never this
  // session's model_catalog/sandbox_policy/mailbox/auto_compaction.
  ChildSessionFactory child_factory = [](ChildSessionSpec spec) {
    return std::make_unique<SessionRuntime>(SessionRuntime::Config{
        .agent_options = std::move(spec.agent_options),
        .tools = std::move(spec.tools),
        .session_store = std::move(spec.session_store),
    });
  };

  auto tree = std::make_shared<AgentTaskManager>(
      agent_, std::move(child_factory), std::move(child_options), limits,
      fan_out_agent_task_callbacks(std::move(task_callbacks)),
      child_write_tools);
  agent_.set_task_tree(tree);

  mailbox_runtime_.connect(*this, tree, std::move(wake_root));
}

std::optional<SessionRecord>
SessionRuntime::load_session(const std::string &session_id) const {
  if (!session_store_)
    return std::nullopt;
  return session_store_->load(session_id);
}

void SessionRuntime::activate_session(const SessionRecord &record) {
  apply_sandbox_mode(record.header.sandbox_mode, sandbox_policy_);
  last_warning_.reset();
  std::string restored_provider = record.header.provider;
  std::string restored_model = record.header.model;
  if (restored_provider.empty() || restored_model.empty()) {
    for (const auto &message : std::ranges::reverse_view(record.messages)) {
      const auto *assistant = std::get_if<AssistantMessage>(&message);
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
  if (model_catalog_ && !restored_provider.empty() && !restored_model.empty()) {
    ModelSelection selection{.provider = restored_provider,
                             .model = restored_model,
                             .source = "session"};
    auto resolution = model_catalog_->resolve(selection);
    if (resolution) {
      // resolution's operator bool() is defined as model.has_value(), so the
      // check above already guarantees model is set here.
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
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

SandboxMode SessionRuntime::sandbox_mode() const {
  return sandbox_policy_->mode();
}

ModelResolution
SessionRuntime::resolve_model(const ModelSelection &selection) const {
  if (!model_catalog_)
    return {.error = "model registry is unavailable"};
  return model_catalog_->resolve(selection);
}

ModelSwitchResult SessionRuntime::set_model(Model model,
                                            ThinkingLevel thinking) {
  last_warning_.reset();
  if (auth_availability_ && auth_availability_(model.provider) ==
                                pi::auth::AuthAvailability::missing) {
    const auto current = agent_.state().model();
    return ModelSwitchResult{.previous = current,
                             .current = current,
                             .thinking_level = agent_.state().thinking_level(),
                             .error = "missing authentication for provider '" +
                                      model.provider + "'"};
  }
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

void SessionRuntime::set_sandbox_mode(SandboxMode mode) {
  sandbox_policy_->set_mode(mode);
  if (session_store_ && active_session_id_)
    session_store_->set_sandbox_mode(*active_session_id_,
                                     std::string(sandbox_mode_to_string(mode)));
}

bool SessionRuntime::activate_session(const std::string &session_id) {
  auto record = load_session(session_id);
  if (!record)
    return false;
  activate_session(*record);
  return true;
}

std::string SessionRuntime::open_session(std::string session_id,
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

std::string SessionRuntime::create_session(SessionHeader header) {
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

std::string SessionRuntime::fork_session(SessionHeader header) {
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

bool SessionRuntime::truncate_active_session(std::size_t through) {
  if (!active_session_id_)
    return false;

  auto messages = agent_.state().messages();
  messages.resize(std::min(through, messages.size()));
  agent_.state().set_messages(messages);
  if (session_store_)
    session_store_->append_truncate(*active_session_id_, through);
  return true;
}

SessionRuntime::RunResult
SessionRuntime::run_prompt(std::string prompt, const EventCallback &callback) {
  UserMessage message;
  message.content.emplace_back(TextContent{.text = std::move(prompt)});
  return run_messages({AgentInput{.message = Message{std::move(message)}}},
                      callback);
}

SessionRuntime::RunResult SessionRuntime::drain_agent_stream(
    EventStream<AgentEvent, std::vector<Message>> stream,
    const EventCallback &callback) {
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

SessionRuntime::RunResult
SessionRuntime::maybe_retry_after_context_window_error(
    RunResult result, const EventCallback &callback, bool &retried) {
  retried = false;
  if (!result.error || !auto_compaction_.enabled)
    return result;
  if (!looks_like_context_window_error(*result.error))
    return result;

  const auto model = agent_.state().model();
  if (!supports_remote_compaction(model))
    return result; // no local fallback exists yet; surface the original error

  // Unlike the post-turn threshold trigger below, this path does not need
  // our own budget estimate to agree that the context is oversized — the
  // provider has already said so via the error we just received. Only
  // "nothing to compact" (no prior successful assistant/tool turn) makes
  // this degenerate.
  const auto context = agent_.context_snapshot();
  if (!has_compactable_history(context)) {
    result.error = "context window exceeded and there is no prior "
                   "assistant/tool history to compact away (original "
                   "error: " +
                   *result.error + ")";
    return result;
  }

  retried = true;
  auto compaction =
      compact_active_session(CompactionTrigger::context_window_retry, callback);
  if (!compaction.success)
    return result; // keep the original context-window error

  // Retry exactly once: continue from the compacted context without
  // recursing back into this function, per plan §7's "avoid repeated
  // compaction loops" / "at most one compaction retry."
  return drain_agent_stream(agent_.continue_(), callback);
}

void SessionRuntime::maybe_auto_compact_after_turn(
    const EventCallback &callback) {
  if (!auto_compaction_.enabled)
    return;
  const auto model = agent_.state().model();
  if (!supports_remote_compaction(model))
    return;

  const ContextBudgetPolicy policy{.threshold_pct =
                                       auto_compaction_.threshold_pct};
  const auto context = agent_.context_snapshot();
  // Nothing compaction can do about a still-oversized single first turn;
  // do not spend a network round-trip finding that out again here (the
  // context-window-retry path already reports this distinctly if the
  // provider itself rejects the request).
  if (is_degenerate_oversized_context(context, policy))
    return;
  if (!exceeds_context_budget(context, policy))
    return;

  // Fire-and-forget from the caller's perspective: the turn that just
  // completed already succeeded, so a failed automatic compaction here
  // does not fail it. The next completed turn (or a context-window error)
  // gets another chance to trigger.
  compact_active_session(CompactionTrigger::automatic_pre_turn, callback);
}

SessionRuntime::RunResult
SessionRuntime::run_messages(std::vector<AgentInput> messages,
                             const EventCallback &callback) {
  auto result =
      drain_agent_stream(agent_.prompt(std::move(messages)), callback);

  bool retried = false;
  result = maybe_retry_after_context_window_error(std::move(result), callback,
                                                  retried);

  if (!result.error && !retried)
    maybe_auto_compact_after_turn(callback);

  return result;
}

SessionRuntime::CompactionRunResult
SessionRuntime::compact_active_session(CompactionTrigger trigger,
                                       const EventCallback &callback) {
  CompactionRunResult result;
  if (!active_session_id_) {
    result.error = "no active session";
    return result;
  }
  const auto session_id = *active_session_id_;

  auto handle_event = [&](const AgentEvent &event) {
    if (const auto *compaction = std::get_if<CompactionEvent>(&event)) {
      if (compaction->kind == CompactionEventKind::complete) {
        result.retained_message_count = compaction->retained_message_count;

        SessionCompactionRecord record;
        record.messages = compaction->replacement_messages;
        record.provider = compaction->provider;
        record.model = compaction->model;
        record.summary = compaction->summary;
        record.timestamp = compaction->timestamp;

        std::function<void()> persist;
        if (session_store_) {
          persist = [store = session_store_, session_id, record] {
            store->append_compaction(session_id, record);
          };
        }

        try {
          auto commit = agent_.commit_compaction(
              compaction->snapshot_epoch, compaction->replacement_messages,
              persist);
          if (commit.installed)
            result.success = true;
          else
            result.error = commit.error.value_or("compaction install failed");
        } catch (const std::exception &e) {
          result.error = e.what();
        } catch (...) {
          result.error = "Unknown compaction commit error";
        }
      } else if (compaction->kind == CompactionEventKind::error) {
        result.cancelled = compaction->cancelled;
        result.unsupported = compaction->unsupported;
        result.error = compaction->error_message;
      }
    }
    if (callback)
      callback(event);
  };

  try {
    auto stream = agent_.compact(trigger);
    for (const auto &event : stream)
      handle_event(event);
  } catch (const std::exception &e) {
    result.error = e.what();
  } catch (...) {
    result.error = "Unknown compaction error";
  }

  return result;
}

void SessionRuntime::activate_session_state(
    std::string session_id, std::vector<Message> messages,
    std::optional<std::string> session_name) {
  const auto model = agent_.state().model();
  const auto thinking = agent_.state().thinking_level();
  (void)agent_.restore_session(model, thinking, std::move(messages), session_id,
                               std::move(session_name));
  active_session_id_ = std::move(session_id);
}

} // namespace pi::core
