#include "core/mailbox/mailbox_coordinator.h"
#include "core/agent_loop.h"
#include "core/agent_runtime_identity.h"
#include "core/agent_task.h"
#include "core/input_provenance.h"
#include "core/mailbox/mailbox_store.h"
#include "core/mailbox/mailbox_types.h"
#include "core/message_types.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace pi::core {

namespace {

Message mailbox_message_to_message(const MailboxEntry &message) {
  UserMessage user;
  std::string text = "[pici mailbox message]\n";
  text += "message_id=" + message.entry_id + "\n";
  text +=
      "kind=" + std::string(mailbox_entry_kind_to_string(message.kind)) + "\n";
  text += "sender_session_id=" + message.sender_session_id + "\n";
  text += "sender_agent_id=" + message.sender_agent_id + "\n";
  text += "recipient_session_id=" + message.recipient_session_id + "\n";
  text += "recipient_agent_id=" +
          message.recipient_agent_id.value_or("(session root)") + "\n";
  if (message.kind == MailboxEntryKind::request) {
    text += "Reply with agents_reply(message_id=\"" + message.entry_id +
            "\") if a response is appropriate.\n";
  }
  text += "\n" + message.body.text;
  user.content.emplace_back(TextContent{.text = std::move(text)});
  return Message{std::move(user)};
}

InputProvenance mailbox_input_provenance(const MailboxEntry &message) {
  return {.source = InputProvenance::Source::mailbox,
          .message_id = message.entry_id,
          .message_kind =
              std::string(mailbox_entry_kind_to_string(message.kind)),
          .sender_agent_id = message.sender_agent_id,
          .sender_session_id = message.sender_session_id};
}

TimestampMs retention_milliseconds(std::int64_t days) {
  constexpr auto day_ms = static_cast<std::int64_t>(24) * 60 * 60 * 1000;
  if (days <= 0)
    return 0;
  if (days > std::numeric_limits<TimestampMs>::max() / day_ms)
    return std::numeric_limits<TimestampMs>::max();
  return days * day_ms;
}

} // namespace

AgentTaskEventCallback
fan_out_agent_task_callbacks(std::vector<AgentTaskEventCallback> callbacks) {
  return [callbacks = std::move(callbacks)](const AgentTaskEvent &event) {
    for (const auto &callback : callbacks) {
      if (!callback)
        continue;
      try {
        callback(event);
      } catch (...) {
        // An observer is isolated from every other observer and the manager.
        static_cast<void>(0);
      }
    }
  };
}

Mailbox::Mailbox(MailboxOptions options)
    : options_(std::move(options)), lifetime_(std::make_shared<Lifetime>()) {
  if (options_.process_id.empty())
    throw MailboxError(MailboxErrorCode::invalid_message,
                       "mailbox coordinator identities are required");
  if (options_.heartbeat_interval <= std::chrono::milliseconds::zero())
    throw MailboxError(MailboxErrorCode::invalid_message,
                       "mailbox heartbeat interval must be positive");
  if (options_.stale_after <= options_.heartbeat_interval)
    throw MailboxError(MailboxErrorCode::invalid_message,
                       "mailbox stale_after must exceed heartbeat interval");
  if (options_.cleanup_interval <= std::chrono::milliseconds::zero())
    options_.cleanup_interval = options_.heartbeat_interval;
  if (options_.poll_interval <= std::chrono::milliseconds::zero())
    options_.poll_interval = std::chrono::milliseconds(250);

  lifetime_->owner = this;

  store_ = std::make_unique<MailboxStore>(options_.store);
  const auto now = options_.store.clock();
  last_cleanup_ms_ = now;
  store_->register_process(ProcessRecord{
      .process_id = options_.process_id,
      .workspace_id = options_.store.workspace_id,
      .workspace_path = options_.store.workspace_path,
      .pid = options_.pid == 0 ? ::getpid() : options_.pid,
      .hostname = options_.hostname,
      .protocol_version = options_.protocol_version,
      .capabilities_json = options_.capabilities_json,
      .started_at_ms = now,
      .last_seen_at_ms = now,
      .lease_expires_at_ms = now + options_.stale_after.count(),
  });
  maintenance_ = std::jthread([this](const std::stop_token &stop_token) {
    maintenance_loop(stop_token);
  });
  // Always create the default attachment (not only when initial_session_id
  // is set): every unscoped (no explicit key) method below operates on it,
  // and pre-Phase-8 code activates its root explicitly, after construction,
  // rather than relying on initial_session_id -- see MailboxOptions. Without
  // this, that pre-existing call pattern would fail with "attachment is not
  // registered" the first time it touched an unscoped method.
  default_attachment_ = attach();
  {
    std::scoped_lock lock(mutex_);
    attachments_.at(default_attachment_).active_root_agent_id =
        options_.root_agent_id;
  }
  if (!options_.initial_session_id.empty())
    activate_root(default_attachment_, options_.initial_session_id,
                  options_.initial_session_name);
}

Mailbox::~Mailbox() noexcept { stop(); }

MailboxAttachmentKey Mailbox::new_attachment_key() const {
  static std::atomic_uint64_t counter{1};
  std::string key = options_.store.id_generator ? options_.store.id_generator()
                                                : std::string("attachment");
  key += ":attachment:" + std::to_string(counter.fetch_add(1));
  return MailboxAttachmentKey(std::move(key));
}

MailboxAttachmentKey Mailbox::attach() {
  std::scoped_lock lock(mutex_);
  auto key = new_attachment_key();
  // Seeded from the process-wide configured provider/model (matching this
  // class's pre-Phase-8 constructor-time seeding) so a fresh attachment can
  // activate_root() immediately, before ever calling set_model() itself.
  Attachment attachment;
  attachment.provider = options_.provider;
  attachment.model_id = options_.model_id;
  attachments_.emplace(key, std::move(attachment));
  return key;
}

void Mailbox::detach(const MailboxAttachmentKey &key) noexcept {
  try {
    deactivate_root(key);
  } catch (...) {
    static_cast<void>(0);
  }
  std::scoped_lock lock(mutex_);
  attachments_.erase(key);
}

Mailbox::Attachment *
Mailbox::find_attachment_for_session_locked(std::string_view session_id) {
  for (auto &[key, attachment] : attachments_) {
    if (attachment.root_active && attachment.session_id &&
        *attachment.session_id == session_id)
      return &attachment;
  }
  return nullptr;
}

const Mailbox::Attachment *
Mailbox::find_attachment_for_session_locked(std::string_view session_id) const {
  for (const auto &[key, attachment] : attachments_) {
    if (attachment.root_active && attachment.session_id &&
        *attachment.session_id == session_id)
      return &attachment;
  }
  return nullptr;
}

AgentRuntimeIdentity
Mailbox::activate_root(const MailboxAttachmentKey &key, std::string session_id,
                       std::optional<std::string> session_name) {
  if (session_id.empty())
    throw MailboxError(MailboxErrorCode::invalid_message,
                       "mailbox session identity is required");
  std::optional<std::string> old_session;
  std::string old_root_agent_id;
  std::unordered_set<std::string> old_subagent_ids;
  std::string root_agent_id;
  std::string provider;
  std::string model;
  std::shared_ptr<MailboxDeliveryTargets> delivery;
  {
    std::scoped_lock lock(mutex_);
    if (stopped_)
      throw MailboxError(MailboxErrorCode::internal,
                         "mailbox coordinator is stopped");
    const auto it = attachments_.find(key);
    if (it == attachments_.end())
      throw MailboxError(MailboxErrorCode::not_found,
                         "mailbox attachment is not registered");
    auto &attachment = it->second;
    old_session = attachment.session_id;
    old_root_agent_id = attachment.active_root_agent_id;
    old_subagent_ids = attachment.subagent_ids;
    provider = attachment.provider;
    model = attachment.model_id;
    delivery = attachment.delivery_targets;
    if (attachment.root_registered || attachment.active_root_agent_id.empty())
      root_agent_id = options_.store.id_generator();
    else
      root_agent_id = attachment.active_root_agent_id;
  }
  if (old_session && delivery && delivery->drop_queued) {
    try {
      delivery->drop_queued();
    } catch (...) {
      static_cast<void>(0);
    }
  }
  const auto now = options_.store.clock();
  store_->register_agent(AgentRecord{
      .agent_id = root_agent_id,
      .process_id = options_.process_id,
      .kind = "root",
      .session_id = session_id,
      .session_name = session_name,
      .provider = provider,
      .model_id = model,
      .status = "starting",
      .started_at_ms = now,
  });
  try {
    for (const auto &subagent_id : old_subagent_ids)
      store_->close_agent(subagent_id, now);
    if (old_session)
      store_->close_agent(old_root_agent_id, now);
  } catch (...) {
    try {
      store_->close_agent(root_agent_id, now);
    } catch (...) {
      static_cast<void>(0);
    }
    throw;
  }
  AgentRuntimeIdentity registered_identity;
  {
    std::scoped_lock lock(mutex_);
    const auto it = attachments_.find(key);
    if (it == attachments_.end())
      throw MailboxError(MailboxErrorCode::not_found,
                         "mailbox attachment is not registered");
    auto &attachment = it->second;
    attachment.active_root_agent_id = std::move(root_agent_id);
    attachment.session_id = std::move(session_id);
    attachment.session_name = std::move(session_name);
    attachment.root_active = true;
    attachment.root_running = false;
    attachment.root_registered = true;
    attachment.subagent_ids.clear();
    attachment.subagent_endpoint_by_task.clear();
    attachment.subagent_task_by_endpoint.clear();
    registered_identity =
        AgentRuntimeIdentity{.agent_id = attachment.active_root_agent_id,
                             .session_id = *attachment.session_id,
                             .kind = "root"};
  }
  return registered_identity;
}

void Mailbox::deactivate_root(const MailboxAttachmentKey &key) {
  std::optional<std::string> active;
  std::string root_agent_id;
  std::unordered_set<std::string> subagent_ids;
  std::shared_ptr<MailboxDeliveryTargets> delivery;
  {
    std::scoped_lock lock(mutex_);
    const auto it = attachments_.find(key);
    if (it == attachments_.end() || !it->second.root_active)
      return;
    active = it->second.session_id;
    root_agent_id = it->second.active_root_agent_id;
    subagent_ids = it->second.subagent_ids;
    delivery = it->second.delivery_targets;
  }
  if (delivery && delivery->drop_queued) {
    try {
      delivery->drop_queued();
    } catch (...) {
      static_cast<void>(0);
    }
  }
  const auto now = options_.store.clock();
  for (const auto &subagent_id : subagent_ids)
    store_->close_agent(subagent_id, now);
  if (active)
    store_->close_agent(root_agent_id, now);
  std::scoped_lock lock(mutex_);
  const auto it = attachments_.find(key);
  if (it == attachments_.end())
    return;
  auto &attachment = it->second;
  if (attachment.root_active &&
      attachment.active_root_agent_id == root_agent_id) {
    attachment.root_active = false;
    attachment.root_running = false;
    attachment.session_id.reset();
    attachment.session_name.reset();
    attachment.subagent_ids.clear();
    attachment.subagent_endpoint_by_task.clear();
    attachment.subagent_task_by_endpoint.clear();
  }
}

void Mailbox::set_session_name(const MailboxAttachmentKey &key,
                               std::string session_name) {
  std::string root_agent_id;
  {
    std::scoped_lock lock(mutex_);
    const auto it = attachments_.find(key);
    if (it == attachments_.end() || !it->second.root_active)
      return;
    root_agent_id = it->second.active_root_agent_id;
  }
  store_->update_agent(
      AgentUpdate{.agent_id = root_agent_id, .session_name = session_name});
  std::scoped_lock lock(mutex_);
  const auto it = attachments_.find(key);
  if (it != attachments_.end() && it->second.root_active &&
      it->second.active_root_agent_id == root_agent_id)
    it->second.session_name = std::move(session_name);
}

void Mailbox::set_root_running(const MailboxAttachmentKey &key, bool running) {
  std::string status;
  std::string root_agent_id;
  std::shared_ptr<MailboxDeliveryTargets> delivery;
  {
    std::scoped_lock lock(mutex_);
    const auto it = attachments_.find(key);
    if (it == attachments_.end() || !it->second.root_active ||
        it->second.root_running == running)
      return;
    status = running ? "running" : "idle";
    root_agent_id = it->second.active_root_agent_id;
    delivery = it->second.delivery_targets;
  }
  if (!running && delivery && delivery->drop_root_queued) {
    try {
      delivery->drop_root_queued();
    } catch (...) {
      static_cast<void>(0);
    }
  }
  store_->update_agent(
      AgentUpdate{.agent_id = root_agent_id, .status = std::move(status)});
  std::scoped_lock lock(mutex_);
  const auto it = attachments_.find(key);
  if (it != attachments_.end() && it->second.root_active &&
      it->second.active_root_agent_id == root_agent_id)
    it->second.root_running = running;
}

void Mailbox::set_model(const MailboxAttachmentKey &key, std::string provider,
                        std::string model_id) {
  std::optional<std::string> active;
  std::string root_agent_id;
  {
    std::scoped_lock lock(mutex_);
    const auto it = attachments_.find(key);
    if (it == attachments_.end())
      return;
    active = it->second.session_id;
    if (!active) {
      it->second.provider = std::move(provider);
      it->second.model_id = std::move(model_id);
      return;
    }
    root_agent_id = it->second.active_root_agent_id;
  }
  if (active)
    store_->update_agent(AgentUpdate{
        .agent_id = root_agent_id, .provider = provider, .model_id = model_id});
  std::scoped_lock lock(mutex_);
  const auto it = attachments_.find(key);
  if (it != attachments_.end() && it->second.root_active &&
      it->second.active_root_agent_id == root_agent_id) {
    it->second.provider = std::move(provider);
    it->second.model_id = std::move(model_id);
  }
}

AgentRuntimeIdentity
Mailbox::register_subagent(const MailboxAttachmentKey &key, std::string task_id,
                           std::string task_path,
                           std::optional<std::string> parent_id) {
  std::optional<std::string> session;
  std::string provider;
  std::string model;
  std::string root_agent_id;
  std::string owner_agent_id;
  {
    std::scoped_lock lock(mutex_);
    const auto it = attachments_.find(key);
    if (it == attachments_.end() || !it->second.root_active)
      throw MailboxError(MailboxErrorCode::not_found,
                         "mailbox root session is not active");
    auto &attachment = it->second;
    session = attachment.session_id;
    provider = attachment.provider;
    model = attachment.model_id;
    root_agent_id = attachment.active_root_agent_id;
    owner_agent_id = root_agent_id;
    if (parent_id && *parent_id != "root") {
      if (const auto endpoint_it =
              attachment.subagent_endpoint_by_task.find(*parent_id);
          endpoint_it != attachment.subagent_endpoint_by_task.end())
        owner_agent_id = endpoint_it->second;
      else
        throw MailboxError(MailboxErrorCode::not_found,
                           "mailbox parent task is not registered");
    }
  }
  if (!session)
    throw MailboxError(MailboxErrorCode::not_found,
                       "mailbox root session is not active");
  std::string endpoint;
  {
    if (options_.store.id_generator)
      endpoint = options_.store.id_generator();
    else
      endpoint = "endpoint";
    static std::atomic_uint64_t endpoint_counter{1};
    endpoint += ":subagent:" + std::to_string(endpoint_counter.fetch_add(1));
  }
  const auto now = options_.store.clock();
  store_->register_agent(AgentRecord{
      .agent_id = endpoint,
      .process_id = options_.process_id,
      .kind = "subagent",
      .owner_agent_id = owner_agent_id,
      .session_id = *session,
      .task_id = task_id,
      .task_path = task_path,
      .provider = provider,
      .model_id = model,
      .status = "pending",
      .started_at_ms = now,
  });
  bool belongs_to_current_session = false;
  {
    std::scoped_lock lock(mutex_);
    const auto it = attachments_.find(key);
    belongs_to_current_session =
        it != attachments_.end() && it->second.root_active &&
        it->second.session_id == session &&
        it->second.active_root_agent_id == root_agent_id;
    if (belongs_to_current_session) {
      it->second.subagent_ids.insert(endpoint);
      it->second.subagent_endpoint_by_task[task_id] = endpoint;
      it->second.subagent_task_by_endpoint[endpoint] = task_id;
    }
  }
  if (!belongs_to_current_session) {
    try {
      store_->close_agent(endpoint, now);
    } catch (...) {
      static_cast<void>(0);
    }
  }
  if (!belongs_to_current_session)
    throw MailboxError(MailboxErrorCode::not_found,
                       "mailbox root session changed during registration");
  return AgentRuntimeIdentity{.agent_id = endpoint,
                              .session_id = *session,
                              .kind = "subagent",
                              .task_id = std::move(task_id),
                              .task_path = std::move(task_path),
                              .owner_agent_id = std::move(owner_agent_id)};
}

void Mailbox::unregister_subagent(const MailboxAttachmentKey &key,
                                  std::string_view task_id) {
  std::string endpoint;
  {
    std::scoped_lock lock(mutex_);
    const auto it = attachments_.find(key);
    if (it == attachments_.end())
      return;
    const auto task_it =
        it->second.subagent_endpoint_by_task.find(std::string(task_id));
    if (task_it == it->second.subagent_endpoint_by_task.end())
      return;
    endpoint = task_it->second;
  }
  store_->close_agent(endpoint, options_.store.clock());
  std::scoped_lock lock(mutex_);
  const auto it = attachments_.find(key);
  if (it == attachments_.end())
    return;
  it->second.subagent_ids.erase(endpoint);
  it->second.subagent_endpoint_by_task.erase(std::string(task_id));
  it->second.subagent_task_by_endpoint.erase(endpoint);
}

std::string Mailbox::task_status(AgentTaskStatusKind status) {
  return std::string(agent_task_status_to_string(status));
}

void Mailbox::observe_task_event(const MailboxAttachmentKey &key,
                                 const AgentTaskEvent &event) {
  if (const auto *spawned = std::get_if<AgentTaskSpawnedEvent>(&event)) {
    // Registration is synchronous during spawn; this event is observational.
    static_cast<void>(spawned);
  } else if (const auto *changed =
                 std::get_if<AgentTaskStatusChangedEvent>(&event)) {
    std::string endpoint;
    bool owned = false;
    {
      std::scoped_lock lock(mutex_);
      const auto it = attachments_.find(key);
      if (it != attachments_.end() && it->second.root_active) {
        const auto endpoint_it =
            it->second.subagent_endpoint_by_task.find(changed->id);
        owned = endpoint_it != it->second.subagent_endpoint_by_task.end();
        if (owned)
          endpoint = endpoint_it->second;
      }
    }
    if (owned) {
      const auto status = task_status(changed->current);
      store_->update_agent(AgentUpdate{.agent_id = endpoint, .status = status});
    }
  } else if (const auto *closed = std::get_if<AgentTaskClosedEvent>(&event)) {
    std::string endpoint;
    bool owned = false;
    {
      std::scoped_lock lock(mutex_);
      const auto it = attachments_.find(key);
      if (it != attachments_.end() && it->second.root_active) {
        const auto endpoint_it =
            it->second.subagent_endpoint_by_task.find(closed->id);
        owned = endpoint_it != it->second.subagent_endpoint_by_task.end();
        if (owned)
          endpoint = endpoint_it->second;
      }
    }
    if (owned) {
      store_->close_agent(endpoint, options_.store.clock());
      std::scoped_lock lock(mutex_);
      const auto it = attachments_.find(key);
      if (it != attachments_.end()) {
        it->second.subagent_ids.erase(endpoint);
        it->second.subagent_endpoint_by_task.erase(closed->id);
        it->second.subagent_task_by_endpoint.erase(endpoint);
      }
    }
  }
}

void Mailbox::attach_delivery(const MailboxAttachmentKey &key,
                              std::shared_ptr<MailboxDeliveryTargets> targets) {
  std::scoped_lock lock(mutex_);
  const auto it = attachments_.find(key);
  if (it != attachments_.end())
    it->second.delivery_targets = std::move(targets);
}

void Mailbox::detach_delivery(const MailboxAttachmentKey &key) {
  std::scoped_lock lock(mutex_);
  const auto it = attachments_.find(key);
  if (it != attachments_.end())
    it->second.delivery_targets.reset();
}

void Mailbox::drop_queued_delivery(const MailboxAttachmentKey &key) {
  std::shared_ptr<MailboxDeliveryTargets> delivery;
  {
    std::scoped_lock lock(mutex_);
    const auto it = attachments_.find(key);
    if (it != attachments_.end())
      delivery = it->second.delivery_targets;
  }
  if (delivery && delivery->drop_queued)
    delivery->drop_queued();
}

bool Mailbox::idle_root_work_pending(const MailboxAttachmentKey &key) {
  std::string root_agent_id;
  std::string session_id;
  {
    std::scoped_lock lock(mutex_);
    const auto it = attachments_.find(key);
    if (it == attachments_.end())
      return false;
    const auto &attachment = it->second;
    if (!attachment.root_active || attachment.root_running || stopped_ ||
        !attachment.session_id)
      return false;
    root_agent_id = attachment.active_root_agent_id;
    session_id = *attachment.session_id;
  }
  try {
    const auto messages = store_->inspect(InboxQuery{
        .session_id = std::move(session_id),
        .workspace_id = options_.store.workspace_id,
        .agent_id = std::move(root_agent_id),
        .agent_kind = "root",
        .kinds = {MailboxEntryKind::steer, MailboxEntryKind::request},
        .claimable_only = true,
        .limit = 1,
        .now_ms = options_.store.clock()});
    return !messages.empty();
  } catch (...) {
    return false;
  }
}

std::vector<AgentInput>
Mailbox::claim_idle_root_turn(const MailboxAttachmentKey &key,
                              std::size_t limit) {
  if (limit == 0)
    return {};
  limit = std::min<std::size_t>(limit, 16);

  std::string root_agent_id;
  std::string session_id;
  {
    std::scoped_lock lock(mutex_);
    const auto it = attachments_.find(key);
    if (it == attachments_.end())
      return {};
    auto &attachment = it->second;
    if (!attachment.root_active || attachment.root_running || stopped_ ||
        !attachment.session_id)
      return {};
    root_agent_id = attachment.active_root_agent_id;
    session_id = *attachment.session_id;

    // Reserve the root turn before claiming.  This makes the claim and the
    // main-thread running transition one coordinator-serialized decision.
    store_->update_agent(
        AgentUpdate{.agent_id = root_agent_id, .status = "running"});
    attachment.root_running = true;
    try {
      const auto claimed = store_->claim(ClaimRequest{
          .session_id = session_id,
          .agent_id = root_agent_id,
          .workspace_id = options_.store.workspace_id,
          .kinds = {MailboxEntryKind::steer, MailboxEntryKind::request},
          .limit = limit,
          .now_ms = options_.store.clock(),
          .lease_ms = options_.store.claim_lease_ms});
      if (claimed.messages.empty()) {
        store_->update_agent(
            AgentUpdate{.agent_id = root_agent_id, .status = "idle"});
        attachment.root_running = false;
        return {};
      }

      std::vector<AgentInput> result;
      result.reserve(claimed.messages.size());
      for (const auto &claimed_message : claimed.messages) {
        const auto message_id = claimed_message.entry_id;
        const auto claim_token = claimed_message.claim_token.value_or("");
        const auto endpoint_ref =
            std::make_shared<const std::string>(root_agent_id);
        const auto message_id_ref =
            std::make_shared<const std::string>(message_id);
        const auto claim_token_ref =
            std::make_shared<const std::string>(claim_token);
        mark_delivered_best_effort(message_id);
        result.push_back(AgentInput{
            .message = mailbox_message_to_message(claimed_message),
            .on_accepted =
                [weak = std::weak_ptr<Lifetime>(lifetime_), endpoint_ref,
                 message_id_ref, claim_token_ref] noexcept {
                  auto state = weak.lock();
                  if (!state)
                    return;
                  Mailbox *owner = nullptr;
                  {
                    std::scoped_lock lock(state->mutex);
                    if (!state->active || state->owner == nullptr)
                      return;
                    owner = state->owner;
                    ++state->in_flight;
                  }
                  try {
                    owner->acknowledge_delivery(*endpoint_ref, *message_id_ref,
                                                *claim_token_ref);
                  } catch (...) {
                    static_cast<void>(0);
                  }
                  {
                    std::scoped_lock lock(state->mutex);
                    if (state->in_flight > 0)
                      --state->in_flight;
                    if (state->in_flight == 0)
                      state->condition.notify_all();
                  }
                },
            .presentation = mailbox_input_provenance(claimed_message)});
      }
      return result;
    } catch (...) {
      try {
        store_->update_agent(
            AgentUpdate{.agent_id = root_agent_id, .status = "idle"});
      } catch (...) {
        static_cast<void>(0);
      }
      it->second.root_running = false;
      throw;
    }
  }
}

void Mailbox::mark_delivered_best_effort(const std::string &entry_id) {
  try {
    store_->mark_delivered(entry_id, options_.store.workspace_id,
                           options_.store.clock());
  } catch (...) {
    // Best-effort observability field; never fail delivery over it.
    static_cast<void>(0);
  }
}

void Mailbox::acknowledge_delivery(std::string agent_id, std::string entry_id,
                                   std::string claim_token) {
  store_->acknowledge(
      AcknowledgeRequest{.entry_id = std::move(entry_id),
                         .agent_id = std::move(agent_id),
                         .claim_token = std::move(claim_token),
                         .workspace_id = options_.store.workspace_id,
                         .now_ms = options_.store.clock()});
}

void Mailbox::pump_inbox() { poll_inbox(); }

// One (attachment key, delivery, root agent/session/running, subagent maps)
// snapshot per currently-active attachment, taken under one lock so a
// concurrent attach()/detach() can't be observed half-applied.
struct Mailbox::AttachmentSnapshot {
  std::shared_ptr<MailboxDeliveryTargets> delivery;
  std::string root_agent_id;
  std::string session_id;
  bool root_running{false};
  std::unordered_set<std::string> subagent_ids;
  std::unordered_map<std::string, std::string> subagent_tasks;
};

void Mailbox::poll_inbox() {
  std::vector<AttachmentSnapshot> snapshots;
  {
    std::scoped_lock lock(mutex_);
    snapshots.reserve(attachments_.size());
    for (const auto &[key, attachment] : attachments_) {
      if (!attachment.root_active || !attachment.delivery_targets ||
          !attachment.session_id)
        continue;
      snapshots.push_back(AttachmentSnapshot{
          .delivery = attachment.delivery_targets,
          .root_agent_id = attachment.active_root_agent_id,
          .session_id = *attachment.session_id,
          .root_running = attachment.root_running,
          .subagent_ids = attachment.subagent_ids,
          .subagent_tasks = attachment.subagent_task_by_endpoint,
      });
    }
  }
  if (snapshots.empty())
    return;

  std::vector<AgentRecord> local_agents;
  try {
    local_agents = store_->list_agents(
        AgentQuery{.workspace_id = options_.store.workspace_id,
                   .include_stale = false,
                   .include_closed = false,
                   .limit = 1000,
                   .now_ms = options_.store.clock()});
  } catch (...) {
    return;
  }

  for (const auto &snapshot : snapshots) {
    for (const auto &agent : local_agents) {
      if (agent.process_id != options_.process_id || agent.agent_id.empty())
        continue;
      const bool is_root = agent.agent_id == snapshot.root_agent_id;
      if (is_root) {
        if (!snapshot.root_running || !snapshot.delivery->root)
          continue;
      } else if (!snapshot.subagent_ids.contains(agent.agent_id) ||
                 !snapshot.subagent_tasks.contains(agent.agent_id) ||
                 !snapshot.delivery->subagent || agent.status == "closing" ||
                 agent.status == "closed" || agent.status == "shutdown") {
        continue;
      }
      poll_agent_claims(snapshot, agent, is_root);
    }
  }
}

void Mailbox::poll_agent_claims(const AttachmentSnapshot &snapshot,
                                const AgentRecord &agent, bool is_root) {
  ClaimResult claimed;
  try {
    claimed = store_->claim(ClaimRequest{
        .session_id = snapshot.session_id,
        .agent_id = agent.agent_id,
        .workspace_id = options_.store.workspace_id,
        .kinds = {MailboxEntryKind::steer, MailboxEntryKind::request},
        .limit = 16,
        .now_ms = options_.store.clock(),
        .lease_ms = options_.store.claim_lease_ms});
  } catch (...) {
    return;
  }

  for (auto &claimed_message : claimed.messages) {
    const auto message_id = claimed_message.entry_id;
    const auto claim_token = claimed_message.claim_token.value_or("");
    const auto endpoint = agent.agent_id;
    const auto task_id = snapshot.subagent_tasks.contains(endpoint)
                             ? snapshot.subagent_tasks.at(endpoint)
                             : std::string{};
    mark_delivered_best_effort(message_id);
    const auto endpoint_ref = std::make_shared<const std::string>(endpoint);
    const auto message_id_ref = std::make_shared<const std::string>(message_id);
    const auto claim_token_ref =
        std::make_shared<const std::string>(claim_token);
    AgentInput envelope{
        .message = mailbox_message_to_message(claimed_message),
        .on_accepted =
            [weak = std::weak_ptr<Lifetime>(lifetime_), endpoint_ref,
             message_id_ref, claim_token_ref] noexcept {
              auto state = weak.lock();
              if (!state)
                return;
              Mailbox *owner = nullptr;
              {
                std::scoped_lock lock(state->mutex);
                if (!state->active || state->owner == nullptr)
                  return;
                owner = state->owner;
                ++state->in_flight;
              }
              try {
                owner->acknowledge_delivery(*endpoint_ref, *message_id_ref,
                                            *claim_token_ref);
              } catch (...) {
                // Lease expiry provides redelivery when acknowledgement
                // fails.
                static_cast<void>(0);
              }
              {
                std::scoped_lock lock(state->mutex);
                if (state->in_flight > 0)
                  --state->in_flight;
                if (state->in_flight == 0)
                  state->condition.notify_all();
              }
            },
        .presentation = mailbox_input_provenance(claimed_message)};
    try {
      if (is_root)
        snapshot.delivery->root(std::vector<AgentInput>{std::move(envelope)});
      else
        snapshot.delivery->subagent(
            endpoint, task_id, std::vector<AgentInput>{std::move(envelope)});
    } catch (...) {
      static_cast<void>(0);
    }
  }
}

void Mailbox::require_actor(const AgentRuntimeIdentity &actor) const {
  if (actor.agent_id.empty() || actor.session_id.empty() ||
      (actor.kind != "root" && actor.kind != "subagent"))
    throw MailboxError(MailboxErrorCode::permission_denied,
                       "mailbox actor identity is invalid");
  {
    std::scoped_lock lock(mutex_);
    const auto *attachment =
        find_attachment_for_session_locked(actor.session_id);
    if (attachment == nullptr)
      throw MailboxError(MailboxErrorCode::permission_denied,
                         "mailbox actor is not active");
    if (actor.kind == "root") {
      if (actor.agent_id != attachment->active_root_agent_id)
        throw MailboxError(MailboxErrorCode::permission_denied,
                           "mailbox actor is not the active root");
    } else {
      const auto it =
          attachment->subagent_task_by_endpoint.find(actor.agent_id);
      if (it == attachment->subagent_task_by_endpoint.end() || !actor.task_id ||
          *actor.task_id != it->second)
        throw MailboxError(MailboxErrorCode::permission_denied,
                           "mailbox subagent actor is not active");
    }
  }
  const auto live =
      store_->list_agents(AgentQuery{.agent_id = actor.agent_id,
                                     .include_stale = false,
                                     .include_closed = false,
                                     .limit = 1,
                                     .now_ms = options_.store.clock()});
  if (live.empty())
    throw MailboxError(MailboxErrorCode::permission_denied,
                       "mailbox actor lease is not live");
}

AgentRecord Mailbox::self(const AgentRuntimeIdentity &actor) {
  require_actor(actor);
  auto agents = list_agents(AgentQuery{.agent_id = actor.agent_id,
                                       .include_stale = true,
                                       .include_closed = true});
  if (agents.empty())
    throw MailboxError(MailboxErrorCode::not_found,
                       "mailbox agent is not registered");
  return std::move(agents.front());
}

std::optional<AgentRuntimeIdentity>
Mailbox::active_root_identity(const MailboxAttachmentKey &key) const {
  std::scoped_lock lock(mutex_);
  const auto it = attachments_.find(key);
  if (it == attachments_.end())
    return std::nullopt;
  const auto &attachment = it->second;
  if (!attachment.root_active || !attachment.session_id)
    return std::nullopt;
  return AgentRuntimeIdentity{.agent_id = attachment.active_root_agent_id,
                              .session_id = *attachment.session_id,
                              .kind = "root"};
}

std::vector<AgentRecord> Mailbox::list_agents(AgentQuery query) {
  query.workspace_id = options_.store.workspace_id;
  if (query.now_ms == 0)
    query.now_ms = options_.store.clock();
  return store_->list_agents(query);
}

std::vector<AgentRecord> Mailbox::list_agents(const AgentRuntimeIdentity &actor,
                                              AgentQuery query) {
  require_actor(actor);
  query.workspace_id = options_.store.workspace_id;
  if (query.now_ms == 0)
    query.now_ms = options_.store.clock();
  return store_->list_agents(query);
}

MailboxEnqueueReceipt Mailbox::send(const AgentRuntimeIdentity &actor,
                                    EnqueueMailboxEntryRequest request) {
  require_actor(actor);
  request.sender_agent_id = actor.agent_id;
  request.sender_session_id = actor.session_id;
  request.workspace_id = options_.store.workspace_id;
  return store_->send(request);
}

MailboxEnqueueReceipt Mailbox::reply(const AgentRuntimeIdentity &actor,
                                     std::string entry_id,
                                     MailboxPayload body) {
  require_actor(actor);
  const auto incoming = inspect(actor, InboxQuery{.entry_id = entry_id,
                                                  .include_acknowledged = true,
                                                  .limit = 1});
  if (incoming.empty())
    throw MailboxError(MailboxErrorCode::not_found,
                       "mailbox message not found");
  const auto &original = incoming.front();
  if (original.kind != MailboxEntryKind::request)
    throw MailboxError(MailboxErrorCode::invalid_message,
                       "mailbox reply target is not a request");
  const auto live_sender =
      list_agents(AgentQuery{.agent_id = original.sender_agent_id,
                             .include_stale = false,
                             .include_closed = false,
                             .limit = 1});
  MailboxTarget target;
  if (!live_sender.empty())
    target.agent_id = original.sender_agent_id;
  else
    target.session_id = original.sender_session_id;
  return send(actor, EnqueueMailboxEntryRequest{.target = std::move(target),
                                                .kind = MailboxEntryKind::reply,
                                                .body = std::move(body),
                                                .reply_to_entry_id =
                                                    original.entry_id});
}

std::vector<MailboxEntry> Mailbox::inspect(const AgentRuntimeIdentity &actor,
                                           InboxQuery query) {
  require_actor(actor);
  query.session_id = actor.session_id;
  query.agent_id = actor.agent_id;
  query.agent_kind = actor.kind;
  query.workspace_id = options_.store.workspace_id;
  if (query.now_ms == 0)
    query.now_ms = options_.store.clock();
  return store_->inspect(query);
}

ClaimResult Mailbox::claim(const AgentRuntimeIdentity &actor,
                           ClaimRequest request) {
  require_actor(actor);
  request.session_id = actor.session_id;
  request.agent_id = actor.agent_id;
  request.workspace_id = options_.store.workspace_id;
  if (request.now_ms == 0)
    request.now_ms = options_.store.clock();
  return store_->claim(request);
}

void Mailbox::acknowledge(const AgentRuntimeIdentity &actor,
                          AcknowledgeRequest request) {
  require_actor(actor);
  if (!request.agent_id.empty() && request.agent_id != actor.agent_id)
    throw MailboxError(MailboxErrorCode::permission_denied,
                       "mailbox acknowledgement actor mismatch");
  request.agent_id = actor.agent_id;
  request.workspace_id = options_.store.workspace_id;
  if (request.now_ms == 0)
    request.now_ms = options_.store.clock();
  store_->acknowledge(request);
}

WaitResult Mailbox::wait(WaitRequest request, std::stop_token stop_token) {
  request.workspace_id = options_.store.workspace_id;
  return store_->wait_for_change(request, std::move(stop_token));
}

void Mailbox::maintenance_loop(const std::stop_token &stop_token) {
  while (!stop_token.stop_requested()) {
    const auto now = options_.store.clock();
    maintenance_once(now, last_cleanup_ms_);
    std::unique_lock lock(mutex_);
    maintenance_wakeup_.wait_for(
        lock, stop_token,
        std::min(options_.heartbeat_interval, options_.poll_interval),
        [] { return false; });
  }
}

void Mailbox::maintenance_once(TimestampMs now, TimestampMs &last_cleanup) {
  std::scoped_lock maintenance_lock(maintenance_mutex_);
  try {
    if (next_heartbeat_ms_ == 0 || now >= next_heartbeat_ms_) {
      store_->heartbeat_process(options_.process_id, now,
                                now + options_.stale_after.count());
      next_heartbeat_ms_ = now + options_.heartbeat_interval.count();
    }
    if (now - last_cleanup >= options_.cleanup_interval.count()) {
      store_->cleanup(CleanupRequest{
          .workspace_id = options_.store.workspace_id,
          .now_ms = now,
          .acknowledged_retention_ms =
              retention_milliseconds(options_.store.retention_days),
          .stale_retention_ms =
              static_cast<std::int64_t>(7) * 24 * 60 * 60 * 1000,
      });
      last_cleanup = now;
    }
    if (next_poll_ms_ == 0 || now >= next_poll_ms_) {
      next_poll_ms_ = now + options_.poll_interval.count();
      poll_inbox();
    }
    signal_idle_root_work();
  } catch (...) {
    // A transient busy/permission failure must not kill process presence.
    static_cast<void>(0);
  }
}

void Mailbox::signal_idle_root_work() {
  std::vector<MailboxAttachmentKey> keys;
  {
    std::scoped_lock lock(mutex_);
    keys.reserve(attachments_.size());
    for (const auto &[key, attachment] : attachments_)
      keys.push_back(key);
  }
  for (const auto &key : keys) {
    if (!idle_root_work_pending(key))
      continue;
    std::shared_ptr<MailboxDeliveryTargets> delivery;
    {
      std::scoped_lock lock(mutex_);
      const auto it = attachments_.find(key);
      if (it != attachments_.end())
        delivery = it->second.delivery_targets;
    }
    if (delivery && delivery->root_wake) {
      try {
        delivery->root_wake();
      } catch (...) {
        static_cast<void>(0);
      }
    }
  }
}

void Mailbox::maintenance_tick() {
  maintenance_once(options_.store.clock(), last_cleanup_ms_);
}

MailboxAttachmentStatus Mailbox::status(const MailboxAttachmentKey &key) const {
  MailboxAttachmentStatus result;
  {
    std::scoped_lock lock(mutex_);
    result.process_id = options_.process_id;
    const auto it = attachments_.find(key);
    if (it != attachments_.end()) {
      const auto &attachment = it->second;
      result.root_agent_id = attachment.active_root_agent_id;
      result.session_id = attachment.session_id;
      result.session_name = attachment.session_name;
      result.provider = attachment.provider;
      result.model_id = attachment.model_id;
      result.root_active = attachment.root_active;
      result.root_running = attachment.root_running;
      if (attachment.root_running)
        result.status = "running";
      else if (attachment.root_active)
        result.status = "idle";
      else
        result.status = "closed";
    } else {
      result.status = "closed";
    }
  }
  result.mailbox = store_->status(
      StatusRequest{.workspace_id = options_.store.workspace_id});
  return result;
}

MailboxAttachmentStatus
Mailbox::status(const AgentRuntimeIdentity &actor) const {
  require_actor(actor);
  MailboxAttachmentStatus result;
  {
    std::scoped_lock lock(mutex_);
    result.process_id = options_.process_id;
    const auto *attachment =
        find_attachment_for_session_locked(actor.session_id);
    if (attachment != nullptr) {
      result.root_agent_id = attachment->active_root_agent_id;
      result.session_id = attachment->session_id;
      result.session_name = attachment->session_name;
      result.provider = attachment->provider;
      result.model_id = attachment->model_id;
      result.root_active = attachment->root_active;
      result.root_running = attachment->root_running;
      if (attachment->root_running)
        result.status = "running";
      else if (attachment->root_active)
        result.status = "idle";
      else
        result.status = "closed";
    } else {
      result.status = "closed";
    }
  }
  result.mailbox = store_->status(
      StatusRequest{.workspace_id = options_.store.workspace_id});
  return result;
}

MailboxStatus Mailbox::mailbox_status() const {
  return store_->status(
      StatusRequest{.workspace_id = options_.store.workspace_id});
}

MailboxStore &Mailbox::store() { return *store_; }
const MailboxStore &Mailbox::store() const { return *store_; }

void Mailbox::stop() noexcept {
  {
    std::scoped_lock lock(mutex_);
    if (stopped_)
      return;
    stopped_ = true;
  }
  {
    std::scoped_lock lock(lifetime_->mutex);
    lifetime_->active = false;
    lifetime_->owner = nullptr;
  }
  {
    std::unique_lock lock(lifetime_->mutex);
    lifetime_->condition.wait(lock,
                              [this] { return lifetime_->in_flight == 0; });
  }
  maintenance_.request_stop();
  maintenance_wakeup_.notify_all();
  if (maintenance_.joinable())
    maintenance_.join();
  std::vector<MailboxAttachmentKey> keys;
  {
    std::scoped_lock lock(mutex_);
    keys.reserve(attachments_.size());
    for (const auto &[key, attachment] : attachments_)
      keys.push_back(key);
  }
  for (const auto &key : keys) {
    try {
      deactivate_root(key);
    } catch (...) {
      static_cast<void>(0);
    }
  }
  try {
    store_->close_process(options_.process_id, options_.store.clock());
  } catch (...) {
    static_cast<void>(0);
  }
}

} // namespace pi::core
