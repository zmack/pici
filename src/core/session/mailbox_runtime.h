#pragma once

// Per-session mailbox attachment: connects one Mailbox attachment slot to a
// root session, task tree, and wake callback. Per
// docs/architecture-lexicon.md's ownership table, Mailbox is a
// process-owned aggregate; this class is the per-session binding to it, not
// a peer owner -- plans/object-taxonomy-migration.md Phase 8 renamed this
// class from MailboxRuntime to MailboxAttachment and fixed shutdown() to
// detach only this session's attachment (Mailbox::detach()) instead of
// stopping the whole shared Mailbox (Mailbox::stop()), which used to make
// sharing one Mailbox across sessions unsafe.
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
  MailboxOptions coordinator;
  std::filesystem::path resolved_path;
  std::filesystem::path workspace_path;
};

std::shared_ptr<Mailbox> start_mailbox(const MailboxLaunchOptions &options);

// Application-facing adapter for one session's native mailbox binding. It
// owns all wiring between its Mailbox attachment, root session, child task
// tree, and REPL wakeup. SessionRuntime's destructor explicitly shuts its
// Agent's TaskTree down before any of its members (this one included)
// destruct, so teardown closes child tasks while the mailbox observer is
// still attached -- see core/session/session_runtime.h's ~SessionRuntime()
// and its mailbox_runtime_ declaration-order comment.
class MailboxAttachment {
public:
  class RootTurn {
  public:
    RootTurn() = default;
    RootTurn(std::shared_ptr<Mailbox> coordinator, MailboxAttachmentKey key,
             bool mark_running);
    RootTurn(const RootTurn &) = delete;
    RootTurn &operator=(const RootTurn &) = delete;
    RootTurn(RootTurn &&other) noexcept;
    RootTurn &operator=(RootTurn &&other) noexcept;
    ~RootTurn() noexcept;

  private:
    std::shared_ptr<Mailbox> coordinator_;
    MailboxAttachmentKey key_;
  };

  explicit MailboxAttachment(std::shared_ptr<Mailbox> coordinator = {});
  ~MailboxAttachment() noexcept;

  MailboxAttachment(const MailboxAttachment &) = delete;
  MailboxAttachment &operator=(const MailboxAttachment &) = delete;

  bool enabled() const noexcept;
  std::shared_ptr<Mailbox> coordinator() const;
  AgentTaskEventCallback task_event_callback() const;

  void connect(SessionRuntime &root, const std::shared_ptr<TaskTree> &tasks,
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
  std::shared_ptr<Mailbox> coordinator_;
  // Empty (and every method a safe no-op) when coordinator_ is null.
  // Assigned once, at construction, via coordinator_->attach() -- never
  // reassigned, so every forwarding call below can use it without a null
  // check beyond coordinator_ itself.
  MailboxAttachmentKey key_;
  std::shared_ptr<Observer> observer_;
  std::shared_ptr<MailboxDeliveryTargets> delivery_;
  bool connected_{false};
};

} // namespace pi::core
