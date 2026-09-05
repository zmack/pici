#include "core/process/pici_process.h"

#include "core/auth/authentication_adapter.h"
#include "core/llm_client.h"
#include "core/mailbox/mailbox_coordinator.h"
#include "core/mailbox/mailbox_types.h"
#include "core/mcp/mcp_config.h"
#include "core/models.h"
#include "core/session/session_record.h"
#include "support/gtest_helpers.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#ifndef FAKE_MCP_SERVER_BINARY
#error "FAKE_MCP_SERVER_BINARY must be defined by CMakeLists.txt"
#endif

namespace {

void register_builtin_clients() {
  for (const auto &provider : pi::core::ModelCatalog::builtin_providers()) {
    pi::core::LLMClientRegistry::instance().register_client(
        provider.api, [] { return std::shared_ptr<pi::core::LLMClient>{}; });
  }
  pi::core::LLMClientRegistry::instance().register_client(
      "test-api", [] { return std::shared_ptr<pi::core::LLMClient>{}; });
}

pi::core::ProviderConfig make_provider(const std::string &id) {
  pi::core::ProviderConfig provider;
  provider.id = id;
  provider.api = "test-api";
  provider.base_url = "http://" + id + ".test/v1";
  provider.auth = pi::core::ProviderAuthPolicy::none;
  return provider;
}

pi::core::PiciProcess::Config two_provider_config(std::string session_dir = {}) {
  register_builtin_clients();
  pi::core::PiciProcess::Config config;
  config.providers = {
      {"alpha", make_provider("alpha")},
      {"beta", make_provider("beta")},
  };
  config.session_dir = std::move(session_dir);
  return config;
}

pi::core::mcp::McpServerConfig
fake_mcp_server(const std::string &name, int init_delay_ms = 0) {
  pi::core::mcp::McpServerConfig server;
  server.name = name;
  server.command = FAKE_MCP_SERVER_BINARY;
  server.env["FAKE_MCP_SERVER_MODE"] = "normal";
  if (init_delay_ms > 0)
    server.env["FAKE_MCP_SERVER_INIT_DELAY_MS"] = std::to_string(init_delay_ms);
  return server;
}

} // namespace

TEST(PiciProcess, ConstructsAccessorsNonNullAndInternallyConsistent) {
  pi::core::PiciProcess process(two_provider_config());

  ASSERT_NE(process.model_catalog(), nullptr);
  ASSERT_NE(process.authentication(), nullptr);
  ASSERT_NE(process.session_store(), nullptr);

  EXPECT_NE(process.model_catalog()->provider("alpha"), nullptr);
  EXPECT_NE(process.model_catalog()->provider("beta"), nullptr);
  EXPECT_EQ(process.authentication()->availability("alpha"),
            pi::auth::AuthAvailability::not_required);
  EXPECT_EQ(process.authentication()->availability("beta"),
            pi::auth::AuthAvailability::not_required);
}

TEST(PiciProcess, AccessorsAreStableAcrossRepeatedCalls) {
  pi::core::PiciProcess process(two_provider_config());

  EXPECT_EQ(process.model_catalog(), process.model_catalog());
  EXPECT_EQ(process.authentication(), process.authentication());
  EXPECT_EQ(process.session_store(), process.session_store());
}

TEST(PiciProcess, EnsureMailboxIsLazyAndSharedAcrossSessions) {
  pi::test::TemporaryDirectory directory{"pici-process-mailbox"};
  std::filesystem::permissions(directory.path(),
                               std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace);
  pi::core::PiciProcess process(two_provider_config());

  pi::core::MailboxOptions options;
  options.store.path = directory.path() / "mailbox.sqlite3";
  options.store.workspace_id = "workspace";
  options.store.workspace_path = directory.path().string();
  options.store.clock = [] { return pi::core::TimestampMs{1'000}; };
  options.store.id_generator = [] { return std::string("generated"); };
  options.process_id = "process-under-test";
  options.provider = "test";
  options.model_id = "test-model";
  options.heartbeat_interval = std::chrono::hours(1);
  options.stale_after = std::chrono::hours(2);

  const auto first = process.ensure_mailbox(options);
  const auto second = process.ensure_mailbox(options);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first, second);

  // Two sessions attaching to the process's one Mailbox get independent
  // attachments: activating one's root never touches the other's.
  const auto key_a = first->attach();
  const auto key_b = first->attach();
  first->activate_root(key_a, "session-a");
  EXPECT_TRUE(first->status(key_a).root_active);
  EXPECT_TRUE(!first->status(key_b).root_active);
}

TEST(PiciProcess, InvalidProviderThrowsAndLeavesNoUsableObject) {
  register_builtin_clients();
  pi::core::PiciProcess::Config config;
  pi::core::ProviderConfig invalid;
  invalid.id = "broken";
  invalid.api = "not-a-registered-api";
  invalid.base_url = "http://broken.test/v1";
  invalid.auth = pi::core::ProviderAuthPolicy::none;
  config.providers = {{"broken", invalid}};

  EXPECT_THROW({ pi::core::PiciProcess process(config); }, std::runtime_error);
}

TEST(PiciProcess, SessionStoreReflectsConfiguredSessionDirectory) {
  pi::test::TemporaryDirectory directory{"pici-process"};
  pi::core::PiciProcess process(
      two_provider_config(directory.path().string()));

  pi::core::SessionHeader header;
  header.id = "pici-process-test-session";
  header.model = "test-model";
  header.provider = "alpha";
  const auto session_id = process.session_store()->create(header);

  const auto expected_path = directory.path() / (session_id + ".jsonl");
  EXPECT_TRUE(std::filesystem::exists(expected_path));
  EXPECT_EQ(process.session_store()->session_path(session_id), expected_path);

  const auto loaded = process.session_store()->load(session_id);
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(loaded->header.provider, // NOLINT(bugprone-unchecked-optional-access)
            "alpha");
}

TEST(PiciProcess, McpToolsPopulatedFromConfiguredServers) {
  auto config = two_provider_config();
  config.mcp_servers = {fake_mcp_server("fs")};

  pi::core::PiciProcess process(config);
  const auto tools = process.mcp_tools();

  // The fake server always advertises "test_tool" (valid) and
  // "broken_schema_tool" (a deliberately malformed inputSchema, skipped at
  // discovery) -- see fake_mcp_server.cpp's file comment.
  ASSERT_EQ(tools.size(), 1u);
  EXPECT_EQ(tools.front()->name(), "fs__test_tool");
}

TEST(PiciProcess, McpToolNamesAreQualifiedByServerName) {
  auto config = two_provider_config();
  config.mcp_servers = {fake_mcp_server("server-one"),
                        fake_mcp_server("server-two")};

  pi::core::PiciProcess process(config);
  const auto tools = process.mcp_tools();

  ASSERT_EQ(tools.size(), 2u);
  bool has_one = false;
  bool has_two = false;
  for (const auto &tool : tools) {
    if (tool->name() == "server-one__test_tool")
      has_one = true;
    if (tool->name() == "server-two__test_tool")
      has_two = true;
  }
  EXPECT_TRUE(has_one);
  EXPECT_TRUE(has_two);
}

TEST(PiciProcess, McpServerHandshakeFailureLogsAndSkipsWithoutThrowing) {
  auto config = two_provider_config();
  pi::core::mcp::McpServerConfig broken;
  broken.name = "nonexistent";
  broken.command = "/nonexistent/path/to/an/mcp/server/binary";

  // A GOOD server alongside the broken one, to prove the failure is
  // isolated to the misconfigured server rather than aborting discovery
  // for everyone.
  config.mcp_servers = {broken, fake_mcp_server("good")};

  pi::core::PiciProcess process(config); // must not throw
  const auto tools = process.mcp_tools();
  ASSERT_EQ(tools.size(), 1u);
  EXPECT_EQ(tools.front()->name(), "good__test_tool");
}

TEST(PiciProcess, McpServerDisabledIsNotSpawned) {
  auto config = two_provider_config();
  auto disabled = fake_mcp_server("off");
  disabled.enabled = false;
  config.mcp_servers = {disabled};

  pi::core::PiciProcess process(config);
  EXPECT_TRUE(process.mcp_tools().empty());
}

TEST(PiciProcess, McpToolsBecomeUnavailableAfterServerCrashWithoutAffectingOtherServers) {
  auto config = two_provider_config();
  config.mcp_servers = {fake_mcp_server("crashy"), fake_mcp_server("good")};

  pi::core::PiciProcess process(config);
  const auto tools = process.mcp_tools();
  ASSERT_EQ(tools.size(), 2u);

  std::shared_ptr<const pi::core::ToolDefinition> crashy_tool;
  std::shared_ptr<const pi::core::ToolDefinition> good_tool;
  for (const auto &tool : tools) {
    if (tool->name() == "crashy__test_tool")
      crashy_tool = tool;
    if (tool->name() == "good__test_tool")
      good_tool = tool;
  }
  ASSERT_NE(crashy_tool, nullptr);
  ASSERT_NE(good_tool, nullptr);

  // Crash "crashy"'s server through an ordinary tools/call (no direct
  // transport access) -- exercising exactly the path a real server crash
  // mid-call would take.
  auto crash_result = crashy_tool->execute("call-crash", R"({"crash":true})");
  ASSERT_NE(crash_result, nullptr);
  EXPECT_TRUE(crash_result->is_error());

  // "good"'s tools must be completely unaffected.
  auto good_result =
      good_tool->execute("call-good", R"({"text":"still-fine"})");
  ASSERT_NE(good_result, nullptr);
  EXPECT_FALSE(good_result->is_error());
  EXPECT_EQ(good_result->content(), "still-fine");

  // Subsequent calls to the crashed server's tool must return a CLEAN
  // error immediately (via McpTool::execute()'s is_alive() pre-check), not
  // hang -- and must NOT succeed, which would only happen if something had
  // reconnected/respawned the server. v1 deliberately never does (see
  // PiciProcess::mcp_tools()'s doc comment) -- repeating the call proves
  // it, since a respawn (if one existed) would eventually let a later call
  // through.
  for (int attempt = 0; attempt < 3; ++attempt) {
    auto result = crashy_tool->execute("call-crash-retry", R"({"text":"x"})");
    ASSERT_NE(result, nullptr);
    EXPECT_TRUE(result->is_error());
    EXPECT_NE(result->content().find("no longer running"), std::string::npos)
        << "attempt " << attempt << ": " << result->content();
  }

  // "good" keeps working the whole time -- one crashed server never
  // affects any other configured server.
  auto good_result_again =
      good_tool->execute("call-good-2", R"({"text":"still-fine-2"})");
  ASSERT_NE(good_result_again, nullptr);
  EXPECT_FALSE(good_result_again->is_error());
  EXPECT_EQ(good_result_again->content(), "still-fine-2");
}

TEST(PiciProcess, McpServerHandshakesRunConcurrentlyNotSerially) {
  auto config = two_provider_config();
  // Two servers, each deliberately slow to reply to "initialize". If
  // handshakes ran serially, total construction time would be roughly the
  // SUM of both delays (>= 800ms); run concurrently, it should be close to
  // a SINGLE delay (~400ms) plus overhead. The assertion below uses a
  // generous threshold to avoid CI flakiness while still being meaningful:
  // strictly less than the sum of both delays.
  constexpr int kDelayMs = 400;
  config.mcp_servers = {fake_mcp_server("slow-one", kDelayMs),
                        fake_mcp_server("slow-two", kDelayMs)};

  const auto start = std::chrono::steady_clock::now();
  pi::core::PiciProcess process(config);
  const auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_EQ(process.mcp_tools().size(), 2u);
  EXPECT_LT(elapsed, std::chrono::milliseconds(kDelayMs * 2))
      << "construction took "
      << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
      << "ms, expected well under the "
      << (kDelayMs * 2)
      << "ms a fully-serial (sum-of-delays) handshake would take";
}
