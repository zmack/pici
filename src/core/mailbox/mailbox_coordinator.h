#pragma once

#include "core/agent_task.h"
#include "core/mailbox/mailbox_store.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pi::core {

// Opaque handle a caller gets back from Mailbox::attach() and must present to
// every attachment-scoped call. Never exposed on the wire; internal-only. A
// distinct type (not a std::string alias) so overload resolution can never
// confuse a key argument with a session_id/session_name string argument --
// see activate_root()'s keyed and unscoped overloads below, which would
// otherwise be genuinely ambiguous for any 2-string-literal call.
class MailboxAttachmentKey {
public:
  MailboxAttachmentKey() = default;
  explicit MailboxAttachmentKey(std::string value) : value_(std::move(value)) {}

  bool empty() const noexcept { return value_.empty(); }
  const std::string &value() const noexcept { return value_; }
  bool operator==(const MailboxAttachmentKey &) const = default;

private:
  std::string value_;
};

} // namespace pi::core

template <> struct std::hash<pi::core::MailboxAttachmentKey> {
  std::size_t operator()(const pi::core::MailboxAttachmentKey &key) const {
    return std::hash<std::string>()(key.value());
  }
};

namespace pi::core {

struct MailboxOptions {
  MailboxStoreOptions store;
  std::string process_id;
  // Optional: when both are set, the constructor creates and immediately
  // activates one attachment automatically (using root_agent_id verbatim for
  // that attachment's first activation), matching this class's
  // pre-multi-attachment, single-root behavior exactly -- see
  // default_attachment(). New composition code (PiciProcess) should leave
  // these empty and call attach() explicitly for each session instead.
  std::string root_agent_id;
  std::string initial_session_id;
  std::optional<std::string> initial_session_name;
  std::string provider;
  std::string model_id;
  std::string hostname;
  std::int64_t pid{0};
  std::int64_t protocol_version{1};
  std::string capabilities_json{"[]"};
  std::chrono::milliseconds heartbeat_interval{2000};
  std::chrono::milliseconds stale_after{10'000};
  std::chrono::milliseconds poll_interval{250};
  std::chrono::milliseconds cleanup_interval{30'000};
};

struct MailboxAutonomousTurnBudget {
  static constexpr std::size_t max_consecutive_turns = 8;

  bool can_run() const noexcept {
    return consecutive_turns < max_consecutive_turns;
  }
  void record() noexcept { ++consecutive_turns; }
  void reset() noexcept { consecutive_turns = 0; }
  bool exhausted() const noexcept { return !can_run(); }

private:
  std::size_t consecutive_turns{0};
};

struct MailboxDeliveryTargets {
  std::function<bool(std::vector<AgentInput>)> root;
  std::function<bool(std::string, std::string, std::vector<AgentInput>)>
      subagent;
  std::function<void()> drop_queued;
  std::function<void()> drop_root_queued;
  // Called by maintenance when an idle root has actionable work.  This is a
  // wake hint only; the main thread performs the claim and runs the turn.
  std::function<void()> root_wake;
};

struct MailboxAttachmentStatus {
  std::string process_id;
  std::string root_agent_id;
  std::optional<std::string> session_id;
  std::optional<std::string> session_name;
  std::string status;
  std::string provider;
  std::string model_id;
  bool root_active{false};
  bool root_running{false};
  MailboxStatus mailbox;
};

// Durable process/workspace coordination aggregate: owns the mailbox store,
// process presence/heartbeat/cleanup maintenance, and a registry of
// independent attachments. Each attachment holds one session's root
// activation (and that root's subagents) -- attach()/detach() add and remove
// entries without disturbing any other attachment, so multiple sessions can
// share one Mailbox safely (plans/object-taxonomy-migration.md Phase 8).
// Mailbox itself never assumes there is exactly one attachment; the
// "default attachment" described on MailboxOptions above is a compatibility
// convenience for existing single-attachment call sites, not a limitation of
// the underlying design.
class Mailbox {
public:
  explicit Mailbox(MailboxOptions options);
  ~Mailbox() noexcept;

  Mailbox(const Mailbox &) = delete;
  Mailbox &operator=(const Mailbox &) = delete;
  Mailbox(Mailbox &&) = delete;
  Mailbox &operator=(Mailbox &&) = delete;

  // Registers a new, independent attachment (initially inactive: no root,
  // no subagents, no delivery). Never throws for capacity reasons -- the
  // registry has no fixed size.
  MailboxAttachmentKey attach();
  // Deactivates the attachment's root (closing its subagents the same way
  // deactivate_root() would) and removes it from the registry. Detaching an
  // unknown or already-detached key is a no-op. Never touches any other
  // attachment or the mailbox/process-level maintenance state.
  void detach(const MailboxAttachmentKey &key) noexcept;
  // One attachment the constructor always creates automatically (seeded
  // from MailboxOptions::root_agent_id/provider/model_id, and immediately
  // activated too if initial_session_id is set). Every unscoped (no
  // explicit key) overload below operates on this attachment, preserving
  // this class's pre-Phase-8 single-root call shape exactly for existing
  // callers; new multi-attachment callers should call attach() for every
  // additional session instead of relying on this one.
  const MailboxAttachmentKey &default_attachment() const noexcept {
    return default_attachment_;
  }

  AgentRuntimeIdentity
  activate_root(const MailboxAttachmentKey &key, std::string session_id,
                std::optional<std::string> session_name = {});
  void deactivate_root(const MailboxAttachmentKey &key);
  void set_session_name(const MailboxAttachmentKey &key,
                        std::string session_name);
  void set_root_running(const MailboxAttachmentKey &key, bool running);
  void set_model(const MailboxAttachmentKey &key, std::string provider,
                 std::string model_id);
  void observe_task_event(const MailboxAttachmentKey &key,
                          const AgentTaskEvent &event);
  AgentRuntimeIdentity register_subagent(const MailboxAttachmentKey &key,
                                         std::string task_id,
                                         std::string task_path,
                                         std::optional<std::string> parent_id);
  void unregister_subagent(const MailboxAttachmentKey &key,
                           std::string_view task_id);
  void attach_delivery(const MailboxAttachmentKey &key,
                       std::shared_ptr<MailboxDeliveryTargets> targets);
  void detach_delivery(const MailboxAttachmentKey &key);
  void drop_queued_delivery(const MailboxAttachmentKey &key);
  bool idle_root_work_pending(const MailboxAttachmentKey &key);
  std::vector<AgentInput> claim_idle_root_turn(const MailboxAttachmentKey &key,
                                               std::size_t limit = 16);
  std::optional<AgentRuntimeIdentity>
  active_root_identity(const MailboxAttachmentKey &key) const;
  MailboxAttachmentStatus status(const MailboxAttachmentKey &key) const;

  // Convenience overloads: operate on default_attachment(). A caller that
  // never set MailboxOptions::initial_session_id gets each method's
  // natural no-op/empty/false result (the same result an unknown key
  // produces), not a throw.
  AgentRuntimeIdentity
  activate_root(std::string session_id,
                std::optional<std::string> session_name = {}) {
    return activate_root(default_attachment_, std::move(session_id),
                         std::move(session_name));
  }
  void deactivate_root() { deactivate_root(default_attachment_); }
  void set_session_name(std::string session_name) {
    set_session_name(default_attachment_, std::move(session_name));
  }
  void set_root_running(bool running) {
    set_root_running(default_attachment_, running);
  }
  void set_model(std::string provider, std::string model_id) {
    set_model(default_attachment_, std::move(provider), std::move(model_id));
  }
  void observe_task_event(const AgentTaskEvent &event) {
    observe_task_event(default_attachment_, event);
  }
  AgentRuntimeIdentity register_subagent(std::string task_id,
                                         std::string task_path,
                                         std::optional<std::string> parent_id) {
    return register_subagent(default_attachment_, std::move(task_id),
                             std::move(task_path), std::move(parent_id));
  }
  void unregister_subagent(std::string_view task_id) {
    unregister_subagent(default_attachment_, task_id);
  }
  void attach_delivery(std::shared_ptr<MailboxDeliveryTargets> targets) {
    attach_delivery(default_attachment_, std::move(targets));
  }
  void detach_delivery() { detach_delivery(default_attachment_); }
  void drop_queued_delivery() { drop_queued_delivery(default_attachment_); }
  bool idle_root_work_pending() {
    return idle_root_work_pending(default_attachment_);
  }
  std::vector<AgentInput> claim_idle_root_turn(std::size_t limit = 16) {
    return claim_idle_root_turn(default_attachment_, limit);
  }
  std::optional<AgentRuntimeIdentity> active_root_identity() const {
    return active_root_identity(default_attachment_);
  }
  MailboxAttachmentStatus status() const { return status(default_attachment_); }

  // Unscoped: actor-carrying calls resolve their own attachment internally
  // (by the actor's session_id), so they need no explicit key. Actor
  // identities are never valid across two different attachments' sessions.
  AgentRecord self(const AgentRuntimeIdentity &actor);
  std::vector<AgentRecord> list_agents(AgentQuery query = {});
  std::vector<AgentRecord> list_agents(const AgentRuntimeIdentity &actor,
                                       AgentQuery query = {});
  MailboxEnqueueReceipt send(const AgentRuntimeIdentity &actor,
                             EnqueueMailboxEntryRequest request);
  MailboxEnqueueReceipt reply(const AgentRuntimeIdentity &actor,
                              std::string entry_id, MailboxPayload body);
  std::vector<MailboxEntry> inspect(const AgentRuntimeIdentity &actor,
                                    InboxQuery query = {});
  ClaimResult claim(const AgentRuntimeIdentity &actor, ClaimRequest request);
  void acknowledge(const AgentRuntimeIdentity &actor,
                   AcknowledgeRequest request);
  WaitResult wait(WaitRequest request, std::stop_token stop_token = {});
  // Resolves actor to its attachment by session_id (same rule
  // require_actor() uses) rather than needing that attachment's key.
  MailboxAttachmentStatus status(const AgentRuntimeIdentity &actor) const;

  // Store-wide stats only (schema/workspace/live-agent/generation counters);
  // never attachment-scoped. Use status(key) for one attachment's root state
  // alongside these same counters.
  MailboxStatus mailbox_status() const;

  // Polls every attachment's inbox and routes claimed work through whichever
  // attachment's delivery targets own the claiming agent.
  void pump_inbox();
  MailboxStore &store();
  const MailboxStore &store() const;
  void maintenance_tick();
  // Process-level shutdown: stops maintenance, then deactivates and detaches
  // every remaining attachment (not just the default one), then closes the
  // process record. Idempotent.
  void stop() noexcept;

private:
  struct Attachment {
    std::optional<std::string> session_id;
    std::optional<std::string> session_name;
    std::string active_root_agent_id;
    std::string provider;
    std::string model_id;
    std::unordered_set<std::string> subagent_ids;
    std::unordered_map<std::string, std::string> subagent_endpoint_by_task;
    std::unordered_map<std::string, std::string> subagent_task_by_endpoint;
    bool root_active{false};
    bool root_running{false};
    bool root_registered{false};
    std::shared_ptr<MailboxDeliveryTargets> delivery_targets;
  };

  MailboxOptions options_;
  std::unique_ptr<MailboxStore> store_;
  mutable std::mutex mutex_;
  std::unordered_map<MailboxAttachmentKey, Attachment> attachments_;
  MailboxAttachmentKey default_attachment_;
  bool stopped_{false};
  TimestampMs next_heartbeat_ms_{0};
  TimestampMs next_poll_ms_{0};
  TimestampMs last_cleanup_ms_{0};
  mutable std::mutex maintenance_mutex_;
  std::jthread maintenance_;
  std::condition_variable_any maintenance_wakeup_;

  struct Lifetime {
    std::mutex mutex;
    std::condition_variable condition;
    Mailbox *owner{nullptr};
    bool active{true};
    std::size_t in_flight{0};
  };
  std::shared_ptr<Lifetime> lifetime_;

  MailboxAttachmentKey new_attachment_key() const;
  // Attachment whose session_id matches actor.session_id, or nullptr. Used
  // by require_actor()/status(actor) to resolve which attachment an actor
  // identity belongs to without the caller naming a key explicitly.
  Attachment *find_attachment_for_session_locked(std::string_view session_id);
  const Attachment *
  find_attachment_for_session_locked(std::string_view session_id) const;

  // One attachment's delivery/root/subagent state, snapshotted under mutex_
  // so poll_inbox() can claim/route without holding it.
  struct AttachmentSnapshot;
  void maintenance_loop(const std::stop_token &stop_token);
  void maintenance_once(TimestampMs now, TimestampMs &last_cleanup);
  void poll_inbox();
  // Claims and routes one already-registered agent's due mailbox entries for
  // one attachment. Split out of poll_inbox() purely to shrink that
  // function's branch count; behavior is unchanged from the inline version
  // it replaced.
  void poll_agent_claims(const AttachmentSnapshot &snapshot,
                         const AgentRecord &agent, bool is_root);
  void signal_idle_root_work();
  void acknowledge_delivery(std::string agent_id, std::string entry_id,
                            std::string claim_token);
  // Best-effort delivered_at_ms commit (plans/session-runtime-migration.md
  // Phase 6): swallows any store error so a failure to record this
  // observability field never fails delivery itself. A free function, not
  // inlined at its two call sites, so claim_idle_root_turn()/poll_inbox()
  // pay a plain call's complexity instead of an inline try/catch's nesting.
  void mark_delivered_best_effort(const std::string &entry_id);
  void require_actor(const AgentRuntimeIdentity &actor) const;
  static std::string task_status(AgentTaskStatusKind status);
};

// TODO(taxonomy-phase-10): remove. These are the migration-era names; all
// new code must use Mailbox/MailboxOptions/MailboxAttachmentStatus.
using MailboxCoordinator = Mailbox;
using MailboxCoordinatorOptions = MailboxOptions;
using MailboxCoordinatorStatus = MailboxAttachmentStatus;

using AgentTaskEventCallback = AgentTaskManager::EventCallback;
AgentTaskEventCallback
fan_out_agent_task_callbacks(std::vector<AgentTaskEventCallback> callbacks);

} // namespace pi::core
