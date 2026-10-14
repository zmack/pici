#pragma once

// McpClient: thin wrapper owning a persistent McpTransport (stdio child or
// streamable HTTP) plus the MCP initialize/initialized handshake, tool
// discovery (list_tools()), invocation (call_tool()), and reconnect() --
// which rebuilds the transport from its factory and re-handshakes when the
// current one has died.
//
// list_tools() returns pi::core::ToolDefinition (not the concrete McpTool
// type) deliberately: this header must not depend on core/mcp/mcp_tool.h,
// which itself depends on this header (McpTool holds a
// std::shared_ptr<McpClient>) -- returning the already-generic
// ToolDefinition base (which is also exactly what SessionRuntime::Config's
// tools vector element type is) avoids the cycle. mcp_client.cpp is the one
// translation unit that includes mcp_tool.h and does the actual
// construction.

#include "core/mcp/mcp_protocol.h"
#include "core/mcp/mcp_transport.h"
#include "core/message_types.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace pi::core::mcp {

// Built-in fallback for a tool call's timeout when the server's config
// doesn't set McpServerConfig::call_timeout_ms. Deliberately distinct from
// (and much larger than) kMcpHandshakeTimeout-style initialize() timeouts --
// a tool call can legitimately do real work (a slow filesystem walk, a
// network request a downstream MCP server makes on our behalf), whereas the
// initialize handshake is just a protocol formality that should be fast.
inline constexpr std::chrono::milliseconds kDefaultMcpCallTimeout{60000};

// Inherits enable_shared_from_this because list_tools() must hand each
// constructed McpTool a std::shared_ptr<McpClient> pointing at itself, so
// every tool from one server shares ownership of that server's one client/
// transport instead of each holding an independent copy. This means
// list_tools() may only be called on an McpClient that is itself already
// owned by a shared_ptr (shared_from_this() throws std::bad_weak_ptr
// otherwise) -- true for every production call site (PiciProcess
// constructs McpClient via std::make_shared) and for
// test_mcp_tool.cpp's fixtures.
class McpClient : public std::enable_shared_from_this<McpClient> {
public:
  using TransportFactory = std::function<std::unique_ptr<McpTransport>()>;

  // `server_name` is this server's configured name (McpServerConfig::name).
  // McpClient itself doesn't otherwise need it (it's a transport-level
  // concept, not a protocol one), but list_tools() needs it to qualify
  // ("<server>__<tool>") and log about the tools it discovers.
  // `call_timeout` is the per-call timeout McpTool::execute() should use for
  // this server's tools/call invocations (McpServerConfig::call_timeout_ms,
  // threaded through by PiciProcess) -- std::nullopt means "not configured,
  // use kDefaultMcpCallTimeout" (see effective_call_timeout()).
  McpClient(
      std::string server_name, McpServerProcessConfig process_config,
      std::optional<std::chrono::milliseconds> call_timeout = std::nullopt);

  // `factory` builds a fresh, un-handshaken transport; it is called once
  // here and again by every reconnect(). It may throw (e.g. spawn failure).
  McpClient(
      std::string server_name, TransportFactory factory,
      std::optional<std::chrono::milliseconds> call_timeout = std::nullopt);
  ~McpClient() = default;

  McpClient(const McpClient &) = delete;
  McpClient &operator=(const McpClient &) = delete;

  // Performs the initialize/initialized handshake against the server.
  // Throws std::runtime_error with a clear message on any failure:
  // malformed/incomplete response, timeout, or a transport that's already
  // dead. Intended to be called exactly once, before any other use of this
  // client.
  void initialize(
      std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

  // The current transport. A snapshot: reconnect() may replace it, and a
  // caller still holding the old one keeps it alive until it lets go.
  std::shared_ptr<McpTransport> transport() const;

  // True if the current transport still appears usable (child running /
  // HTTP session not expired). McpTool::execute() checks this before a call
  // and calls reconnect() when it is false.
  bool is_alive() const { return transport()->is_alive(); }
  bool is_initialized() const { return initialized_.load(); }
  const std::string &server_name() const { return server_name_; }

  // The timeout McpTool::execute() should use for a tools/call against this
  // server: the configured McpServerConfig::call_timeout_ms if one was
  // given at construction, else kDefaultMcpCallTimeout.
  std::chrono::milliseconds effective_call_timeout() const {
    return call_timeout_.value_or(kDefaultMcpCallTimeout);
  }

  // Sends tools/list, validates each advertised inputSchema is itself a
  // loadable JSON Schema (skipping -- with a logged warning -- any tool
  // whose schema fails to load, so a malformed schema can never reach
  // agent_loop.cpp's unguarded tool->schema().validate_arguments() call),
  // sanitizes/qualifies each surviving tool's name (dropping any that
  // collide with a builtin tool or a sibling MCP tool from this same
  // server, logged the same way), and wraps each survivor in an McpTool.
  // Throws TransportError/std::runtime_error only for a transport-level
  // failure (dead transport, timeout, malformed top-level response) -- a
  // single bad tool entry is skipped, not fatal to the whole call.
  std::vector<std::shared_ptr<const ToolDefinition>> list_tools(
      std::chrono::milliseconds timeout = std::chrono::milliseconds(10000));

  // Sends tools/call {"name": tool_name, "arguments": arguments} and
  // returns the raw JSON "result" object verbatim -- response-shape
  // interpretation (isError, content blocks) is McpTool::execute()'s job,
  // not this layer's. `stop_tok`, when stop-capable, races the wait against
  // cancellation (see McpStdioTransport::call()'s stop_token overload) --
  // McpTool::execute() passes its own `stop_tok` through here so a stopped
  // tool call returns promptly instead of waiting out `timeout`.
  nlohmann::json
  call_tool(const std::string &tool_name, nlohmann::json arguments,
            std::chrono::milliseconds timeout = kDefaultMcpCallTimeout,
            std::stop_token stop_tok = std::stop_token{});

  // Replaces a dead transport: builds a new one from the factory,
  // handshakes it, installs it, and marks the tool list stale (the server may
  // have restarted with a different tool set). Returns true if the client is
  // usable afterwards (also true, with no work done, when the transport is
  // still alive and initialized). Returns false and fills `error` (never
  // containing credentials) on failure. Concurrent callers serialize; a
  // failed attempt is not retried for kReconnectBackoff, so a hard-down
  // server is not hammered by every tool call.
  bool reconnect(
      std::string *error = nullptr,
      std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

  // True when the server sent notifications/tools/list_changed, or the
  // client reconnected, since the last list_tools(). PiciProcess uses it to
  // decide which servers to re-list.
  bool tools_stale() const { return tools_stale_->load(); }

  static constexpr std::chrono::milliseconds kReconnectBackoff{1000};

  // Test hook: shortens kReconnectBackoff-style throttling to zero.
  void set_reconnect_backoff_for_testing(std::chrono::milliseconds backoff) {
    backoff_ = backoff;
  }

private:
  void handshake(McpTransport &transport, std::chrono::milliseconds timeout);
  void install_notification_hook(McpTransport &transport);

  std::string server_name_;
  TransportFactory factory_;
  mutable std::mutex transport_mutex_;
  std::shared_ptr<McpTransport> transport_;
  std::mutex reconnect_mutex_;
  std::chrono::steady_clock::time_point last_reconnect_failure_{};
  std::chrono::milliseconds backoff_{kReconnectBackoff};
  std::string last_reconnect_error_;
  std::atomic<bool> initialized_{false};
  // Shared with the transport's notification callback, which may outlive a
  // reconnect's old transport but never needs the client itself.
  std::shared_ptr<std::atomic<bool>> tools_stale_{
      std::make_shared<std::atomic<bool>>(false)};
  std::optional<std::chrono::milliseconds> call_timeout_;
};

} // namespace pi::core::mcp
