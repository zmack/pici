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

  // ACP never enables auto-compaction (see cli/session_runtime.h); the
  // unused cli::Args{} below is only read when that capability is on.
  auto task_root =
      std::make_shared<core::AgentSession>(cli::build_agent_session_config(
          config.agent_opts, config.model_registry, config.tools, sessions,
          config.sandbox_policy, cli::Args{},
          cli::SessionRuntimeCapabilities{.enable_mailbox = false,
                                          .enable_hooks = false,
                                          .enable_skills = false,
                                          .enable_context_files = false,
                                          .enable_auto_compaction = false}));
  auto task_events = std::make_shared<TaskEventHub>();
  auto task_manager = std::make_shared<core::AgentTaskManager>(
      *task_root, config.agent_opts, core::AgentTaskManager::Limits{},
      [task_events](const core::AgentTaskEvent &event) {
        task_events->publish(event);
      });

  register_routes(svr, config, sessions, task_manager, task_events);

  // Determine listen address
  const char *host = "0.0.0.0";
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
}

} // namespace pi::acp
