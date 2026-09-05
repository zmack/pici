#include "core/process/pici_process.h"

#include "core/auth/authentication.h"
#include "core/llm_client.h"
#include "core/mailbox/mailbox_coordinator.h"
#include "core/mcp/mcp_client.h"
#include "core/mcp/mcp_tool.h"
#include "core/models.h"
#include "core/session/session_store.h"

#include <chrono>
#include <exception>
#include <filesystem>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pi::core {

namespace {

std::shared_ptr<ModelCatalog> build_model_catalog(
    const std::map<std::string, ProviderConfig> &providers,
    std::shared_ptr<ModelDiscoveryAdapterCollection> discovery_adapters,
    std::shared_ptr<InferenceAdapterCollection> inference_adapters) {
  auto catalog = std::make_shared<ModelCatalog>(
      providers, std::move(discovery_adapters), std::move(inference_adapters));
  catalog->validate_registered_apis();
  return catalog;
}

// Per-server initialize() timeout: generous for a slow-to-start node/python
// MCP server, but still bounded so one hung server can't hang startup
// forever.
constexpr auto kMcpHandshakeTimeout = std::chrono::milliseconds(5000);

// Overall cap on how long PiciProcess construction waits, in total, for
// every configured server's handshake to settle (success, failure, or its
// own internal kMcpHandshakeTimeout firing) -- NOT summed per server. Since
// every handshake runs concurrently (one std::async task each), N
// configured servers add roughly max(handshake time), not
// sum(handshake time), to startup, capped here so a run with many
// configured servers still can't stall a `-p` one-shot or --list-models
// invocation indefinitely. Set slightly above kMcpHandshakeTimeout so a
// server that legitimately needs its own full internal timeout to fail
// still gets to report that failure cleanly rather than being cut off by
// the overall cap first.
constexpr auto kMcpOverallHandshakeCap = std::chrono::milliseconds(8000);

struct McpHandshakeAttempt {
  std::string server_name;
  std::shared_ptr<mcp::McpClient> client;
  std::future<void> handshake;
};

// Spawns + handshakes every ENABLED configured server concurrently (one
// std::async task per server) and merges the tools of every server whose
// handshake succeeds within the overall cap. A server that fails to spawn,
// fails its handshake, or times out is logged to std::cerr as a warning and
// simply contributes no tools -- this function never throws.
std::vector<std::shared_ptr<const ToolDefinition>>
discover_mcp_tools(const std::vector<mcp::McpServerConfig> &configs) {
  std::vector<McpHandshakeAttempt> attempts;
  attempts.reserve(configs.size());

  for (const auto &server : configs) {
    if (!server.enabled)
      continue;

    mcp::McpServerProcessConfig process_config;
    process_config.command = server.command;
    process_config.args = server.args;
    process_config.env = server.env;
    process_config.cwd = server.cwd;

    std::shared_ptr<mcp::McpClient> client;
    try {
      // McpClient's constructor spawns the child (via McpStdioTransport) --
      // cheap (a fork+exec), which is why this part stays eager/serial;
      // only the handshake itself (a round-trip that can legitimately take
      // a while) is pushed onto its own thread below.
      client = std::make_shared<mcp::McpClient>(server.name,
                                                std::move(process_config));
    } catch (const std::exception &err) {
      std::cerr << "mcp: server \"" << server.name
                << "\" failed to start: " << err.what() << "\n";
      continue;
    }

    auto handshake = std::async(std::launch::async, [client] {
      client->initialize(kMcpHandshakeTimeout);
    });
    attempts.push_back(McpHandshakeAttempt{
        .server_name = server.name,
        .client = std::move(client),
        .handshake = std::move(handshake),
    });
  }

  const auto deadline =
      std::chrono::steady_clock::now() + kMcpOverallHandshakeCap;

  std::vector<std::shared_ptr<const ToolDefinition>> tools;
  // Cross-server collision check only -- each client's own list_tools()
  // already checks its tools against builtins and its own siblings (see
  // McpClient::list_tools()/mcp_tool.cpp's claim_tool_name), so this set
  // only needs to catch the rare case of two DIFFERENT servers producing
  // the same sanitized/qualified name.
  std::unordered_set<std::string> taken_names;

  for (auto &attempt : attempts) {
    const auto now = std::chrono::steady_clock::now();
    const auto remaining =
        now < deadline
            ? std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
                                                                     now)
            : std::chrono::milliseconds(0);

    bool handshake_ok = false;
    if (attempt.handshake.wait_for(remaining) == std::future_status::ready) {
      try {
        attempt.handshake.get();
        handshake_ok = true;
      } catch (const std::exception &err) {
        std::cerr << "mcp: server \"" << attempt.server_name
                  << "\" handshake failed: " << err.what() << "\n";
      }
    } else {
      std::cerr << "mcp: server \"" << attempt.server_name
                << "\" handshake did not complete within the overall mcp "
                   "startup cap; skipping\n";
    }
    if (!handshake_ok)
      continue;

    try {
      for (auto &tool : attempt.client->list_tools()) {
        if (!mcp::claim_tool_name(std::string(tool->name()),
                                  attempt.server_name, taken_names))
          continue;
        tools.push_back(std::move(tool));
      }
    } catch (const std::exception &err) {
      std::cerr << "mcp: server \"" << attempt.server_name
                << "\" tools/list failed: " << err.what() << "\n";
    }
  }

  return tools;
}

} // namespace

PiciProcess::PiciProcess(const Config &config)
    : model_catalog_(build_model_catalog(config.providers,
                                         config.discovery_adapters,
                                         config.inference_adapters)),
      authentication_(std::make_shared<auth::Authentication>(
          model_catalog_, credential_store_)),
      session_store_(std::make_shared<SessionStore>(
          config.session_dir.empty()
              ? SessionStore::default_sessions_dir()
              : std::filesystem::path(config.session_dir))),
      mcp_tools_(discover_mcp_tools(config.mcp_servers)) {}

std::shared_ptr<Mailbox>
PiciProcess::ensure_mailbox(const MailboxOptions &options) const {
  std::scoped_lock lock(mailbox_mutex_);
  if (!mailbox_)
    mailbox_ = std::make_shared<Mailbox>(options);
  return mailbox_;
}

} // namespace pi::core
