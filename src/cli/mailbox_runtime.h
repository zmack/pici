#pragma once

#include "cli/args.h"
#include "core/agent_task.h"
#include "core/mailbox/mailbox_coordinator.h"
#include "core/models.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace pi::core {
class AgentSession;
}

namespace pi::cli {

struct MailboxLaunchOptions {
  core::MailboxCoordinatorOptions coordinator;
  std::filesystem::path resolved_path;
  std::filesystem::path workspace_path;
};

MailboxLaunchOptions
resolve_mailbox_launch_options(const Args &args, const core::Model &model,
                               std::filesystem::path workspace_path);

std::shared_ptr<core::MailboxCoordinator>
start_mailbox(const MailboxLaunchOptions &options);

// Application-facing adapter for the native mailbox. It owns all wiring
// between the coordinator, root session, child task manager, and REPL wakeup.
// Declare it after AgentSession and before AgentTaskManager so teardown closes
// child tasks while the mailbox observer is still attached.
class MailboxRuntime {
public:
  class RootTurn {
  public:
    RootTurn() = default;
    RootTurn(std::shared_ptr<core::MailboxCoordinator> coordinator,
             bool mark_running);
    RootTurn(const RootTurn &) = delete;
    RootTurn &operator=(const RootTurn &) = delete;
    RootTurn(RootTurn &&other) noexcept;
    RootTurn &operator=(RootTurn &&other) noexcept;
    ~RootTurn() noexcept;

  private:
    std::shared_ptr<core::MailboxCoordinator> coordinator_;
  };

  explicit MailboxRuntime(
      std::shared_ptr<core::MailboxCoordinator> coordinator = {});
  ~MailboxRuntime() noexcept;

  MailboxRuntime(const MailboxRuntime &) = delete;
  MailboxRuntime &operator=(const MailboxRuntime &) = delete;

  bool enabled() const noexcept;
  std::shared_ptr<core::MailboxCoordinator> coordinator() const;
  core::AgentTaskEventCallback task_event_callback() const;

  void connect(core::AgentSession &root,
               const std::shared_ptr<core::AgentTaskManager> &tasks,
               std::function<void()> wake_root);
  void shutdown() noexcept;

  std::optional<core::AgentRuntimeIdentity>
  activate_root(std::string session_id,
                std::optional<std::string> session_name = {});
  void set_session_name(std::string session_name);
  void set_model(std::string provider, std::string model_id);
  void drop_queued_delivery();
  void pump_inbox();
  std::vector<core::AgentMessageEnvelope>
  claim_idle_root_turn(std::size_t limit = 16);
  RootTurn begin_root_turn();
  RootTurn adopt_root_turn();

private:
  struct Observer;
  std::shared_ptr<core::MailboxCoordinator> coordinator_;
  std::shared_ptr<Observer> observer_;
  std::shared_ptr<core::MailboxDeliveryTargets> delivery_;
  bool connected_{false};
};

} // namespace pi::cli
