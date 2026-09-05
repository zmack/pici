#include "core/mcp/mcp_client.h"

#include <stdexcept>

namespace pi::core::mcp {

void McpClient::initialize(std::chrono::milliseconds timeout) {
  const nlohmann::json params{
      {"protocolVersion", "2024-11-05"},
      // Phase 1 declares no sampling/roots/elicitation capability. This
      // makes a server less likely to send those request types, but does
      // NOT make it impossible (a non-compliant or eager server may send
      // one anyway) -- mcp_transport.cpp's reader loop replies to any
      // inbound server-initiated request with a stub "method not found"
      // regardless of what we declare here.
      {"capabilities", nlohmann::json::object()},
      {"clientInfo", {{"name", "pici"}, {"version", PI_CPP_VERSION}}},
  };

  nlohmann::json response;
  try {
    response =
        transport_.call(std::string(methods::kInitialize), params, timeout);
  } catch (const std::exception &err) {
    throw std::runtime_error(std::string("mcp initialize failed: ") +
                             err.what());
  }

  if (!response.is_object() || !response.contains("protocolVersion") ||
      !response.contains("capabilities")) {
    throw std::runtime_error(
        "mcp initialize failed: malformed handshake response (missing "
        "protocolVersion/capabilities)");
  }

  // Notification: no reply expected. If the transport is already dead this
  // throws TransportError, which is a std::runtime_error and propagates as
  // the same "initialize failed" family of error.
  transport_.notify(std::string(methods::kInitialized),
                    nlohmann::json::object());
  initialized_ = true;
}

} // namespace pi::core::mcp
