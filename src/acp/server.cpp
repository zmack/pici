#include "acp/server.h"
#include "acp/handlers.h"
#include "acp/task_events.h"
#include "acp/types.h"
#include "cli/session_runtime.h"
#include "core/agent_task.h"
#include "core/session/agent_session.h"
#include "core/session/session_id.h"
#include "core/session/session_store.h"
#include "nlohmann/json_fwd.hpp"

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <httplib.h>

#include <csignal>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace pi::acp {

void ServerControl::request_stop() noexcept {
  std::function<void()> callback;
  {
    std::lock_guard lock(mutex_);
    stop_requested_ = true;
    callback = stop_callback_;
  }
  if (callback)
    callback();
}

void ServerControl::set_stop_callback(std::function<void()> callback) {
  bool already_requested = false;
  {
    std::lock_guard lock(mutex_);
    stop_callback_ = std::move(callback);
    already_requested = stop_requested_;
  }
  if (already_requested)
    request_stop();
}

void ServerControl::clear_stop_callback() {
  std::lock_guard lock(mutex_);
  stop_callback_ = {};
}

AgentManifest build_manifest(const ServerConfig &cfg) {
  AgentManifest m;
  m.name = cfg.agent_name;
  m.description = cfg.agent_description;

  // Expose tool names in metadata
  nlohmann::json tool_names = nlohmann::json::array();
  for (const auto &t : cfg.tools)
    tool_names.push_back(std::string(t->name()));
  m.metadata = {{"framework", "pi-cpp"},
                {"version", PI_CPP_VERSION},
                {"tools", std::move(tool_names)}};
  return m;
}

void run_server(std::atomic<int> &port, ServerConfig config) {
  auto sessions = config.session_store;
  if (!sessions) {
    auto session_dir = config.session_dir;
    if (session_dir.empty())
      session_dir = std::filesystem::temp_directory_path() /
                    ("pici-acp-" + core::generate_session_id());
    sessions = std::make_shared<core::SessionStore>(std::move(session_dir));
  }

  httplib::Server svr;
  svr.new_task_queue = [&config] -> httplib::TaskQueue * {
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): cpp-httplib owns it.
    return new httplib::ThreadPool(static_cast<std::size_t>(config.threads));
  };

  // Process-wide scratch SessionRuntime backing the /tasks/* HTTP surface
  // (list/spawn/get/message/interrupt/close), kept independent of any
  // durable /runs session: per
  // plans/session-runtime-migration.md Phase 6, ACP's /runs handling moved
  // to one SessionRuntime per durable session, looked up/reused by
  // session_id (see handlers.cpp's per-run registry) -- but /runs has never
  // wired those sessions to AgentTaskManager (ACP's per-run sessions have
  // never supported subagent delegation; SessionRuntimeCapabilities keeps
  // it off), and the /tasks/* wire API has no session_id concept at all to
  // route by (task ids like "root"/"agent_1" are only unique within one
  // AgentTaskManager, so a registry-search-by-id across multiple durable
  // sessions' task trees would collide). Unifying /tasks/* with per-session
  // task trees is therefore a real wire-protocol question left to a later
  // phase, not something this restructuring silently resolves -- this
  // scratch runtime preserves today's behavior exactly, just through the
  // renamed SessionRuntime type and its activate() method.
  //
  // ACP never enables auto-compaction (see cli/session_runtime.h); the
  // unused cli::Args{} below is only read when that capability is on.
  auto scratch_runtime =
      std::make_shared<core::SessionRuntime>(cli::build_agent_session_config(
          config.agent_opts, config.model_catalog, config.tools, sessions,
          config.sandbox_policy, cli::Args{},
          cli::SessionRuntimeCapabilities{.enable_mailbox = false,
                                          .enable_hooks = false,
                                          .enable_skills = false,
                                          .enable_context_files = false,
                                          .enable_auto_compaction = false}));
  auto task_events = std::make_shared<TaskEventHub>();
  scratch_runtime->activate(config.agent_opts, core::AgentTaskManager::Limits{},
                            core::AgentTaskManager::ChildWriteTools::none,
                            [task_events](const core::AgentTaskEvent &event) {
                              task_events->publish(event);
                            });

  register_routes(svr, config, sessions, scratch_runtime->task_manager(),
                  task_events);

  // Determine listen address
  const char *host = "0.0.0.0";
  if (config.stop_control)
    config.stop_control->set_stop_callback([&svr] { svr.stop(); });

  try {
    if (port == 0) {
      // bind_to_any_port returns the assigned port (negative on failure).
      const int bound_port = svr.bind_to_any_port(host);
      if (bound_port <= 0)
        throw std::runtime_error("Failed to bind to any port");
      port = bound_port;
      std::cerr << "[acp] listening on " << host << ":" << port << "\n";
      if (!svr.listen_after_bind())
        throw std::runtime_error("Failed to listen after binding");
    } else {
      std::cerr << "[acp] listening on " << host << ":" << port << "\n";
      if (!svr.listen(host, port)) {
        throw std::runtime_error("Failed to listen on port " +
                                 std::to_string(port));
      }
    }
  } catch (...) {
    if (config.stop_control)
      config.stop_control->clear_stop_callback();
    throw;
  }
  if (config.stop_control)
    config.stop_control->clear_stop_callback();
}

} // namespace pi::acp
