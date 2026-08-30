#include "core/session/mailbox_runtime.h"

#include "core/session/agent_session.h"

#include <mutex>
#include <utility>

namespace pi::core {

std::shared_ptr<MailboxCoordinator>
start_mailbox(const MailboxLaunchOptions &options) {
  return std::make_shared<MailboxCoordinator>(options.coordinator);
}

struct MailboxRuntime::Observer {
  std::mutex mutex;
  std::weak_ptr<MailboxCoordinator> coordinator;

  void observe(const AgentTaskEvent &event) {
    std::shared_ptr<MailboxCoordinator> current;
    {
      std::scoped_lock lock(mutex);
      current = coordinator.lock();
    }
    if (current)
      current->observe_task_event(event);
  }

  void detach() {
    std::scoped_lock lock(mutex);
    coordinator.reset();
  }
};

MailboxRuntime::RootTurn::RootTurn(
    std::shared_ptr<MailboxCoordinator> coordinator, bool mark_running)
    : coordinator_(std::move(coordinator)) {
  if (coordinator_ && mark_running)
    coordinator_->set_root_running(true);
}

MailboxRuntime::RootTurn::RootTurn(RootTurn &&other) noexcept
    : coordinator_(std::move(other.coordinator_)) {}

MailboxRuntime::RootTurn &
MailboxRuntime::RootTurn::operator=(RootTurn &&other) noexcept {
  if (this == &other)
    return *this;
  if (coordinator_) {
    try {
      coordinator_->set_root_running(false);
    } catch (...) {
      static_cast<void>(0);
    }
  }
  coordinator_ = std::move(other.coordinator_);
  return *this;
}

MailboxRuntime::RootTurn::~RootTurn() noexcept {
  if (!coordinator_)
    return;
  try {
    coordinator_->set_root_running(false);
  } catch (...) {
    static_cast<void>(0);
  }
}

MailboxRuntime::MailboxRuntime(std::shared_ptr<MailboxCoordinator> coordinator)
    : coordinator_(std::move(coordinator)),
      observer_(std::make_shared<Observer>()) {
  observer_->coordinator = coordinator_;
}

MailboxRuntime::~MailboxRuntime() noexcept { shutdown(); }

bool MailboxRuntime::enabled() const noexcept {
  return static_cast<bool>(coordinator_);
}

std::shared_ptr<MailboxCoordinator> MailboxRuntime::coordinator() const {
  return coordinator_;
}

AgentTaskEventCallback MailboxRuntime::task_event_callback() const {
  if (!coordinator_)
    return {};
  return [observer = observer_](const AgentTaskEvent &event) {
    observer->observe(event);
  };
}

void MailboxRuntime::connect(SessionRuntime &root,
                             const std::shared_ptr<AgentTaskManager> &tasks,
                             std::function<void()> wake_root) {
  if (!coordinator_ || connected_)
    return;

  const std::weak_ptr<MailboxCoordinator> weak_coordinator = coordinator_;
  tasks->set_endpoint_registration(
      [weak_coordinator](const AgentTaskId &task_id,
                         const std::string &task_path,
                         const std::optional<AgentTaskId> &parent_id) {
        const auto coordinator = weak_coordinator.lock();
        if (!coordinator)
          throw AgentTaskError(AgentTaskErrorKind::invalid_state,
                               "mailbox runtime is unavailable");
        return coordinator->register_subagent(task_id, task_path, parent_id);
      },
      [weak_coordinator](const AgentTaskId &task_id) {
        if (const auto coordinator = weak_coordinator.lock())
          coordinator->unregister_subagent(task_id);
      });

  const std::weak_ptr<AgentTaskManager> weak_tasks = tasks;
  delivery_ = std::make_shared<MailboxDeliveryTargets>();
  delivery_->root = [&root](std::vector<AgentInput> messages) {
    root.agent().steer_envelopes(std::move(messages));
    return true;
  };
  delivery_->subagent = [weak_tasks](const std::string &,
                                     const std::string &task_id,
                                     std::vector<AgentInput> messages) {
    const auto tasks = weak_tasks.lock();
    if (!tasks)
      return false;
    try {
      tasks->steer_envelopes(task_id, std::move(messages));
      return true;
    } catch (...) {
      return false;
    }
  };
  delivery_->drop_queued = [&root, weak_tasks] {
    root.agent().clear_mailbox_steering_queue();
    if (const auto tasks = weak_tasks.lock())
      tasks->drop_mailbox_envelopes();
  };
  delivery_->drop_root_queued = [&root] {
    root.agent().clear_mailbox_steering_queue();
  };
  delivery_->root_wake = std::move(wake_root);
  coordinator_->attach_delivery(delivery_);
  connected_ = true;
}

void MailboxRuntime::shutdown() noexcept {
  if (coordinator_ && connected_)
    coordinator_->detach_delivery();
  connected_ = false;
  delivery_.reset();
  if (observer_)
    observer_->detach();
  if (coordinator_)
    coordinator_->stop();
}

std::optional<AgentRuntimeIdentity>
MailboxRuntime::activate_root(std::string session_id,
                              std::optional<std::string> session_name) {
  if (!coordinator_)
    return std::nullopt;
  return coordinator_->activate_root(std::move(session_id),
                                     std::move(session_name));
}

void MailboxRuntime::set_session_name(std::string session_name) {
  if (coordinator_)
    coordinator_->set_session_name(std::move(session_name));
}

void MailboxRuntime::set_model(std::string provider, std::string model_id) {
  if (coordinator_)
    coordinator_->set_model(std::move(provider), std::move(model_id));
}

void MailboxRuntime::drop_queued_delivery() {
  if (coordinator_)
    coordinator_->drop_queued_delivery();
}

void MailboxRuntime::pump_inbox() {
  if (coordinator_)
    coordinator_->pump_inbox();
}

std::vector<AgentInput>
MailboxRuntime::claim_idle_root_turn(std::size_t limit) {
  if (!coordinator_)
    return {};
  return coordinator_->claim_idle_root_turn(limit);
}

MailboxRuntime::RootTurn MailboxRuntime::begin_root_turn() {
  return RootTurn(coordinator_, true);
}

MailboxRuntime::RootTurn MailboxRuntime::adopt_root_turn() {
  return RootTurn(coordinator_, false);
}

} // namespace pi::core
