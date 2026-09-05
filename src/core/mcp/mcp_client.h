#pragma once

// McpClient: thin wrapper owning a persistent McpStdioTransport plus the
// MCP initialize/initialized handshake. Phase 1 scope only -- list_tools()/
// call_tool() are Phase 2's job (tool discovery/adaptation into
// pi::core::ToolDefinition); their signatures are declared below, commented
// out, purely as a forward-compatibility note for whoever implements Phase
// 2, since implementing them now would require types (McpTool,
// ToolDefinition wiring) that don't exist yet in this phase.

#include "core/mcp/mcp_protocol.h"
#include "core/mcp/mcp_transport.h"

#include <chrono>
#include <string>

namespace pi::core::mcp {

class McpClient {
public:
  explicit McpClient(McpServerProcessConfig process_config)
      : transport_(std::move(process_config)) {}
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

  // Phase 2:
  // nlohmann::json list_tools(std::chrono::milliseconds timeout);
  // std::shared_ptr<ToolResult> call_tool(const std::string &tool_name,
  //                                       nlohmann::json arguments,
  //                                       std::chrono::milliseconds timeout);

private:
  McpStdioTransport transport_;
  bool initialized_{false};
};

} // namespace pi::core::mcp
