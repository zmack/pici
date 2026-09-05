#pragma once

// McpClient: thin wrapper owning a persistent McpStdioTransport plus the
// MCP initialize/initialized handshake, plus (Phase 2) tool discovery
// (list_tools()) and invocation (call_tool()).
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

#include <chrono>
#include <memory>
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
      std::optional<std::chrono::milliseconds> call_timeout = std::nullopt)
      : server_name_(std::move(server_name)),
        transport_(std::move(process_config)), call_timeout_(call_timeout) {}
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

  McpStdioTransport &transport() { return transport_; }
  const McpStdioTransport &transport() const { return transport_; }

  // True if the underlying transport's child process still appears to be
  // running. McpTool::execute() checks this before issuing a call (belt-
  // and-suspenders on top of the transport's own EPIPE/reader-death
  // handling: this just avoids attempting a call at all, and gives a
  // clearer error message, once a server is already known dead).
  //
  // v1 deliberately does NOT reconnect a dead server, and does NOT react to
  // a live server's `notifications/tools/list_changed` -- both are the same
  // underlying question ("how should pici react to a server's tool set
  // changing after startup: a crash-then-restart, or a live add/remove") and
  // are out of scope together, not by oversight. Once a server is dead, its
  // tools stay registered (so they remain visible/nameable) but every call
  // to them fails cleanly via is_alive() below; there is no path back to
  // "alive" for an McpClient short of restarting the whole pici process.
  bool is_alive() const { return transport_.is_alive(); }
  bool is_initialized() const { return initialized_; }
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

private:
  std::string server_name_;
  McpStdioTransport transport_;
  bool initialized_{false};
  std::optional<std::chrono::milliseconds> call_timeout_;
};

} // namespace pi::core::mcp
