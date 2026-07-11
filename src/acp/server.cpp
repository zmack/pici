#include "acp/server.h"
#include "acp/handlers.h"
#include "acp/session_store.h"
#include "acp/types.h"
#include "nlohmann/json_fwd.hpp"

#include <cstddef>
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
  auto sessions = std::make_shared<SessionStore>();

  httplib::Server svr;
  svr.new_task_queue = [&config] {
    return new httplib::ThreadPool(static_cast<std::size_t>(config.threads));
  };

  register_routes(svr, config, sessions);

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
