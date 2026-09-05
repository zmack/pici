#include "core/mcp/mcp_transport.h"

#include "support/gtest_helpers.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <future>
#include <mutex>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <vector>

#ifndef FAKE_MCP_SERVER_BINARY
#error "FAKE_MCP_SERVER_BINARY must be defined by CMakeLists.txt"
#endif

namespace {

using pi::core::mcp::McpServerProcessConfig;
using pi::core::mcp::McpStdioTransport;
using pi::core::mcp::TransportError;

// This test binary has its own (GTest-provided) main() and never links or
// runs src/main.cpp/src/acp/main.cpp, so it does not automatically inherit
// their `std::signal(SIGPIPE, SIG_IGN)` installation. Mirror it here, at
// static-init time (before any TEST body runs), so
// WritingToACrashedServerReturnsErrorNotSigpipeDeath below exercises the
// same documented precondition the real pici binaries establish -- without
// it, that test would itself die to the default SIGPIPE disposition on the
// very write it means to prove is now safe.
struct SigpipeIgnoredForTests {
  SigpipeIgnoredForTests() { std::signal(SIGPIPE, SIG_IGN); }
} const g_sigpipe_ignored_for_tests;

McpServerProcessConfig fake_server_config(const std::string &mode = "normal") {
  McpServerProcessConfig config;
  config.command = FAKE_MCP_SERVER_BINARY;
  if (!mode.empty())
    config.env["FAKE_MCP_SERVER_MODE"] = mode;
  return config;
}

constexpr auto kShortTimeout = std::chrono::milliseconds(2000);

} // namespace

TEST(McpTransport, SpawnsChildAndExchangesOneRoundTrip) {
  McpStdioTransport transport(fake_server_config());
  const auto result =
      transport.call("test/echo", {{"hello", "world"}}, kShortTimeout);
  EXPECT_EQ(result.value("hello", std::string{}), "world");
  EXPECT_TRUE(transport.is_alive());
}

TEST(McpTransport, HandlesOutOfOrderResponses) {
  McpStdioTransport transport(fake_server_config());

  // Issue a slow call first, then a fast one -- the fast one's reply
  // arrives first on the wire, proving the pending-map keys responses by
  // id rather than assuming reply order matches send order.
  auto slow = std::async(std::launch::async, [&] {
    return transport.call("test/echo", {{"tag", "slow"}, {"delay_ms", 200}},
                          kShortTimeout);
  });
  // Give the slow request time to be sent and start its server-side sleep
  // before sending the fast one.
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  const auto fast =
      transport.call("test/echo", {{"tag", "fast"}}, kShortTimeout);

  EXPECT_EQ(fast.value("tag", std::string{}), "fast");
  const auto slow_result = slow.get();
  EXPECT_EQ(slow_result.value("tag", std::string{}), "slow");
}

TEST(McpTransport, HandlesServerInitiatedRequestWithMethodAndNotFoundReply) {
  McpStdioTransport transport(fake_server_config());

  std::mutex mutex;
  bool got_confirmation = false;
  bool got_not_found = false;
  transport.set_notification_callback(
      [&](const std::string &method, const nlohmann::json &params) {
        if (method != "test/serverRequestNotFoundConfirmed")
          return;
        std::lock_guard<std::mutex> lock(mutex);
        got_confirmation = true;
        got_not_found = params.value("gotNotFound", false);
      });

  // The id our own NEXT real request will use -- deliberately handed to the
  // fake server so it sends a server-initiated request colliding with it.
  const auto colliding_id = transport.peek_next_request_id_for_testing();

  auto real_call = std::async(std::launch::async, [&] {
    return transport.call("test/echo", {{"tag", "real"}, {"delay_ms", 150}},
                          kShortTimeout);
  });

  const auto trigger_result = transport.call(
      "test/trigger_server_request",
      {{"id", colliding_id}, {"method", "roots/list"}}, kShortTimeout);
  EXPECT_TRUE(trigger_result.value("ok", false));

  // The real call must still resolve to its OWN response, not be
  // cross-fulfilled by the -32601 reply meant for the server's request.
  const auto real_result = real_call.get();
  EXPECT_EQ(real_result.value("tag", std::string{}), "real");

  ASSERT_TRUE(pi::test::wait_until(
      [&] {
        std::lock_guard<std::mutex> lock(mutex);
        return got_confirmation;
      },
      kShortTimeout, "server request not-found confirmation"));
  {
    std::lock_guard<std::mutex> lock(mutex);
    EXPECT_TRUE(got_not_found)
        << "server's own request should have gotten a -32601 reply, not a "
           "cross-wired response meant for the real client call";
  }
}

TEST(McpTransport, ConcurrentTimeoutAndLateResponseDoesNotCrash) {
  McpStdioTransport transport(fake_server_config());

  // The server replies well after our timeout: forces the exact race the
  // pending-map extract-under-lock design is meant to make safe, whichever
  // side (our timeout, or the reader thread matching the late response)
  // wins.
  EXPECT_THROW(transport.call("test/echo", {{"delay_ms", 300}},
                              std::chrono::milliseconds(50)),
               TransportError);

  // The transport must still be usable afterward -- neither a crash nor a
  // wedged pending-map entry from the race above.
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  const auto result =
      transport.call("test/echo", {{"tag", "after"}}, kShortTimeout);
  EXPECT_EQ(result.value("tag", std::string{}), "after");
}

TEST(McpTransport, NotificationCallbackFiresForServerInitiatedMessages) {
  McpStdioTransport transport(fake_server_config());

  std::mutex mutex;
  std::vector<std::string> methods_seen;
  transport.set_notification_callback(
      [&](const std::string &method, const nlohmann::json &) {
        std::lock_guard<std::mutex> lock(mutex);
        methods_seen.push_back(method);
      });

  const auto id = transport.peek_next_request_id_for_testing() + 1000;
  transport.call("test/trigger_server_request", {{"id", id}}, kShortTimeout);

  ASSERT_TRUE(pi::test::wait_until(
      [&] {
        std::lock_guard<std::mutex> lock(mutex);
        return !methods_seen.empty();
      },
      kShortTimeout, "a notification from the server-initiated exchange"));
}

TEST(McpTransport, DestructorTerminatesChildProcessCleanlyWithoutOrphaning) {
  pid_t child_pid = 0;
  {
    McpStdioTransport transport(fake_server_config());
    child_pid = transport.pid();
    ASSERT_GT(child_pid, 0);
    transport.call("test/echo", {{"tag", "warm-up"}}, kShortTimeout);
  }
  // The destructor already waited for and reaped the child. A further
  // waitpid on the same pid must report ECHILD (no such child), never a
  // live/zombie process left behind.
  int status = 0;
  const pid_t result = ::waitpid(child_pid, &status, WNOHANG);
  EXPECT_EQ(result, -1);
  EXPECT_EQ(errno, ECHILD);
  // Also confirm via /proc that nothing with this pid survives as a zombie
  // or otherwise.
  EXPECT_EQ(::kill(child_pid, 0), -1);
  EXPECT_EQ(errno, ESRCH);
}

TEST(McpTransport, ThrowsOrReturnsErrorWhenCommandDoesNotExist) {
  McpServerProcessConfig config;
  config.command = "/no/such/binary/exists-anywhere-xyz";
  EXPECT_THROW({ McpStdioTransport transport(config); }, std::exception);
}

TEST(McpTransport, NonJsonLineIsSkippedNotFatal) {
  McpStdioTransport transport(fake_server_config());
  const auto result = transport.call("test/emit_garbage_then_reply",
                                     {{"tag", "after-garbage"}}, kShortTimeout);
  EXPECT_EQ(result.value("tag", std::string{}), "after-garbage");
}

TEST(McpTransport, OversizedUnterminatedLineIsTreatedAsDeadTransport) {
  McpStdioTransport transport(fake_server_config());
  transport.notify("test/emit_oversized", nlohmann::json::object());

  ASSERT_TRUE(pi::test::wait_until(
      [&] { return !transport.is_alive(); }, std::chrono::milliseconds(5000),
      "child terminated after an oversized unterminated line"));

  EXPECT_THROW(transport.call("test/echo", {{"tag", "x"}},
                              std::chrono::milliseconds(500)),
               TransportError);
}

TEST(McpTransport, WritingToACrashedServerReturnsErrorNotSigpipeDeath) {
  McpStdioTransport transport(fake_server_config());
  transport.notify("test/crash", nlohmann::json::object());

  ASSERT_TRUE(pi::test::wait_until([&] { return !transport.is_alive(); },
                                   std::chrono::milliseconds(2000),
                                   "child exiting after test/crash"));

  // This is the regression proof: without SIGPIPE globally ignored (see
  // main.cpp/acp/main.cpp), this write would deliver SIGPIPE to THIS TEST
  // PROCESS with its default disposition, killing the whole test binary
  // instead of failing this one call cleanly.
  EXPECT_THROW(transport.call("test/echo", {{"tag", "x"}},
                              std::chrono::milliseconds(500)),
               TransportError);
  // Reaching this line at all is the real assertion: the test process is
  // still alive.
  SUCCEED();
}
