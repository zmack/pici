#include "core/mcp/mcp_client.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

#ifndef FAKE_MCP_SERVER_BINARY
#error "FAKE_MCP_SERVER_BINARY must be defined by CMakeLists.txt"
#endif

namespace {

using pi::core::mcp::McpClient;
using pi::core::mcp::McpServerProcessConfig;

McpServerProcessConfig fake_server_config(const std::string &mode) {
  McpServerProcessConfig config;
  config.command = FAKE_MCP_SERVER_BINARY;
  config.env["FAKE_MCP_SERVER_MODE"] = mode;
  return config;
}

} // namespace

TEST(McpClient, InitializeSucceedsAgainstFakeServer) {
  McpClient client("test-server", fake_server_config("normal"));
  EXPECT_NO_THROW(client.initialize(std::chrono::milliseconds(2000)));
  EXPECT_TRUE(client.is_initialized());
  EXPECT_TRUE(client.is_alive());
}

TEST(McpClient, InitializeThrowsOnMalformedHandshakeResponse) {
  McpClient client("test-server", fake_server_config("malformed_init"));
  EXPECT_THROW(client.initialize(std::chrono::milliseconds(2000)),
               std::runtime_error);
  EXPECT_FALSE(client.is_initialized());
}

TEST(McpClient, InitializeTimesOutIfServerNeverResponds) {
  McpClient client("test-server", fake_server_config("silent_init"));
  EXPECT_THROW(client.initialize(std::chrono::milliseconds(300)),
               std::runtime_error);
  EXPECT_FALSE(client.is_initialized());
}
