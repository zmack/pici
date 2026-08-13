#include "core/mailbox/mailbox_coordinator.h"
#include "core/agent_task.h"
#include "core/mailbox/mailbox_store.h"
#include "core/mailbox/mailbox_types.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace pi::core {

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
      active_root_agent_id_(options_.root_agent_id) {
  if (options_.process_id.empty() || options_.root_agent_id.empty())
    throw MailboxError(MailboxErrorCode::invalid_message,
                       "mailbox coordinator identities are required");
  if (options_.heartbeat_interval <= std::chrono::milliseconds::zero())
    throw MailboxError(MailboxErrorCode::invalid_message,
                       "mailbox heartbeat interval must be positive");
  if (options_.cleanup_interval <= std::chrono::milliseconds::zero())
    options_.cleanup_interval = options_.heartbeat_interval;

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
  {
    std::scoped_lock lock(mutex_);
    if (stopped_)
      throw MailboxError(MailboxErrorCode::internal,
                         "mailbox coordinator is stopped");
    old_session = session_id_;
    old_root_agent_id = active_root_agent_id_;
    session_id_ = session_id;
    session_name_ = std::move(session_name);
    root_active_ = true;
    root_running_ = false;
  }
  const auto now = options_.store.clock();
  std::optional<std::string> active_name;
  std::string provider;
  std::string model;
  std::string root_agent_id;
  {
    std::scoped_lock lock(mutex_);
    active_name = session_name_;
    provider = provider_;
    model = model_id_;
    if (old_session)
      active_root_agent_id_ = options_.store.id_generator();
    root_agent_id = active_root_agent_id_;
  }
  if (old_session)
    store_->close_agent(old_root_agent_id, now);
  store_->register_agent(AgentRecord{
      .agent_id = std::move(root_agent_id),
      .process_id = options_.process_id,
      .kind = "root",
      .session_id = std::move(session_id),
      .session_name = std::move(active_name),
      .provider = std::move(provider),
      .model_id = std::move(model),
      .status = "starting",
      .started_at_ms = now,
  });
}

void MailboxCoordinator::deactivate_root() {
  std::optional<std::string> active;
  std::string root_agent_id;
  {
    std::scoped_lock lock(mutex_);
    if (!root_active_)
      return;
    active = session_id_;
    root_active_ = false;
    root_running_ = false;
    session_id_.reset();
    session_name_.reset();
    root_agent_id = active_root_agent_id_;
  }
  if (active)
    store_->close_agent(root_agent_id, options_.store.clock());
}

void MailboxCoordinator::set_root_running(bool running) {
  std::string status;
  std::string root_agent_id;
  {
    std::scoped_lock lock(mutex_);
    if (!root_active_ || root_running_ == running)
      return;
    root_running_ = running;
    status = running ? "running" : "idle";
    root_agent_id = active_root_agent_id_;
  }
  store_->update_agent(AgentUpdate{.agent_id = std::move(root_agent_id),
                                   .status = std::move(status)});
}

void MailboxCoordinator::set_model(std::string provider, std::string model_id) {
  {
    std::scoped_lock lock(mutex_);
    provider_ = std::move(provider);
    model_id_ = std::move(model_id);
  }
  std::optional<std::string> active;
  std::string root_agent_id;
  std::string current_provider;
  std::string current_model;
  {
    std::scoped_lock lock(mutex_);
    active = session_id_;
    current_provider = provider_;
    current_model = model_id_;
    root_agent_id = active_root_agent_id_;
  }
  if (active)
    store_->update_agent(AgentUpdate{.agent_id = std::move(root_agent_id),
                                     .provider = std::move(current_provider),
                                     .model_id = std::move(current_model)});
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
  const auto now = options_.store.clock();
  store_->register_agent(AgentRecord{
      .agent_id = event.id,
      .process_id = options_.process_id,
      .kind = "subagent",
      .owner_agent_id = std::move(root_agent_id),
      .session_id = *session,
      .task_id = event.id,
      .task_path = event.task_path,
      .provider = std::move(provider),
      .model_id = std::move(model),
      .status = "pending",
      .started_at_ms = now,
  });
}

std::string MailboxCoordinator::task_status(AgentTaskStatusKind status) {
  return std::string(agent_task_status_to_string(status));
}

void MailboxCoordinator::observe_task_event(const AgentTaskEvent &event) {
  if (const auto *spawned = std::get_if<AgentTaskSpawnedEvent>(&event)) {
    register_subagent(*spawned);
  } else if (const auto *changed =
                 std::get_if<AgentTaskStatusChangedEvent>(&event)) {
    bool active = false;
    {
      std::scoped_lock lock(mutex_);
      active = root_active_;
    }
    if (active) {
      const auto status = task_status(changed->current);
      store_->update_agent(
          AgentUpdate{.agent_id = changed->id, .status = status});
    }
  } else if (const auto *closed = std::get_if<AgentTaskClosedEvent>(&event)) {
    bool active = false;
    {
      std::scoped_lock lock(mutex_);
      active = root_active_;
    }
    if (active) {
      store_->close_agent(closed->id, options_.store.clock());
    }
  }
}

void MailboxCoordinator::maintenance_loop(const std::stop_token &stop_token) {
  auto last_cleanup = options_.store.clock();
  while (!stop_token.stop_requested()) {
    const auto now = options_.store.clock();
    maintenance_once(now, last_cleanup);
    std::unique_lock lock(mutex_);
    maintenance_wakeup_.wait_for(lock, stop_token, options_.heartbeat_interval,
                                 [] { return false; });
  }
}

void MailboxCoordinator::maintenance_once(TimestampMs now,
                                          TimestampMs &last_cleanup) {
  try {
    store_->heartbeat_process(options_.process_id, now,
                              now + (options_.heartbeat_interval.count() * 3));
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
