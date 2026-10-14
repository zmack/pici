#include "core/process/pici_process.h"

#include "core/auth/authentication.h"
#include "core/llm_client.h"
#include "core/mailbox/mailbox_coordinator.h"
#include "core/mcp/mcp_client.h"
#include "core/mcp/mcp_http_transport.h"
#include "core/mcp/mcp_tool.h"
#include "core/models.h"
#include "core/session/session_store.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
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

// Bound for a handshake retried from refresh_mcp_tools(): it runs on a
// session-creation path, so it is shorter than the startup handshake.
constexpr auto kMcpRefreshHandshakeTimeout = std::chrono::milliseconds(2000);

mcp::McpClient::TransportFactory
make_transport_factory(const mcp::McpServerConfig &server) {
  if (server.url.empty()) {
    mcp::McpServerProcessConfig process_config;
    process_config.command = server.command;
    process_config.args = server.args;
    process_config.env = server.env;
    process_config.cwd = server.cwd;
    return [process_config] {
      return std::make_unique<mcp::McpStdioTransport>(process_config);
    };
  }
  // The token is resolved per transport (so a rotated env var is picked up
  // on reconnect) and goes straight into the transport; only the variable
  // NAME is ever logged.
  return [url = server.url, headers = server.headers,
          token_env = server.bearer_token_env] {
    mcp::McpHttpConfig http;
    http.url = url;
    http.headers = headers;
    if (!token_env.empty()) {
      const char *value =
          std::getenv(token_env.c_str()); // NOLINT(concurrency-mt-unsafe)
      if (value == nullptr || *value == '\0')
        throw std::runtime_error("bearer token environment variable " +
                                 token_env + " is not set");
      http.bearer_token = value;
    }
    return std::make_unique<mcp::McpHttpTransport>(std::move(http));
  };
}

std::vector<std::shared_ptr<const ToolDefinition>>
merge_mcp_tools(const std::vector<PiciProcess::McpServerEntry> &servers) {
  std::vector<std::shared_ptr<const ToolDefinition>> tools;
  // Cross-server collision check only -- each client's own list_tools()
  // already checks its tools against builtins and its own siblings (see
  // McpClient::list_tools()/mcp_tool.cpp's claim_tool_name), so this set
  // only needs to catch the rare case of two DIFFERENT servers producing
  // the same sanitized/qualified name.
  std::unordered_set<std::string> taken_names;
  for (const auto &server : servers) {
    for (const auto &tool : server.tools) {
      if (!mcp::claim_tool_name(std::string(tool->name()), server.name,
                                taken_names))
        continue;
      tools.push_back(tool);
    }
  }
  return tools;
}

// Spawns/builds + handshakes every ENABLED configured server concurrently
// (one std::async task per server) and lists the tools of every server whose
// handshake succeeds within the overall cap. A server that fails to start,
// fails its handshake, or times out is logged to std::cerr as a warning and
// contributes no tools (its client is kept so refresh_mcp_tools() can retry);
// this function never throws.
std::vector<PiciProcess::McpServerEntry>
start_mcp_servers(const std::vector<mcp::McpServerConfig> &configs) {
  std::vector<McpHandshakeAttempt> attempts;
  attempts.reserve(configs.size());

  for (const auto &server : configs) {
    if (!server.enabled)
      continue;

    const std::optional<std::chrono::milliseconds> call_timeout =
        server.call_timeout_ms
            ? std::optional<std::chrono::milliseconds>(
                  std::chrono::milliseconds(*server.call_timeout_ms))
            : std::nullopt;

    std::shared_ptr<mcp::McpClient> client;
    try {
      // McpClient's constructor builds the transport (a fork+exec for stdio,
      // nothing at all for HTTP) -- cheap, which is why this part stays
      // eager/serial; only the handshake itself (a round-trip that can
      // legitimately take a while) is pushed onto its own thread below.
      client = std::make_shared<mcp::McpClient>(
          server.name, make_transport_factory(server), call_timeout);
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

  std::vector<PiciProcess::McpServerEntry> servers;
  for (auto &attempt : attempts) {
    PiciProcess::McpServerEntry entry{
        .name = attempt.server_name, .client = attempt.client, .tools = {}};
    const auto now = std::chrono::steady_clock::now();
    const auto remaining =
        now < deadline ? std::chrono::duration_cast<std::chrono::milliseconds>(
                             deadline - now)
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
    if (handshake_ok) {
      try {
        entry.tools = attempt.client->list_tools();
      } catch (const std::exception &err) {
        std::cerr << "mcp: server \"" << attempt.server_name
                  << "\" tools/list failed: " << err.what() << "\n";
      }
    }
    servers.push_back(std::move(entry));
  }

  return servers;
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
      mcp_servers_(start_mcp_servers(config.mcp_servers)),
      mcp_tools_(merge_mcp_tools(mcp_servers_)) {}

std::vector<std::shared_ptr<const ToolDefinition>>
PiciProcess::mcp_tools() const {
  bool needs_refresh = false;
  {
    std::scoped_lock lock(mcp_mutex_);
    needs_refresh = std::ranges::any_of(mcp_servers_, [](const auto &server) {
      return server.client->tools_stale() || !server.client->is_alive() ||
             !server.client->is_initialized();
    });
  }
  if (needs_refresh)
    return refresh_mcp_tools();
  std::scoped_lock lock(mcp_mutex_);
  return mcp_tools_;
}

std::vector<std::shared_ptr<const ToolDefinition>>
PiciProcess::refresh_mcp_tools() const {
  // One refresh at a time: it does network/child I/O, so mcp_mutex_ (which
  // guards the published snapshot) is not held across it.
  std::scoped_lock refresh_lock(mcp_refresh_mutex_);

  std::vector<McpServerEntry> snapshot;
  {
    std::scoped_lock lock(mcp_mutex_);
    snapshot = mcp_servers_;
  }

  for (auto &server : snapshot) {
    auto &client = *server.client;
    try {
      if (!client.is_alive() || !client.is_initialized()) {
        std::string error;
        if (!client.reconnect(&error, kMcpRefreshHandshakeTimeout)) {
          std::cerr << "mcp: server \"" << server.name
                    << "\" reconnect failed: " << error << "\n";
          continue;
        }
      }
      if (client.tools_stale())
        server.tools = client.list_tools();
    } catch (const std::exception &err) {
      std::cerr << "mcp: server \"" << server.name
                << "\" tool refresh failed: " << err.what() << "\n";
    }
  }

  auto merged = merge_mcp_tools(snapshot);
  std::scoped_lock lock(mcp_mutex_);
  mcp_servers_ = std::move(snapshot);
  mcp_tools_ = merged;
  return merged;
}

std::shared_ptr<Mailbox>
PiciProcess::ensure_mailbox(const MailboxOptions &options) const {
  std::scoped_lock lock(mailbox_mutex_);
  if (!mailbox_)
    mailbox_ = std::make_shared<Mailbox>(options);
  return mailbox_;
}

} // namespace pi::core
