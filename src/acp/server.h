#pragma once

#include "acp/session_store.h"
#include "acp/types.h"
#include "core/agent.h"
#include "core/builtin_tools.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pi::acp {

struct ServerConfig {
  std::string agent_name{"pi"};
  std::string agent_description{"pi-cpp coding agent"};
  core::Agent::Options agent_opts;
  std::vector<std::shared_ptr<const core::ToolDefinition>> tools;
  int threads{4};
};

// Start the ACP HTTP server.  Blocks until the process receives SIGINT/SIGTERM.
// port = 0 picks a free port and sets it back into the variable (useful in
// tests).
void run_server(int &port, ServerConfig config);

// Build an AgentManifest from the server config
AgentManifest build_manifest(const ServerConfig &cfg);

} // namespace pi::acp
