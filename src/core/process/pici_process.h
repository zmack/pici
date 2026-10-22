#pragma once

// PiciProcess is the process composition root (docs/object-taxonomy.md): it
// owns the process-lifetime ModelCatalog, Authentication, SessionStore, and
// Mailbox instances that CLI and ACP would otherwise assemble independently.
// See plans/object-taxonomy-migration.md Phases 5 and 8.

#include "core/auth/authentication.h"
#include "core/auth/credential_store.h"
#include "core/lua_worker_tool.h"
#include "core/mailbox/mailbox_coordinator.h"
#include "core/mcp/mcp_config.h"
#include "core/message_types.h"
#include "core/models.h"
#include "core/session/session_store.h"

#include <map>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <vector>

namespace pi::core {

namespace mcp {
class McpClient;
}

class PiciProcess {
public:
  struct Config {
    std::map<std::string, ProviderConfig> providers;
    // Empty means "use SessionStore::default_sessions_dir()".
    std::string session_dir;
    // Optional explicit inference-adapter bindings. pi-core cannot link
    // pi-http's provider implementations (that would invert the library
    // dependency), so the executable that links both -- main()/acp::main()
    // -- builds this collection and registers the real providers into it.
    // Null falls back to ModelCatalog's own default collection, which wraps
    // the LLMClientRegistry singleton (kept for callers, mostly tests, that
    // construct a PiciProcess without wiring one explicitly).
    std::shared_ptr<InferenceAdapterCollection> inference_adapters;
    // Same rationale as inference_adapters above, for live model-list
    // discovery. Null falls back to ModelCatalog's own empty default (every
    // provider's discovery binding, if any, simply has no adapter to
    // resolve -- refresh() reports that per-provider rather than throwing).
    std::shared_ptr<ModelDiscoveryAdapterCollection> discovery_adapters;
    // Configured MCP servers. Each ENABLED entry is spawned and handshaken
    // (initialize/initialized) concurrently at construction time (a bounded
    // handful of seconds, not per-server serial addition -- see
    // pici_process.cpp); a server whose handshake fails or times out is
    // logged as a warning and simply contributes no tools, it does not
    // throw or abort construction. See mcp_tools().
    std::vector<mcp::McpServerConfig> mcp_servers;
  };

  // Throws std::runtime_error (propagated from ModelCatalog construction /
  // validate_registered_apis()) on bad provider configuration. Never throws
  // for MCP server misconfiguration/handshake failure -- those are caught,
  // logged, and simply absent from mcp_tools() (see Config::mcp_servers).
  explicit PiciProcess(const Config &config);

  const std::shared_ptr<const LuaWorkerAdapter> &lua_worker_adapter() const {
    return lua_worker_adapter_;
  }

  std::shared_ptr<const ModelCatalog> model_catalog() const {
    return model_catalog_;
  }
  const std::shared_ptr<auth::Authentication> &authentication() const {
    return authentication_;
  }
  const std::shared_ptr<SessionStore> &session_store() const {
    return session_store_;
  }

  // Forwards to the process-owned ModelCatalog's own refresh(), which is
  // already internally synchronized against concurrent view()/search()
  // readers -- this is a thin pass-through, not a new synchronization point.
  // model_catalog() keeps returning shared_ptr<const ModelCatalog> to every
  // other caller; this is the one entry point allowed to mutate it.
  std::vector<ProviderRefreshStatus>
  refresh_model_catalog(const std::vector<std::string> &provider_ids = {},
                        std::stop_token stop_token = {}) const {
    return model_catalog_->refresh(provider_ids, std::move(stop_token));
  }

  // Lazily constructs the process's one Mailbox on first call (using
  // `options`) and returns it; every later call returns that same instance
  // and ignores its `options` argument. Mailbox construction is deferred to
  // first use, rather than done eagerly alongside the catalog/authentication/
  // session store above, because callers (CLI session bootstrap) only know
  // MailboxOptions once model/args resolution -- which happens after
  // PiciProcess itself is constructed -- has completed. Returns the same
  // shared_ptr from every session that calls this, so all of a process's
  // sessions share one Mailbox and attach their own independent
  // MailboxAttachment to it (plans/object-taxonomy-migration.md Phase 8).
  std::shared_ptr<Mailbox> ensure_mailbox(const MailboxOptions &options) const;

  // One configured, enabled MCP server: the process-owned reusable client
  // (which owns the stdio or streamable-HTTP transport) and the tools last
  // listed from it. Archetype: Adapter state owned by PiciProcess; the
  // client is the sole live authority for the connection.
  struct McpServerEntry {
    std::string name;
    std::shared_ptr<mcp::McpClient> client;
    std::vector<std::shared_ptr<const ToolDefinition>> tools;
  };

  // Tools from every configured, enabled MCP server, merged and de-
  // duplicated (see pici_process.cpp). The first call after construction
  // returns the startup discovery. Before returning, any server whose
  // transport died, never finished its handshake, or announced
  // `notifications/tools/list_changed` is reconnected / re-listed (bounded,
  // throttled; failures are logged and keep that server's previous tools).
  // Sessions snapshot the result when they load their tools, so a refresh is
  // seen by sessions created afterwards, not by already-running ones.
  std::vector<std::shared_ptr<const ToolDefinition>> mcp_tools() const;

  // Forces the reconnect/re-list pass described above and returns the
  // refreshed tool set.
  std::vector<std::shared_ptr<const ToolDefinition>> refresh_mcp_tools() const;

private:
  std::shared_ptr<const LuaWorkerAdapter> lua_worker_adapter_{
      std::make_shared<LuaWorkerAdapter>()};
  std::shared_ptr<ModelCatalog> model_catalog_;
  auth::CredentialStore credential_store_;
  std::shared_ptr<auth::Authentication> authentication_;
  std::shared_ptr<SessionStore> session_store_;
  mutable std::mutex mailbox_mutex_;
  mutable std::shared_ptr<Mailbox> mailbox_;
  mutable std::mutex mcp_refresh_mutex_;
  mutable std::mutex mcp_mutex_;
  mutable std::vector<McpServerEntry> mcp_servers_;
  mutable std::vector<std::shared_ptr<const ToolDefinition>> mcp_tools_;
};

} // namespace pi::core
