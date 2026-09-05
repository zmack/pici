#include "core/mcp/mcp_client.h"
#include "core/mcp/mcp_tool.h"
#include "core/message_types.h"
#include "support/gtest_helpers.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#ifndef FAKE_MCP_SERVER_BINARY
#error "FAKE_MCP_SERVER_BINARY must be defined by CMakeLists.txt"
#endif

namespace {

using pi::core::ToolDefinition;
using pi::core::ToolExecutionMode;
using pi::core::mcp::McpClient;
using pi::core::mcp::McpServerProcessConfig;
using pi::core::mcp::sanitize_mcp_tool_name;

McpServerProcessConfig fake_server_config(const std::string &mode = "normal") {
  McpServerProcessConfig config;
  config.command = FAKE_MCP_SERVER_BINARY;
  config.env["FAKE_MCP_SERVER_MODE"] = mode;
  return config;
}

// Fake server always advertises exactly two tools (see
// fake_mcp_server.cpp's file comment): "test_tool" (a valid, flexible
// echo/control tool) and "broken_schema_tool" (a deliberately malformed
// inputSchema). Discovery must skip the latter, so list_tools() should
// always return exactly one McpTool: "<server>__test_tool".
std::shared_ptr<McpClient>
make_initialized_client(const std::string &server_name = "test-server",
                        const std::string &mode = "normal") {
  auto client =
      std::make_shared<McpClient>(server_name, fake_server_config(mode));
  client->initialize(std::chrono::milliseconds(2000));
  return client;
}

std::shared_ptr<const ToolDefinition>
discover_test_tool(const std::string &server_name = "test-server") {
  auto client = make_initialized_client(server_name);
  auto tools = client->list_tools(std::chrono::milliseconds(2000));
  for (auto &tool : tools) {
    if (tool->name().find("__test_tool") != std::string_view::npos)
      return tool;
  }
  return nullptr;
}

} // namespace

TEST(McpTool, ListToolsReturnsToolsAdvertisedByFakeServer) {
  auto client = make_initialized_client("test-server");
  auto tools = client->list_tools(std::chrono::milliseconds(2000));

  // "broken_schema_tool" is skipped at discovery (malformed inputSchema),
  // so only "test_tool" should survive.
  ASSERT_EQ(tools.size(), 1u);
  EXPECT_EQ(tools.front()->name(), "test-server__test_tool");
  EXPECT_FALSE(tools.front()->description().empty());
}

TEST(McpTool, MalformedInputSchemaIsSkippedAtDiscoveryNotAtCallTime) {
  auto client = make_initialized_client("test-server");
  auto tools = client->list_tools(std::chrono::milliseconds(2000));
  for (const auto &tool : tools) {
    EXPECT_EQ(tool->name().find("broken_schema_tool"), std::string_view::npos)
        << "a tool with a malformed inputSchema must never reach the "
           "constructed tool list";
  }
}

TEST(McpTool, QualifiedNameIsServerDoubleUnderscoreToolName) {
  auto tool = discover_test_tool("srv");
  ASSERT_NE(tool, nullptr);
  EXPECT_EQ(tool->name(), "srv__test_tool");
}

TEST(McpTool, SourcePathReportsMcpColonServerName) {
  auto tool = discover_test_tool("srv2");
  ASSERT_NE(tool, nullptr);
  EXPECT_EQ(tool->source_path(), "mcp:srv2");
}

TEST(McpTool, CapabilitiesDefaultToNotChildSafe) {
  auto tool = discover_test_tool();
  ASSERT_NE(tool, nullptr);
  EXPECT_FALSE(tool->capabilities().child_safe);
}

TEST(McpTool, ExecutionModeDefaultsToParallel) {
  auto tool = discover_test_tool();
  ASSERT_NE(tool, nullptr);
  // NOT sequential: an earlier draft overrode this, but that flag is a
  // batch-wide de-parallelizer (agent_loop.cpp's is_sequential_tool is
  // checked with any_of across the WHOLE tool-call batch) that doesn't even
  // address the real concurrency risk. See ConcurrentCallsFromMultipleThreadsAreSafe
  // below for the actual regression proof.
  EXPECT_EQ(tool->execution_mode(), ToolExecutionMode::parallel);
}

TEST(McpTool, ExecuteMapsSuccessfulToolsCallToNonErrorResult) {
  auto tool = discover_test_tool();
  ASSERT_NE(tool, nullptr);
  auto result = tool->execute("call-1", R"({"text":"hello"})");
  ASSERT_NE(result, nullptr);
  EXPECT_FALSE(result->is_error());
  EXPECT_EQ(result->content(), "hello");
}

TEST(McpTool, ExecuteMapsIsErrorTrueToErrorToolResult) {
  auto tool = discover_test_tool();
  ASSERT_NE(tool, nullptr);
  auto result =
      tool->execute("call-2", R"({"fail":true,"fail_message":"boom"})");
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->is_error());
  EXPECT_EQ(result->content(), "boom");
}

TEST(McpTool, ExecuteMapsMultipleTextContentBlocksJoined) {
  auto tool = discover_test_tool();
  ASSERT_NE(tool, nullptr);
  auto result = tool->execute("call-3", R"({"blocks":["a","b","c"]})");
  ASSERT_NE(result, nullptr);
  EXPECT_FALSE(result->is_error());
  EXPECT_EQ(result->content(), "a\nb\nc");
}

TEST(McpTool, DropsNonTextContentBlocksInsteadOfMappingThem) {
  auto tool = discover_test_tool();
  ASSERT_NE(tool, nullptr);
  auto result =
      tool->execute("call-4", R"({"text":"hi","include_image_block":true})");
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result->content(), "hi");
}

TEST(McpTool, TruncatesOversizedToolResultContent) {
  auto tool = discover_test_tool();
  ASSERT_NE(tool, nullptr);

  const std::string huge(200 * 1024, 'a');
  const nlohmann::json args{{"text", huge}};
  auto result = tool->execute("call-5", args.dump());
  ASSERT_NE(result, nullptr);
  EXPECT_LT(result->content().size(), huge.size());
  EXPECT_NE(result->content().find("Truncated"), std::string::npos);
}

TEST(McpTool, ConcurrentCallsFromMultipleThreadsAreSafe) {
  auto tool = discover_test_tool();
  ASSERT_NE(tool, nullptr);

  constexpr int kThreadCount = 8;
  std::vector<std::thread> threads;
  std::atomic<int> success_count{0};
  threads.reserve(kThreadCount);
  for (int i = 0; i < kThreadCount; ++i) {
    threads.emplace_back([&, i] {
      const std::string expected = "thread-" + std::to_string(i);
      const nlohmann::json args{{"text", expected}};
      auto result = tool->execute("call-concurrent", args.dump());
      if (result && !result->is_error() && result->content() == expected)
        ++success_count;
    });
  }
  for (auto &t : threads)
    t.join();

  EXPECT_EQ(success_count.load(), kThreadCount);
}

TEST(McpTool, SanitizesToolNameToProviderSafeCharacterSet) {
  const auto name = sanitize_mcp_tool_name("my server!", "weird tool name?");
  for (char c : name) {
    EXPECT_TRUE((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '-')
        << "unsafe character in sanitized name: " << name;
  }
  EXPECT_LE(name.size(), 64u);
}

TEST(McpTool, ExecuteReturnsErrorResultImmediatelyWhenTransportAlreadyDead) {
  auto client = make_initialized_client("dead-server");
  auto tools = client->list_tools(std::chrono::milliseconds(2000));
  std::shared_ptr<const ToolDefinition> tool;
  for (auto &candidate : tools) {
    if (candidate->name().find("__test_tool") != std::string_view::npos)
      tool = candidate;
  }
  ASSERT_NE(tool, nullptr);

  // Kill the server WITHOUT going through the tool interface (no in-flight
  // call), then confirm execute() notices via is_alive() and refuses to
  // even attempt a call.
  client->transport().notify("test/crash", nlohmann::json::object());
  ASSERT_TRUE(pi::test::wait_until([&] { return !client->is_alive(); },
                                   std::chrono::milliseconds(2000),
                                   "server exiting after test/crash"));

  const auto start = std::chrono::steady_clock::now();
  auto result = tool->execute("call-dead", R"({"text":"hello"})");
  const auto elapsed = std::chrono::steady_clock::now() - start;

  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->is_error());
  EXPECT_NE(result->content().find("no longer running"), std::string::npos)
      << "actual content: " << result->content();
  // No call was attempted at all -- this should return near-instantly, not
  // pay for a write/wait cycle against a dead transport.
  EXPECT_LT(elapsed, std::chrono::milliseconds(50));
}

TEST(McpTool, ExecuteResultDetailsIncludeCapturedStderrOnServerCrash) {
  auto tool = discover_test_tool("crash-server");
  ASSERT_NE(tool, nullptr);

  const nlohmann::json args{
      {"crash", true},
      {"stderr_message", "fatal: simulated crash for test\n"},
  };
  auto result = tool->execute("call-crash", args.dump());

  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->is_error());
  const auto details = result->details();
  ASSERT_TRUE(details.has_value());
  EXPECT_NE(details->find("simulated crash for test"), std::string::npos)
      << "actual details: " << *details;
}

TEST(McpTool, SanitizeTruncatesOverlongNamesWithDistinctSuffixes) {
  const std::string long_tool_name(100, 'x');
  const auto name1 = sanitize_mcp_tool_name("server", long_tool_name + "1");
  const auto name2 = sanitize_mcp_tool_name("server", long_tool_name + "2");
  EXPECT_LE(name1.size(), 64u);
  EXPECT_LE(name2.size(), 64u);
  EXPECT_NE(name1, name2)
      << "two names truncating to the same prefix must stay distinct";
}
