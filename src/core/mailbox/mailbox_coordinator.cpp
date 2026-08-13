#include "core/mailbox/mailbox_coordinator.h"
#include "core/agent_loop.h"
#include "core/agent_task.h"
#include "core/mailbox/mailbox_store.h"
#include "core/mailbox/mailbox_types.h"
#include "core/message_types.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
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

Message mailbox_message_to_message(const MailboxMessage &message) {
  UserMessage user;
  user.content.emplace_back(TextContent{
      .text = "[Mailbox message " + message.message_id + " from session " +
              message.sender_session_id + " / agent " +
              message.sender_agent_id + "]\n" + message.body.text});
  return Message{std::move(user)};
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

MailboxCoordinator::MailboxCoordinator(MailboxCoordinatorOptions options)
    : options_(std::move(options)), provider_(options_.provider),
      model_id_(options_.model_id),
      active_root_agent_id_(options_.root_agent_id),
      lifetime_(std::make_shared<Lifetime>()) {
  if (options_.process_id.empty() || options_.root_agent_id.empty())
    throw MailboxError(MailboxErrorCode::invalid_message,
                       "mailbox coordinator identities are required");
  if (options_.heartbeat_interval <= std::chrono::milliseconds::zero())
    throw MailboxError(MailboxErrorCode::invalid_message,
                       "mailbox heartbeat interval must be positive");
  if (options_.cleanup_interval <= std::chrono::milliseconds::zero())
    options_.cleanup_interval = options_.heartbeat_interval;
  if (options_.poll_interval <= std::chrono::milliseconds::zero())
    options_.poll_interval = std::chrono::milliseconds(250);

  lifetime_->owner = this;

  store_ = std::make_unique<MailboxStore>(options_.store);
  const auto now = options_.store.clock();
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
      .lease_expires_at_ms = now + (options_.heartbeat_interval.count() * 3),
  });
  maintenance_ = std::jthread([this](const std::stop_token &stop_token) {
    maintenance_loop(stop_token);
  });
  if (!options_.initial_session_id.empty())
    activate_root(options_.initial_session_id, options_.initial_session_name);
}

MailboxCoordinator::~MailboxCoordinator() noexcept { stop(); }

void MailboxCoordinator::activate_root(
    std::string session_id, std::optional<std::string> session_name) {
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
    old_session = session_id_;
    old_root_agent_id = active_root_agent_id_;
    old_subagent_ids = subagent_ids_;
    provider = provider_;
    model = model_id_;
    delivery = delivery_targets_;
    if (root_registered_)
      root_agent_id = options_.store.id_generator();
    else
      root_agent_id = active_root_agent_id_;
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
  {
    std::scoped_lock lock(mutex_);
    active_root_agent_id_ = std::move(root_agent_id);
    session_id_ = std::move(session_id);
    session_name_ = std::move(session_name);
    root_active_ = true;
    root_running_ = false;
    root_registered_ = true;
    subagent_ids_.clear();
    subagent_endpoint_by_task_.clear();
    subagent_task_by_endpoint_.clear();
  }
}

void MailboxCoordinator::deactivate_root() {
  std::optional<std::string> active;
  std::string root_agent_id;
  std::unordered_set<std::string> subagent_ids;
  std::shared_ptr<MailboxDeliveryTargets> delivery;
  {
    std::scoped_lock lock(mutex_);
    if (!root_active_)
      return;
    active = session_id_;
    root_agent_id = active_root_agent_id_;
    subagent_ids = subagent_ids_;
    delivery = delivery_targets_;
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
  if (root_active_ && active_root_agent_id_ == root_agent_id) {
    root_active_ = false;
    root_running_ = false;
    session_id_.reset();
    session_name_.reset();
    subagent_ids_.clear();
    subagent_endpoint_by_task_.clear();
    subagent_task_by_endpoint_.clear();
  }
}

void MailboxCoordinator::set_root_running(bool running) {
  std::string status;
  std::string root_agent_id;
  {
    std::scoped_lock lock(mutex_);
    if (!root_active_ || root_running_ == running)
      return;
    status = running ? "running" : "idle";
    root_agent_id = active_root_agent_id_;
  }
  store_->update_agent(
      AgentUpdate{.agent_id = root_agent_id, .status = std::move(status)});
  std::scoped_lock lock(mutex_);
  if (root_active_ && active_root_agent_id_ == root_agent_id)
    root_running_ = running;
}

void MailboxCoordinator::set_model(std::string provider, std::string model_id) {
  std::optional<std::string> active;
  std::string root_agent_id;
  {
    std::scoped_lock lock(mutex_);
    active = session_id_;
    if (!active) {
      provider_ = std::move(provider);
      model_id_ = std::move(model_id);
      return;
    }
    root_agent_id = active_root_agent_id_;
  }
  if (active)
    store_->update_agent(AgentUpdate{
        .agent_id = root_agent_id, .provider = provider, .model_id = model_id});
  std::scoped_lock lock(mutex_);
  if (root_active_ && active_root_agent_id_ == root_agent_id) {
    provider_ = std::move(provider);
    model_id_ = std::move(model_id);
  }
}

void MailboxCoordinator::register_subagent(const AgentTaskSpawnedEvent &event) {
  std::optional<std::string> session;
  std::string provider;
  std::string model;
  std::string root_agent_id;
  {
    std::scoped_lock lock(mutex_);
    if (!root_active_)
      return;
    session = session_id_;
    provider = provider_;
    model = model_id_;
    root_agent_id = active_root_agent_id_;
  }
  if (!session)
    return;
  std::string endpoint;
  {
    std::scoped_lock lock(mutex_);
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
      .owner_agent_id = root_agent_id,
      .session_id = *session,
      .task_id = event.id,
      .task_path = event.task_path,
      .provider = provider,
      .model_id = model,
      .status = "pending",
      .started_at_ms = now,
  });
  bool belongs_to_current_session = false;
  {
    std::scoped_lock lock(mutex_);
    belongs_to_current_session = root_active_ && session_id_ == session &&
                                 active_root_agent_id_ == root_agent_id;
    if (belongs_to_current_session)
      subagent_ids_.insert(endpoint);
    if (belongs_to_current_session) {
      subagent_endpoint_by_task_[event.id] = endpoint;
      subagent_task_by_endpoint_[endpoint] = event.id;
    }
  }
  if (!belongs_to_current_session) {
    try {
      store_->close_agent(endpoint, now);
    } catch (...) {
      static_cast<void>(0);
    }
  }
}

std::string MailboxCoordinator::task_status(AgentTaskStatusKind status) {
  return std::string(agent_task_status_to_string(status));
}

void MailboxCoordinator::observe_task_event(const AgentTaskEvent &event) {
  if (const auto *spawned = std::get_if<AgentTaskSpawnedEvent>(&event)) {
    register_subagent(*spawned);
  } else if (const auto *changed =
                 std::get_if<AgentTaskStatusChangedEvent>(&event)) {
    std::string endpoint;
    bool owned = false;
    {
      std::scoped_lock lock(mutex_);
      const auto it = subagent_endpoint_by_task_.find(changed->id);
      owned = root_active_ && it != subagent_endpoint_by_task_.end();
      if (owned)
        endpoint = it->second;
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
      const auto it = subagent_endpoint_by_task_.find(closed->id);
      owned = root_active_ && it != subagent_endpoint_by_task_.end();
      if (owned)
        endpoint = it->second;
    }
    if (owned) {
      store_->close_agent(endpoint, options_.store.clock());
      std::scoped_lock lock(mutex_);
      subagent_ids_.erase(endpoint);
      subagent_endpoint_by_task_.erase(closed->id);
      subagent_task_by_endpoint_.erase(endpoint);
    }
  }
}

void MailboxCoordinator::attach_delivery(
    std::shared_ptr<MailboxDeliveryTargets> targets) {
  std::scoped_lock lock(mutex_);
  delivery_targets_ = std::move(targets);
}

void MailboxCoordinator::detach_delivery() {
  std::scoped_lock lock(mutex_);
  delivery_targets_.reset();
}

void MailboxCoordinator::drop_queued_delivery() {
  std::shared_ptr<MailboxDeliveryTargets> delivery;
  {
    std::scoped_lock lock(mutex_);
    delivery = delivery_targets_;
  }
  if (delivery && delivery->drop_queued)
    delivery->drop_queued();
}

void MailboxCoordinator::acknowledge_delivery(std::string agent_id,
                                              std::string message_id,
                                              std::string claim_token) {
  store_->acknowledge(
      AcknowledgeRequest{.message_id = std::move(message_id),
                         .agent_id = std::move(agent_id),
                         .claim_token = std::move(claim_token),
                         .workspace_id = options_.store.workspace_id,
                         .now_ms = options_.store.clock()});
}

void MailboxCoordinator::pump_inbox() { poll_inbox(); }

void MailboxCoordinator::poll_inbox() {
  std::shared_ptr<MailboxDeliveryTargets> delivery;
  std::string root_agent_id;
  std::optional<std::string> session_id;
  bool root_running = false;
  std::unordered_set<std::string> subagent_ids;
  std::unordered_map<std::string, std::string> subagent_tasks;
  {
    std::scoped_lock lock(mutex_);
    delivery = delivery_targets_;
    if (!root_active_ || !delivery)
      return;
    root_agent_id = active_root_agent_id_;
    session_id = session_id_;
    root_running = root_running_;
    subagent_ids = subagent_ids_;
    subagent_tasks = subagent_task_by_endpoint_;
  }
  if (!session_id)
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

  for (const auto &agent : local_agents) {
    if (agent.process_id != options_.process_id || agent.agent_id.empty())
      continue;
    const bool is_root = agent.agent_id == root_agent_id;
    if (is_root) {
      if (!root_running || !delivery->root)
        continue;
    } else if (!subagent_ids.contains(agent.agent_id) ||
               !subagent_tasks.contains(agent.agent_id) ||
               !delivery->subagent || agent.status == "closing" ||
               agent.status == "closed" || agent.status == "shutdown") {
      continue;
    }

    ClaimResult claimed;
    try {
      claimed = store_->claim(ClaimRequest{
          .session_id = *session_id,
          .agent_id = agent.agent_id,
          .workspace_id = options_.store.workspace_id,
          .kinds = {MailboxMessageKind::steer, MailboxMessageKind::request},
          .limit = 16,
          .now_ms = options_.store.clock(),
          .lease_ms = options_.store.claim_lease_ms});
    } catch (...) {
      continue;
    }

    for (auto &claimed_message : claimed.messages) {
      const auto message_id = claimed_message.message_id;
      const auto claim_token = claimed_message.claim_token.value_or("");
      const auto endpoint = agent.agent_id;
      const auto task_id = subagent_tasks.contains(endpoint)
                               ? subagent_tasks.at(endpoint)
                               : std::string{};
      AgentMessageEnvelope envelope{
          .message = mailbox_message_to_message(claimed_message),
          .on_accepted =
              [weak = std::weak_ptr<Lifetime>(lifetime_), endpoint, message_id,
               claim_token] noexcept {
                auto state = weak.lock();
                if (!state)
                  return;
                MailboxCoordinator *owner = nullptr;
                {
                  std::scoped_lock lock(state->mutex);
                  if (!state->active || state->owner == nullptr)
                    return;
                  owner = state->owner;
                  ++state->in_flight;
                }
                try {
                  owner->acknowledge_delivery(endpoint, message_id,
                                              claim_token);
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
          .source = AgentMessageSource::mailbox};
      bool routed = false;
      try {
        if (is_root)
          routed = delivery->root(
              std::vector<AgentMessageEnvelope>{std::move(envelope)});
        else
          routed = delivery->subagent(
              endpoint, task_id,
              std::vector<AgentMessageEnvelope>{std::move(envelope)});
      } catch (...) {
        routed = false;
      }
      if (!routed)
        continue;
    }
  }
}

AgentRecord MailboxCoordinator::self() {
  std::string agent_id;
  {
    std::scoped_lock lock(mutex_);
    if (!root_active_)
      throw MailboxError(MailboxErrorCode::not_found,
                         "mailbox session is not active");
    agent_id = active_root_agent_id_;
  }
  auto agents = list_agents(AgentQuery{.agent_id = std::move(agent_id),
                                       .include_stale = true,
                                       .include_closed = true});
  if (agents.empty())
    throw MailboxError(MailboxErrorCode::not_found,
                       "mailbox agent is not registered");
  return std::move(agents.front());
}

std::vector<AgentRecord> MailboxCoordinator::list_agents(AgentQuery query) {
  query.workspace_id = options_.store.workspace_id;
  if (query.now_ms == 0)
    query.now_ms = options_.store.clock();
  return store_->list_agents(query);
}

SendReceipt MailboxCoordinator::send(SendRequest request) {
  std::string session;
  std::string agent;
  {
    std::scoped_lock lock(mutex_);
    if (!root_active_)
      throw MailboxError(MailboxErrorCode::not_found,
                         "mailbox session is not active");
    if (!session_id_)
      throw MailboxError(MailboxErrorCode::not_found,
                         "mailbox session is not active");
    session = *session_id_;
    agent = active_root_agent_id_;
  }
  request.sender_agent_id = std::move(agent);
  request.sender_session_id = std::move(session);
  request.workspace_id = options_.store.workspace_id;
  return store_->send(request);
}

std::vector<MailboxMessage> MailboxCoordinator::inspect(InboxQuery query) {
  std::string session;
  std::string agent;
  {
    std::scoped_lock lock(mutex_);
    if (!root_active_)
      throw MailboxError(MailboxErrorCode::not_found,
                         "mailbox session is not active");
    if (!session_id_)
      throw MailboxError(MailboxErrorCode::not_found,
                         "mailbox session is not active");
    session = *session_id_;
    agent = active_root_agent_id_;
  }
  query.session_id = std::move(session);
  query.agent_id = std::move(agent);
  query.workspace_id = options_.store.workspace_id;
  if (query.now_ms == 0)
    query.now_ms = options_.store.clock();
  return store_->inspect(query);
}

ClaimResult MailboxCoordinator::claim(ClaimRequest request) {
  std::string session;
  std::string agent;
  {
    std::scoped_lock lock(mutex_);
    if (!root_active_)
      throw MailboxError(MailboxErrorCode::not_found,
                         "mailbox session is not active");
    if (!session_id_)
      throw MailboxError(MailboxErrorCode::not_found,
                         "mailbox session is not active");
    session = *session_id_;
    agent = active_root_agent_id_;
  }
  request.session_id = std::move(session);
  request.agent_id = std::move(agent);
  request.workspace_id = options_.store.workspace_id;
  if (request.now_ms == 0)
    request.now_ms = options_.store.clock();
  return store_->claim(request);
}

void MailboxCoordinator::acknowledge(AcknowledgeRequest request) {
  {
    std::scoped_lock lock(mutex_);
    if (!root_active_)
      throw MailboxError(MailboxErrorCode::not_found,
                         "mailbox session is not active");
    request.agent_id = active_root_agent_id_;
  }
  request.workspace_id = options_.store.workspace_id;
  if (request.now_ms == 0)
    request.now_ms = options_.store.clock();
  store_->acknowledge(request);
}

WaitResult MailboxCoordinator::wait(WaitRequest request,
                                    std::stop_token stop_token) {
  request.workspace_id = options_.store.workspace_id;
  return store_->wait_for_change(request, std::move(stop_token));
}

void MailboxCoordinator::maintenance_loop(const std::stop_token &stop_token) {
  auto last_cleanup = options_.store.clock();
  while (!stop_token.stop_requested()) {
    const auto now = options_.store.clock();
    maintenance_once(now, last_cleanup);
    std::unique_lock lock(mutex_);
    maintenance_wakeup_.wait_for(
        lock, stop_token,
        std::min(options_.heartbeat_interval, options_.poll_interval),
        [] { return false; });
  }
}

void MailboxCoordinator::maintenance_once(TimestampMs now,
                                          TimestampMs &last_cleanup) {
  try {
    if (next_heartbeat_ms_ == 0 || now >= next_heartbeat_ms_) {
      store_->heartbeat_process(options_.process_id, now,
                                now +
                                    (options_.heartbeat_interval.count() * 3));
      next_heartbeat_ms_ = now + options_.heartbeat_interval.count();
    }
    if (now - last_cleanup >= options_.cleanup_interval.count()) {
      store_->cleanup(CleanupRequest{
          .workspace_id = options_.store.workspace_id,
          .now_ms = now,
          .acknowledged_retention_ms =
              options_.store.retention_days * 24 * 60 * 60 * 1000,
          .stale_retention_ms =
              static_cast<std::int64_t>(7) * 24 * 60 * 60 * 1000,
      });
      last_cleanup = now;
    }
    if (next_poll_ms_ == 0 || now >= next_poll_ms_) {
      next_poll_ms_ = now + options_.poll_interval.count();
      poll_inbox();
    }
  } catch (...) {
    // A transient busy/permission failure must not kill process presence.
    static_cast<void>(0);
  }
}

void MailboxCoordinator::maintenance_tick() {
  auto last_cleanup = options_.store.clock();
  maintenance_once(options_.store.clock(), last_cleanup);
}

MailboxCoordinatorStatus MailboxCoordinator::status() const {
  MailboxCoordinatorStatus result;
  {
    std::scoped_lock lock(mutex_);
    result.process_id = options_.process_id;
    result.root_agent_id = active_root_agent_id_;
    result.session_id = session_id_;
    result.session_name = session_name_;
    result.provider = provider_;
    result.model_id = model_id_;
    result.root_active = root_active_;
    result.root_running = root_running_;
    if (root_running_)
      result.status = "running";
    else if (root_active_)
      result.status = "idle";
    else
      result.status = "closed";
  }
  result.mailbox = store_->status(
      StatusRequest{.workspace_id = options_.store.workspace_id});
  return result;
}

MailboxStore &MailboxCoordinator::store() { return *store_; }
const MailboxStore &MailboxCoordinator::store() const { return *store_; }

void MailboxCoordinator::stop() noexcept {
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
  try {
    deactivate_root();
    store_->close_process(options_.process_id, options_.store.clock());
  } catch (...) {
    static_cast<void>(0);
  }
}

} // namespace pi::core
