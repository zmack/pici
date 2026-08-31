#pragma once

// Product-runtime mailbox attachment: connects a mailbox coordinator to a
// root session, task manager, and wake callback. Per
// docs/architecture-lexicon.md's ownership table, MailboxCoordinator is a
// runtime/product-service attachment, not a frontend concern -- this used
// to live in pi::cli (as cli::MailboxRuntime) but had no actual dependency
// on cli::Args or any other CLI-specific type, so
// plans/session-runtime-migration.md Phase 3 relocated it here unchanged.
//
// What stays in cli::mailbox_runtime.h: resolve_mailbox_launch_options(),
// which translates cli::Args/config into a MailboxLaunchOptions below --
// that translation is a legitimate frontend concern (reading CLI flags and
// config.toml), so it remains a thin cli-owned adapter that returns the
// core type defined here.

#include "core/agent_task.h"
#include "core/mailbox/mailbox_coordinator.h"
#include "core/models.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pi::core {

class SessionRuntime;

struct MailboxLaunchOptions {
  MailboxCoordinatorOptions coordinator;
  std::filesystem::path resolved_path;
  std::filesystem::path workspace_path;
};

std::shared_ptr<MailboxCoordinator>
start_mailbox(const MailboxLaunchOptions &options);

// Application-facing adapter for the native mailbox. It owns all wiring
// between the coordinator, root session, child task manager, and REPL
// wakeup. Declare it after SessionRuntime and before AgentTaskManager so
// teardown closes child tasks while the mailbox observer is still
// attached (see RuntimeBundle's field-order comment in
// cli/session_runtime.h, which preserves this same constraint for its own
// declaration order).
class MailboxRuntime {
public:
  class RootTurn {
  public:
    RootTurn() = default;
    RootTurn(std::shared_ptr<MailboxCoordinator> coordinator,
             bool mark_running);
    RootTurn(const RootTurn &) = delete;
    RootTurn &operator=(const RootTurn &) = delete;
    RootTurn(RootTurn &&other) noexcept;
    RootTurn &operator=(RootTurn &&other) noexcept;
    ~RootTurn() noexcept;

  private:
    std::shared_ptr<MailboxCoordinator> coordinator_;
  };

  explicit MailboxRuntime(std::shared_ptr<MailboxCoordinator> coordinator = {});
  ~MailboxRuntime() noexcept;

  MailboxRuntime(const MailboxRuntime &) = delete;
  MailboxRuntime &operator=(const MailboxRuntime &) = delete;

  bool enabled() const noexcept;
  std::shared_ptr<MailboxCoordinator> coordinator() const;
  AgentTaskEventCallback task_event_callback() const;

  void connect(SessionRuntime &root,
               const std::shared_ptr<AgentTaskManager> &tasks,
               std::function<void()> wake_root);
  void shutdown() noexcept;

  std::optional<AgentRuntimeIdentity>
  activate_root(std::string session_id,
                std::optional<std::string> session_name = {});
  void set_session_name(std::string session_name);
  void set_model(std::string provider, std::string model_id);
  void drop_queued_delivery();
  void pump_inbox();
  std::vector<AgentInput> claim_idle_root_turn(std::size_t limit = 16);
  RootTurn begin_root_turn();
  RootTurn adopt_root_turn();

private:
  struct Observer;
  std::shared_ptr<MailboxCoordinator> coordinator_;
  std::shared_ptr<Observer> observer_;
  std::shared_ptr<MailboxDeliveryTargets> delivery_;
  bool connected_{false};
};

} // namespace pi::core
