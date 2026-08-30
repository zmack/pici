#pragma once

#include "core/agent_task.h"
#include "core/auth/auth_resolver.h"
#include "core/compaction.h"
#include "core/session/agent_session.h"

#include <atomic>
#include <functional>
#include <iosfwd>
#include <mutex>
#include <nlohmann/json.hpp>
#include <thread>
#include <vector>

namespace pi::cli {

// Long-lived, line-delimited JSON control surface for embedding pici. Commands
// arrive on stdin and all responses and agent events are emitted on stdout.
class RpcMode {
public:
  using Output = std::function<void(const nlohmann::json &)>;

  RpcMode(core::SessionRuntime &session, Output output,
          core::AgentTaskManager *task_manager = nullptr,
          std::shared_ptr<auth::AuthResolver> auth_resolver = {});
  ~RpcMode();

  RpcMode(const RpcMode &) = delete;
  RpcMode &operator=(const RpcMode &) = delete;

  void handle(const nlohmann::json &command);
  void wait_for_idle();
  void stop();

private:
  void emit(const nlohmann::json &value) const;
  void response(const nlohmann::json &command, bool success,
                nlohmann::json data = nullptr, std::string error = {}) const;
  void start_prompt(const nlohmann::json &command, std::string message);
  void start_wait(const nlohmann::json &command);
  void start_compact(const nlohmann::json &command,
                     core::CompactionTrigger trigger);

  void handle_prompt(const nlohmann::json &command);
  void handle_steer_or_follow_up(const nlohmann::json &command,
                                 const std::string &type);
  void handle_abort(const nlohmann::json &command);
  void handle_spawn_agent(const nlohmann::json &command);
  void handle_list_agents(const nlohmann::json &command);
  void handle_agent_target_command(const nlohmann::json &command,
                                   const std::string &type);
  void handle_interrupt_agent(const nlohmann::json &command);
  void handle_close_agent(const nlohmann::json &command);
  void handle_list_models(const nlohmann::json &command);
  void handle_set_model(const nlohmann::json &command);
  void handle_get_state(const nlohmann::json &command);
  void handle_get_messages(const nlohmann::json &command);
  void handle_set_thinking_level(const nlohmann::json &command);
  void handle_session_command(const nlohmann::json &command,
                              const std::string &type);

  core::SessionRuntime &session_;
  core::AgentTaskManager *task_manager_{nullptr};
  std::shared_ptr<auth::AuthResolver> auth_resolver_;
  Output output_;
  mutable std::mutex output_mutex_;
  std::atomic<bool> run_active_{false};
  std::jthread run_thread_;
  mutable std::mutex wait_mutex_;
  std::vector<std::jthread> wait_threads_;
};

int run_rpc_mode(core::SessionRuntime &session, std::istream &input,
                 std::ostream &output,
                 core::AgentTaskManager *task_manager = nullptr,
                 std::shared_ptr<auth::AuthResolver> auth_resolver = {});

} // namespace pi::cli
