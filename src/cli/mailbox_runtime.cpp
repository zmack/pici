#include "cli/mailbox_runtime.h"

#include "cli/config.h"
#include "core/session/agent_session.h"
#include "core/session/session_id.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <unistd.h>
#include <utility>

namespace pi::cli {

namespace {

std::string local_hostname() {
  std::array<char, 256> buffer{};
  if (::gethostname(buffer.data(), buffer.size() - 1) == 0) {
    buffer.back() = '\0';
    return {buffer.data()};
  }
  return "local";
}

std::string workspace_identity(const std::filesystem::path &workspace_path) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (const auto character : workspace_path.string()) {
    hash ^= static_cast<unsigned char>(character);
    hash *= 1099511628211ULL;
  }
  std::ostringstream result;
  result << "workspace-" << std::hex << hash;
  return result.str();
}

} // namespace

MailboxLaunchOptions
resolve_mailbox_launch_options(const Args &args, const core::Model &model,
                               std::filesystem::path workspace_path) {
  std::error_code canonical_error;
  const auto canonical =
      std::filesystem::weakly_canonical(workspace_path, canonical_error);
  if (!canonical_error)
    workspace_path = canonical;
  else
    workspace_path = workspace_path.lexically_normal();

  const auto settings =
      args.config_document ? args.config_document->mailbox : MailboxSettings{};
  std::filesystem::path mailbox_path =
      args.mailbox_path.empty() ? settings.path
                                : std::filesystem::path(args.mailbox_path);
  if (mailbox_path.empty())
    mailbox_path = default_mailbox_path(
        args.config_path.empty() ? default_config_path()
                                 : std::filesystem::path(args.config_path));

  MailboxLaunchOptions result;
  result.resolved_path = mailbox_path;
  result.workspace_path = workspace_path;
  auto &options = result.coordinator;
  options.store.path = std::move(mailbox_path);
  options.store.workspace_id = workspace_identity(workspace_path);
  options.store.workspace_path = workspace_path.string();
  options.store.scope = settings.scope == "global"
                            ? core::MailboxScope::global
                            : core::MailboxScope::workspace;
  options.store.claim_lease_ms = settings.claim_lease_ms;
  options.store.retention_days = settings.retention_days;
  options.store.clock = [] {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  };
  options.store.id_generator = [] { return core::generate_session_id(); };
  options.process_id = core::generate_session_id();
  options.root_agent_id = core::generate_session_id();
  options.provider = model.provider;
  options.model_id = model.id;
  options.hostname = local_hostname();
  options.heartbeat_interval =
      std::chrono::milliseconds(settings.heartbeat_interval_ms);
  options.stale_after = std::chrono::milliseconds(settings.stale_after_ms);
  options.poll_interval = std::chrono::milliseconds(settings.poll_interval_ms);
  options.cleanup_interval = std::chrono::hours(1);
  return result;
}

std::shared_ptr<core::MailboxCoordinator>
start_mailbox(const MailboxLaunchOptions &options) {
  return std::make_shared<core::MailboxCoordinator>(options.coordinator);
}

struct MailboxRuntime::Observer {
  std::mutex mutex;
  std::weak_ptr<core::MailboxCoordinator> coordinator;

  void observe(const core::AgentTaskEvent &event) {
    std::shared_ptr<core::MailboxCoordinator> current;
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
    std::shared_ptr<core::MailboxCoordinator> coordinator, bool mark_running)
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

MailboxRuntime::MailboxRuntime(
    std::shared_ptr<core::MailboxCoordinator> coordinator)
    : coordinator_(std::move(coordinator)),
      observer_(std::make_shared<Observer>()) {
  observer_->coordinator = coordinator_;
}

MailboxRuntime::~MailboxRuntime() noexcept { shutdown(); }

bool MailboxRuntime::enabled() const noexcept {
  return static_cast<bool>(coordinator_);
}

std::shared_ptr<core::MailboxCoordinator> MailboxRuntime::coordinator() const {
  return coordinator_;
}

core::AgentTaskEventCallback MailboxRuntime::task_event_callback() const {
  if (!coordinator_)
    return {};
  return [observer = observer_](const core::AgentTaskEvent &event) {
    observer->observe(event);
  };
}

void MailboxRuntime::connect(
    core::AgentSession &root,
    const std::shared_ptr<core::AgentTaskManager> &tasks,
    std::function<void()> wake_root) {
  if (!coordinator_ || connected_)
    return;

  const std::weak_ptr<core::MailboxCoordinator> weak_coordinator = coordinator_;
  tasks->set_endpoint_registration(
      [weak_coordinator](const core::AgentTaskId &task_id,
                         const std::string &task_path,
                         const std::optional<core::AgentTaskId> &parent_id) {
        const auto coordinator = weak_coordinator.lock();
        if (!coordinator)
          throw core::AgentTaskError(core::AgentTaskErrorKind::invalid_state,
                                     "mailbox runtime is unavailable");
        return coordinator->register_subagent(task_id, task_path, parent_id);
      },
      [weak_coordinator](const core::AgentTaskId &task_id) {
        if (const auto coordinator = weak_coordinator.lock())
          coordinator->unregister_subagent(task_id);
      });

  const std::weak_ptr<core::AgentTaskManager> weak_tasks = tasks;
  delivery_ = std::make_shared<core::MailboxDeliveryTargets>();
  delivery_->root = [&root](std::vector<core::AgentMessageEnvelope> messages) {
    root.agent().steer_envelopes(std::move(messages));
    return true;
  };
  delivery_->subagent =
      [weak_tasks](const std::string &, const std::string &task_id,
                   std::vector<core::AgentMessageEnvelope> messages) {
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

std::optional<core::AgentRuntimeIdentity>
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

std::vector<core::AgentMessageEnvelope>
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

} // namespace pi::cli
