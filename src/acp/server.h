#pragma once

#include "acp/types.h"
#include "core/agent.h"
#include "core/auth/auth_resolver.h"
#include "core/builtin_tools.h"
#include "core/models.h"
#include "core/sandbox.h"
#include "core/session/session_store.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace pi::acp {

struct ServerConfig;

class ServerControl {
public:
  // Idempotently request the server listening loop to stop. Safe to call from
  // a thread other than the one running run_server().
  void request_stop() noexcept;

private:
  friend void run_server(std::atomic<int> &port, ServerConfig config);
  void set_stop_callback(std::function<void()> callback);
  void clear_stop_callback();

  mutable std::mutex mutex_;
  bool stop_requested_{false};
  std::function<void()> stop_callback_;
};

struct ServerConfig {
  // ACP agent profile name this server advertises in its manifest -- not an
  // activation or session identity (a durable session/SessionRuntime is a
  // separate concept; see plans/session-runtime-migration.md Phase 6).
  std::string agent_name{"pi"};
  std::string agent_description{"pi-cpp coding agent"};
  core::Agent::Options agent_opts;
  std::shared_ptr<const core::ModelCatalog> model_catalog;
  std::shared_ptr<auth::AuthResolver> auth_resolver;
  std::vector<std::shared_ptr<const core::ToolDefinition>> tools;
  std::shared_ptr<core::SessionStore> session_store;
  core::SandboxPolicyPtr sandbox_policy;
  std::filesystem::path session_dir;
  // Optional in-process lifecycle control. A null control preserves the
  // existing process-signal-only behavior.
  std::shared_ptr<ServerControl> stop_control;
  int threads{4};
};

// Start the ACP HTTP server. Blocks until the process receives SIGINT/SIGTERM
// or an optional ServerControl requests shutdown.
// port = 0 picks a free port and sets it back into the variable (useful in
// tests). `port` is atomic because callers that run this on a background
// thread (e.g. tests) read it from a different thread while it's listening.
void run_server(std::atomic<int> &port, ServerConfig config);

// Build an AgentManifest from the server config
AgentManifest build_manifest(const ServerConfig &cfg);

} // namespace pi::acp
