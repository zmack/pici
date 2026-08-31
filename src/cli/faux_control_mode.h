#pragma once

#include "core/providers/faux_control.h"
#include "core/session/session_runtime.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <unordered_set>

namespace pi::cli {

class FauxControlMode {
public:
  using Output = std::function<void(const nlohmann::json &)>;
  using ToolRegistrar = std::function<void(const std::string &)>;
  using EventObserver = std::function<void(const core::AgentEvent &)>;

  FauxControlMode(core::SessionRuntime &session, core::RemoteFauxClient &client,
                  std::shared_ptr<core::ScriptedToolRegistry> tool_registry,
                  Output output, ToolRegistrar tool_registrar = {},
                  EventObserver event_observer = {});
  ~FauxControlMode();
  FauxControlMode(const FauxControlMode &) = delete;
  FauxControlMode &operator=(const FauxControlMode &) = delete;

  void handle(const nlohmann::json &command);
  void wait_for_idle();
  void stop(bool close_client = true);
  bool turn_active() const { return run_active_.load(); }
  bool quit_requested() const { return should_quit_.load(); }

private:
  void emit(const nlohmann::json &value) const;
  void response(const nlohmann::json &command, bool success,
                nlohmann::json data = nullptr, std::string error = {}) const;
  void handle_round(const nlohmann::json &command);
  void handle_turn(const nlohmann::json &command);

  core::SessionRuntime &session_;
  core::RemoteFauxClient &client_;
  std::shared_ptr<core::ScriptedToolRegistry> tool_registry_;
  Output output_;
  ToolRegistrar tool_registrar_;
  EventObserver event_observer_;
  mutable std::mutex output_mutex_;
  std::unordered_set<std::string> known_tool_names_;
  std::atomic<bool> run_active_{false};
  std::atomic<bool> should_quit_{false};
  std::jthread run_thread_;
};

int run_faux_control_socket(
    core::SessionRuntime &session, core::RemoteFauxClient &client,
    const std::shared_ptr<core::ScriptedToolRegistry> &tool_registry,
    const std::string &socket_path,
    const FauxControlMode::ToolRegistrar &tool_registrar = {},
    const FauxControlMode::EventObserver &event_observer = {});

} // namespace pi::cli
