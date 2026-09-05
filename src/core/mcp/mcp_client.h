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

#include "core/message_types.h"
#include "core/mcp/mcp_protocol.h"
#include "core/mcp/mcp_transport.h"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace pi::core::mcp {

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
  McpClient(std::string server_name, McpServerProcessConfig process_config)
      : server_name_(std::move(server_name)),
        transport_(std::move(process_config)) {}
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

  bool is_alive() const { return transport_.is_alive(); }
  bool is_initialized() const { return initialized_; }
  const std::string &server_name() const { return server_name_; }

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
  std::vector<std::shared_ptr<const ToolDefinition>>
  list_tools(std::chrono::milliseconds timeout = std::chrono::milliseconds(
                 10000));

  // Sends tools/call {"name": tool_name, "arguments": arguments} and
  // returns the raw JSON "result" object verbatim -- response-shape
  // interpretation (isError, content blocks) is McpTool::execute()'s job,
  // not this layer's.
  nlohmann::json
  call_tool(const std::string &tool_name, nlohmann::json arguments,
           std::chrono::milliseconds timeout = std::chrono::milliseconds(
               30000));

private:
  std::string server_name_;
  McpStdioTransport transport_;
  bool initialized_{false};
};

} // namespace pi::core::mcp
