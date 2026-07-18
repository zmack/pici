#pragma once

#include "core/agent_task.h"
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

  RpcMode(core::AgentSession &session, Output output,
          core::AgentTaskManager *task_manager = nullptr);
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

  core::AgentSession &session_;
  core::AgentTaskManager *task_manager_{nullptr};
  Output output_;
  mutable std::mutex output_mutex_;
  std::atomic<bool> run_active_{false};
  std::jthread run_thread_;
  mutable std::mutex wait_mutex_;
  std::vector<std::jthread> wait_threads_;
};

int run_rpc_mode(core::AgentSession &session, std::istream &input,
                 std::ostream &output,
                 core::AgentTaskManager *task_manager = nullptr);

} // namespace pi::cli
