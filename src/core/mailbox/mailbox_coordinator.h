#pragma once

#include "core/agent_task.h"
#include "core/mailbox/mailbox_store.h"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
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
  std::chrono::milliseconds cleanup_interval{30'000};
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

  void activate_root(std::string session_id,
                     std::optional<std::string> session_name = {});
  void deactivate_root();
  void set_root_running(bool running);
  void set_model(std::string provider, std::string model_id);
  void observe_task_event(const AgentTaskEvent &event);

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
  bool root_active_{false};
  bool root_running_{false};
  bool root_registered_{false};
  bool stopped_{false};
  std::jthread maintenance_;
  std::condition_variable_any maintenance_wakeup_;

  void maintenance_loop(const std::stop_token &stop_token);
  void maintenance_once(TimestampMs now, TimestampMs &last_cleanup);
  void register_subagent(const AgentTaskSpawnedEvent &event);
  static std::string task_status(AgentTaskStatusKind status);
};

using AgentTaskEventCallback = AgentTaskManager::EventCallback;
AgentTaskEventCallback
fan_out_agent_task_callbacks(std::vector<AgentTaskEventCallback> callbacks);

} // namespace pi::core
