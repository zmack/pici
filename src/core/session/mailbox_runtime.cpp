#include "core/session/mailbox_runtime.h"

#include "core/mailbox/mailbox_coordinator.h"
#include "core/session/session_runtime.h"

#include <mutex>
#include <utility>

namespace pi::core {

std::shared_ptr<Mailbox> start_mailbox(const MailboxLaunchOptions &options) {
  return std::make_shared<Mailbox>(options.coordinator);
}

struct MailboxAttachment::Observer {
  std::mutex mutex;
  std::weak_ptr<Mailbox> coordinator;
  MailboxAttachmentKey key;

  void observe(const AgentTaskEvent &event) {
    std::shared_ptr<Mailbox> current;
    MailboxAttachmentKey observed_key;
    {
      std::scoped_lock lock(mutex);
      current = coordinator.lock();
      observed_key = key;
    }
    if (current)
      current->observe_task_event(observed_key, event);
  }

  void detach() {
    std::scoped_lock lock(mutex);
    coordinator.reset();
  }
};

MailboxAttachment::RootTurn::RootTurn(std::shared_ptr<Mailbox> coordinator,
                                      MailboxAttachmentKey key,
                                      bool mark_running)
    : coordinator_(std::move(coordinator)), key_(std::move(key)) {
  if (coordinator_ && mark_running)
    coordinator_->set_root_running(key_, true);
}

MailboxAttachment::RootTurn::RootTurn(RootTurn &&other) noexcept
    : coordinator_(std::move(other.coordinator_)), key_(std::move(other.key_)) {
}

MailboxAttachment::RootTurn &
MailboxAttachment::RootTurn::operator=(RootTurn &&other) noexcept {
  if (this == &other)
    return *this;
  if (coordinator_) {
    try {
      coordinator_->set_root_running(key_, false);
    } catch (...) {
      static_cast<void>(0);
    }
  }
  coordinator_ = std::move(other.coordinator_);
  key_ = std::move(other.key_);
  return *this;
}

MailboxAttachment::RootTurn::~RootTurn() noexcept {
  if (!coordinator_)
    return;
  try {
    coordinator_->set_root_running(key_, false);
  } catch (...) {
    static_cast<void>(0);
  }
}

MailboxAttachment::MailboxAttachment(std::shared_ptr<Mailbox> coordinator)
    : coordinator_(std::move(coordinator)),
      observer_(std::make_shared<Observer>()) {
  if (coordinator_)
    key_ = coordinator_->attach();
  observer_->coordinator = coordinator_;
  observer_->key = key_;
}

MailboxAttachment::~MailboxAttachment() noexcept { shutdown(); }

bool MailboxAttachment::enabled() const noexcept {
  return static_cast<bool>(coordinator_);
}

std::shared_ptr<Mailbox> MailboxAttachment::coordinator() const {
  return coordinator_;
}

AgentTaskEventCallback MailboxAttachment::task_event_callback() const {
  if (!coordinator_)
    return {};
  return [observer = observer_](const AgentTaskEvent &event) {
    observer->observe(event);
  };
}

void MailboxAttachment::connect(SessionRuntime &root,
                                const std::shared_ptr<TaskTree> &tasks,
                                std::function<void()> wake_root) {
  if (!coordinator_ || connected_)
    return;

  const std::weak_ptr<Mailbox> weak_coordinator = coordinator_;
  const auto key = key_;
  tasks->set_endpoint_registration(
      [weak_coordinator, key](const AgentTaskId &task_id,
                              const std::string &task_path,
                              const std::optional<AgentTaskId> &parent_id) {
        const auto coordinator = weak_coordinator.lock();
        if (!coordinator)
          throw AgentTaskError(AgentTaskErrorKind::invalid_state,
                               "mailbox runtime is unavailable");
        return coordinator->register_subagent(key, task_id, task_path,
                                              parent_id);
      },
      [weak_coordinator, key](const AgentTaskId &task_id) {
        if (const auto coordinator = weak_coordinator.lock())
          coordinator->unregister_subagent(key, task_id);
      });

  const std::weak_ptr<TaskTree> weak_tasks = tasks;
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
  coordinator_->attach_delivery(key_, delivery_);
  connected_ = true;
}

void MailboxAttachment::shutdown() noexcept {
  if (coordinator_ && connected_)
    coordinator_->detach_delivery(key_);
  connected_ = false;
  delivery_.reset();
  if (observer_)
    observer_->detach();
  if (coordinator_)
    coordinator_->detach(key_);
}

std::optional<AgentRuntimeIdentity>
MailboxAttachment::activate_root(std::string session_id,
                                 std::optional<std::string> session_name) {
  if (!coordinator_)
    return std::nullopt;
  return coordinator_->activate_root(key_, std::move(session_id),
                                     std::move(session_name));
}

void MailboxAttachment::set_session_name(std::string session_name) {
  if (coordinator_)
    coordinator_->set_session_name(key_, std::move(session_name));
}

void MailboxAttachment::set_model(std::string provider, std::string model_id) {
  if (coordinator_)
    coordinator_->set_model(key_, std::move(provider), std::move(model_id));
}

void MailboxAttachment::drop_queued_delivery() {
  if (coordinator_)
    coordinator_->drop_queued_delivery(key_);
}

void MailboxAttachment::pump_inbox() {
  if (coordinator_)
    coordinator_->pump_inbox();
}

std::vector<AgentInput>
MailboxAttachment::claim_idle_root_turn(std::size_t limit) {
  if (!coordinator_)
    return {};
  return coordinator_->claim_idle_root_turn(key_, limit);
}

MailboxAttachment::RootTurn MailboxAttachment::begin_root_turn() {
  return RootTurn(coordinator_, key_, true);
}

MailboxAttachment::RootTurn MailboxAttachment::adopt_root_turn() {
  return RootTurn(coordinator_, key_, false);
}

} // namespace pi::core
