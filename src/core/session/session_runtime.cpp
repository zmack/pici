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
#include <chrono>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
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
      on_session_forked_(std::move(config.on_session_forked)),
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

void SessionRuntime::activate(Agent::Options child_options,
                              TaskTree::Limits limits,
                              TaskTree::ChildWriteTools child_write_tools,
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

  auto tree = std::make_shared<TaskTree>(
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

bool SessionRuntime::provider_auth_missing(const std::string &provider) const {
  return auth_availability_ &&
         auth_availability_(provider) == pi::auth::AuthAvailability::missing;
}

ModelSwitchResult SessionRuntime::set_model(Model model,
                                            ThinkingLevel thinking) {
  last_warning_.reset();
  if (provider_auth_missing(model.provider)) {
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
  if (!active_session_id_ || !session_store_)
    return false;

  // Both the branch target and the vector installed in memory below must
  // come from the SAME fresh walk of the durable session, not from
  // separately resizing agent_.state()'s own (possibly not-yet-fully-
  // persisted) vector -- those two can diverge, e.g. on a prior persistence
  // error. load()'s entry_ids is that one source of truth.
  auto record = load_session(*active_session_id_);
  if (!record)
    return false;

  const auto &entry_ids = record->entry_ids;
  through = std::min(through, entry_ids.size());

  // A `through` that lands strictly inside an atomic (multi-message,
  // single-entry-id) contribution -- e.g. mid-compaction -- can't be
  // expressed as a branch target: that would mean keeping only part of an
  // entry that's indivisible on disk.
  if (through > 0 && through < entry_ids.size() &&
      entry_ids[through] == entry_ids[through - 1]) {
    throw std::runtime_error(
        "cannot truncate to " + std::to_string(through) +
        ": that point falls inside an atomic session entry");
  }

  std::optional<std::string> target_entry_id =
      through > 0 ? std::optional<std::string>(entry_ids[through - 1])
                  : std::nullopt;

  // branch() persists the rewind non-destructively (the abandoned messages
  // stay on disk, replayable) and hands back the exact flat vector it just
  // wrote, which is installed as-is below -- the single source of truth for
  // both the disk record and the in-memory state, so the two can never
  // disagree.
  auto messages = session_store_->branch(*active_session_id_, target_entry_id);
  agent_.state().set_messages(std::move(messages));
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

  // The ONLY call site that ever applies a pending Lua-requested session
  // action (plans/velvety-skipping-marble.md's central constraint). This
  // runs on whatever thread called run_messages -- never on a thread that
  // could be holding a LuaHooksImpl::mutex_. Do not add another drain call
  // site; see try_drain_pending_action()'s declaration comment.
  try_drain_pending_action(callback);

  return result;
}

std::string_view SessionRuntime::pending_action_kind_name(
    const PendingSessionAction &action) {
  return std::visit(
      [](const auto &value) -> std::string_view {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, PendingCompaction>)
          return "compaction";
        else if constexpr (std::is_same_v<T, PendingModelSwitch>)
          return "model_switch";
        else
          return "fork";
      },
      action);
}

SessionRuntime::DeferredActionResult
SessionRuntime::request_deferred_compaction() {
  std::scoped_lock lock(pending_action_mutex_);
  if (pending_action_) {
    return {.accepted = false,
            .error = "a " +
                     std::string(pending_action_kind_name(*pending_action_)) +
                     " is already pending"};
  }
  if (applying_action_) {
    return {.accepted = false,
            .error = "a " + std::string(applying_action_kind_) +
                     " is already applying"};
  }
  pending_action_ = PendingCompaction{};
  return {.accepted = true, .error = {}};
}

SessionRuntime::DeferredActionResult
SessionRuntime::request_deferred_model_switch(const ModelSelection &selection,
                                              ThinkingLevel thinking) {
  // Resolve + auth-check happen BEFORE the pending slot is ever touched, so
  // an invalid request never occupies it (plans/velvety-skipping-marble.md
  // Phase 2) -- no cleanup needed at drain time for a request that should
  // never have been accepted in the first place.
  const auto resolution = resolve_model(selection);
  if (!resolution)
    return {.accepted = false, .error = resolution.error};
  // resolution's operator bool() is defined as model.has_value(), so the
  // check above already guarantees model is set here.
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  const auto &resolved_model = resolution.model.value();
  if (provider_auth_missing(resolved_model.provider)) {
    return {.accepted = false,
            .error = "missing authentication for provider '" +
                     resolved_model.provider + "'"};
  }

  std::scoped_lock lock(pending_action_mutex_);
  if (pending_action_) {
    return {.accepted = false,
            .error = "a " +
                     std::string(pending_action_kind_name(*pending_action_)) +
                     " is already pending"};
  }
  if (applying_action_) {
    return {.accepted = false,
            .error = "a " + std::string(applying_action_kind_) +
                     " is already applying"};
  }
  pending_action_ = PendingModelSwitch{resolved_model, thinking};
  return {.accepted = true, .error = {}};
}

SessionRuntime::DeferredActionResult
SessionRuntime::request_deferred_fork(std::optional<std::string> name) {
  // No synchronous precheck here, unlike request_deferred_model_switch():
  // a fork has no equivalent "resolve first" step, so the single-slot
  // pending/applying rejection below is the entire check.
  std::scoped_lock lock(pending_action_mutex_);
  if (pending_action_) {
    return {.accepted = false,
            .error = "a " +
                     std::string(pending_action_kind_name(*pending_action_)) +
                     " is already pending"};
  }
  if (applying_action_) {
    return {.accepted = false,
            .error = "a " + std::string(applying_action_kind_) +
                     " is already applying"};
  }
  pending_action_ = PendingFork{std::move(name)};
  return {.accepted = true, .error = {}};
}

nlohmann::json SessionRuntime::action_status() const {
  std::scoped_lock lock(pending_action_mutex_);
  nlohmann::json status;
  status["pending"] =
      pending_action_
          ? nlohmann::json(std::string(pending_action_kind_name(*pending_action_)))
          : nlohmann::json(nullptr);
  status["applying"] = applying_action_;
  status["last_error"] = last_deferred_action_error_
                             ? nlohmann::json(*last_deferred_action_error_)
                             : nlohmann::json(nullptr);
  // Phase 3: persists until the next successful fork overwrites it (see
  // last_fork_result_'s declaration comment) -- not reset to null here just
  // because it was read.
  status["last_fork_session_id"] = last_fork_result_
                                       ? nlohmann::json(*last_fork_result_)
                                       : nlohmann::json(nullptr);
  return status;
}

void SessionRuntime::try_drain_pending_action(const EventCallback &callback) {
  std::optional<PendingSessionAction> action;
  {
    std::scoped_lock lock(pending_action_mutex_);
    if (applying_action_ || !pending_action_)
      return;
    action = std::exchange(pending_action_, std::nullopt);
    applying_action_ = true;
    applying_action_kind_ = pending_action_kind_name(*action);
  }

  try {
    std::visit(
        [&](auto &&value) {
          using T = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<T, PendingCompaction>) {
            auto result =
                compact_active_session(CompactionTrigger::lua_requested, callback);
            if (result.error) {
              std::scoped_lock lock(pending_action_mutex_);
              last_deferred_action_error_ = result.error;
            }
          } else if constexpr (std::is_same_v<T, PendingModelSwitch>) {
            // May legitimately throw here even though the request was
            // accepted earlier: with_idle_transition's fuller idle
            // definition (steering/follow-up queues empty, not just
            // is_streaming() false) is only checked at drain time, not at
            // request time. The outer try/catch below is what turns that
            // into a reported, not silently dropped, failure.
            auto result = set_model(value.model, value.thinking);
            if (result.error) {
              std::scoped_lock lock(pending_action_mutex_);
              last_deferred_action_error_ = result.error;
            }
          } else if constexpr (std::is_same_v<T, PendingFork>) {
            // Parent linkage is built here, at drain time, from whatever
            // session/message state is current now -- not at request time
            // -- mirroring exactly what handle_fork_command()
            // (cmd_run_session.h) and the RPC "fork" handler (rpc_mode.cpp)
            // already build on their own header before calling
            // fork_session() directly. fork_session() itself only ever
            // defaults id/model/provider/sandbox_mode (see its own body);
            // it does not populate parent_id/parent_offset/
            // parent_entry_anchor, so those still have to be set here.
            if (!active_session_id_)
              throw std::runtime_error("no active session to fork");
            const auto parent_id = *active_session_id_;

            SessionHeader header;
            header.name = value.name;
            header.created = std::chrono::system_clock::to_time_t(
                std::chrono::system_clock::now());
            header.parent_id = parent_id;
            header.parent_offset = agent_.state().messages().size();
            if (session_store_) {
              const auto anchor =
                  session_store_->current_leaf_anchor(parent_id);
              header.parent_entry_anchor = anchor.anchor;
              header.parent_entry_id = anchor.entry_id;
            }

            // Fork-ordering tradeoff (plans/velvety-skipping-marble.md
            // Phase 3, accepted rather than a deeper fork_session()
            // redesign): fork_session() writes the new session's header to
            // disk via session_store_->create() BEFORE calling
            // agent_.set_session_identity(), which can itself throw via
            // with_idle_transition if the fuller idle condition (steering/
            // follow-up queues empty, not just is_streaming() false) isn't
            // satisfied -- newly reachable here because, unlike /fork and
            // the RPC fork handler (which only ever call fork_session()
            // once they already know the agent is fully idle), a deferred
            // fork has no such guarantee at drain time. If that throws,
            // the newly-created session file is left on disk as a
            // harmless orphan: a valid, unparented-from-anything-live
            // session record with no messages -- clutter, not corruption.
            // The outer try/catch around this whole std::visit is what
            // turns that throw into a reported last_deferred_action_error_
            // instead of letting it escape run_messages() uncaught;
            // building cleanup machinery for the orphan file is
            // deliberately out of scope (see the plan).
            const auto new_id = fork_session(header);

            {
              std::scoped_lock lock(pending_action_mutex_);
              last_fork_result_ = new_id;
            }
            if (on_session_forked_)
              on_session_forked_(new_id, parent_id);
          }
        },
        *action);
  } catch (const std::exception &e) {
    std::scoped_lock lock(pending_action_mutex_);
    last_deferred_action_error_ = e.what();
  } catch (...) {
    std::scoped_lock lock(pending_action_mutex_);
    last_deferred_action_error_ = "Unknown deferred session action error";
  }

  std::scoped_lock lock(pending_action_mutex_);
  applying_action_ = false;
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
