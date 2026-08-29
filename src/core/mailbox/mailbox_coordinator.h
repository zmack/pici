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
#include <vector>

namespace pi::core {

struct MailboxCoordinatorOptions {
  MailboxStoreOptions store;
  std::string process_id;
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
  std::function<bool(std::vector<AgentMessageEnvelope>)> root;
  std::function<bool(std::string, std::string,
                     std::vector<AgentMessageEnvelope>)>
      subagent;
  std::function<void()> drop_queued;
  std::function<void()> drop_root_queued;
  // Called by maintenance when an idle root has actionable work.  This is a
  // wake hint only; the main thread performs the claim and runs the turn.
  std::function<void()> root_wake;
};

struct MailboxCoordinatorStatus {
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

class MailboxCoordinator {
public:
  explicit MailboxCoordinator(MailboxCoordinatorOptions options);
  ~MailboxCoordinator() noexcept;

  MailboxCoordinator(const MailboxCoordinator &) = delete;
  MailboxCoordinator &operator=(const MailboxCoordinator &) = delete;
  MailboxCoordinator(MailboxCoordinator &&) = delete;
  MailboxCoordinator &operator=(MailboxCoordinator &&) = delete;

  AgentRuntimeIdentity
  activate_root(std::string session_id,
                std::optional<std::string> session_name = {});
  void deactivate_root();
  void set_session_name(std::string session_name);
  void set_root_running(bool running);
  void set_model(std::string provider, std::string model_id);
  void observe_task_event(const AgentTaskEvent &event);
  AgentRuntimeIdentity register_subagent(std::string task_id,
                                         std::string task_path,
                                         std::optional<std::string> parent_id);
  void unregister_subagent(std::string_view task_id);
  void attach_delivery(std::shared_ptr<MailboxDeliveryTargets> targets);
  void detach_delivery();
  void pump_inbox();
  void drop_queued_delivery();
  bool idle_root_work_pending();
  std::vector<AgentMessageEnvelope>
  claim_idle_root_turn(std::size_t limit = 16);

  AgentRecord self(const AgentRuntimeIdentity &actor);
  std::optional<AgentRuntimeIdentity> active_root_identity() const;
  std::vector<AgentRecord> list_agents(AgentQuery query = {});
  std::vector<AgentRecord> list_agents(const AgentRuntimeIdentity &actor,
                                       AgentQuery query = {});
  SendReceipt send(const AgentRuntimeIdentity &actor, SendRequest request);
  SendReceipt reply(const AgentRuntimeIdentity &actor, std::string message_id,
                    MailboxBody body);
  std::vector<MailboxMessage> inspect(const AgentRuntimeIdentity &actor,
                                      InboxQuery query = {});
  ClaimResult claim(const AgentRuntimeIdentity &actor, ClaimRequest request);
  void acknowledge(const AgentRuntimeIdentity &actor,
                   AcknowledgeRequest request);
  WaitResult wait(WaitRequest request, std::stop_token stop_token = {});

  MailboxCoordinatorStatus status() const;
  MailboxStore &store();
  const MailboxStore &store() const;
  void maintenance_tick();
  void stop() noexcept;

private:
  MailboxCoordinatorOptions options_;
  std::unique_ptr<MailboxStore> store_;
  mutable std::mutex mutex_;
  std::optional<std::string> session_id_;
  std::optional<std::string> session_name_;
  std::string active_root_agent_id_;
  std::string provider_;
  std::string model_id_;
  std::unordered_set<std::string> subagent_ids_;
  std::unordered_map<std::string, std::string> subagent_endpoint_by_task_;
  std::unordered_map<std::string, std::string> subagent_task_by_endpoint_;
  bool root_active_{false};
  bool root_running_{false};
  bool root_registered_{false};
  bool stopped_{false};
  TimestampMs next_heartbeat_ms_{0};
  TimestampMs next_poll_ms_{0};
  TimestampMs last_cleanup_ms_{0};
  mutable std::mutex maintenance_mutex_;
  std::jthread maintenance_;
  std::condition_variable_any maintenance_wakeup_;
  std::shared_ptr<MailboxDeliveryTargets> delivery_targets_;

  struct Lifetime {
    std::mutex mutex;
    std::condition_variable condition;
    MailboxCoordinator *owner{nullptr};
    bool active{true};
    std::size_t in_flight{0};
  };
  std::shared_ptr<Lifetime> lifetime_;

  void maintenance_loop(const std::stop_token &stop_token);
  void maintenance_once(TimestampMs now, TimestampMs &last_cleanup);
  void poll_inbox();
  void signal_idle_root_work();
  void acknowledge_delivery(std::string agent_id, std::string message_id,
                            std::string claim_token);
  void require_actor(const AgentRuntimeIdentity &actor) const;
  static std::string task_status(AgentTaskStatusKind status);
};

using AgentTaskEventCallback = AgentTaskManager::EventCallback;
AgentTaskEventCallback
fan_out_agent_task_callbacks(std::vector<AgentTaskEventCallback> callbacks);

} // namespace pi::core
