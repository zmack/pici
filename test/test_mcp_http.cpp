#include "core/mcp/mcp_client.h"
#include "core/mcp/mcp_http_transport.h"
#include "support/fake_http_mcp_server.h"
#include "support/gtest_helpers.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>

namespace {

using namespace std::chrono_literals;
using pi::core::mcp::McpClient;
using pi::core::mcp::McpHttpConfig;
using pi::core::mcp::McpHttpTransport;
using pi::core::mcp::TransportError;
using pi::test::FakeHttpMcpServer;

McpHttpConfig config_for(const FakeHttpMcpServer &server) {
  McpHttpConfig config;
  config.url = server.url();
  return config;
}

std::shared_ptr<McpClient> make_client(const FakeHttpMcpServer &server,
                                       McpHttpConfig config) {
  config.url = server.url();
  return std::make_shared<McpClient>(
      "remote", [config] { return std::make_unique<McpHttpTransport>(config); });
}

} // namespace

TEST(McpHttpTransport, RejectsUnsafeConfiguration) {
  McpHttpConfig config;
  config.url = "ftp://example.test/mcp";
  EXPECT_THROW(McpHttpTransport{config}, std::runtime_error);
  config.url = "http://user:secret@127.0.0.1/mcp";
  EXPECT_THROW(McpHttpTransport{config}, std::runtime_error);
  config.url = "http://127.0.0.1/mcp";
  config.headers["X-Test"] = "a\r\nInjected: 1";
  EXPECT_THROW(McpHttpTransport{config}, std::runtime_error);
  config.headers.clear();
  config.bearer_token = "tok\nen";
  EXPECT_THROW(McpHttpTransport{config}, std::runtime_error);
}

TEST(McpHttpTransport, DisplayUrlDropsQueryAndFragment) {
  McpHttpConfig config;
  config.url = "https://example.test:8443/api/mcp?key=SECRET#frag";
  McpHttpTransport transport(config);
  EXPECT_EQ(transport.display_url(), "https://example.test:8443/api/mcp");
}

TEST(McpHttpTransport, HandshakeCarriesSessionBearerAndProtocolHeaders) {
  FakeHttpMcpServer server;
  auto config = config_for(server);
  config.bearer_token = "tok-123";
  config.headers["X-Tenant"] = "acme";
  McpHttpTransport transport(config);

  const auto init = transport.call("initialize", {}, 2000ms);
  EXPECT_EQ(init.at("protocolVersion"), "2025-06-18");
  EXPECT_EQ(server.last_authorization(), "Bearer tok-123");

  transport.notify("notifications/initialized", nlohmann::json::object());
  const auto listed = transport.call("tools/list", {}, 2000ms);
  ASSERT_EQ(listed.at("tools").size(), 1u);
  EXPECT_EQ(server.last_protocol_header(), "2025-06-18");

  const auto bodies = server.bodies();
  ASSERT_EQ(bodies.size(), 3u);
  EXPECT_EQ(bodies[0].at("method"), "initialize");
  EXPECT_FALSE(bodies[1].contains("id"));
}

TEST(McpHttpTransport, ReadsResponseFromEventStreamAndForwardsNotifications) {
  FakeHttpMcpServer server;
  server.set_sse(true);
  server.set_sse_prefix(
      ": keepalive\n\n"
      "data: {\"jsonrpc\":\"2.0\",\"method\":\"notifications/tools/"
      "list_changed\"}\n\n");
  McpHttpTransport transport(config_for(server));

  std::vector<std::string> seen;
  transport.set_notification_callback(
      [&](const std::string &method, const nlohmann::json &) {
        seen.push_back(method);
      });
  const auto init = transport.call("initialize", {}, 2000ms);
  EXPECT_TRUE(init.contains("capabilities"));
  ASSERT_EQ(seen.size(), 1u);
  EXPECT_EQ(seen[0], "notifications/tools/list_changed");
}

TEST(McpHttpTransport, AnswersServerRequestsWithMethodNotFound) {
  FakeHttpMcpServer server;
  server.set_sse(true);
  server.set_sse_prefix(
      "data: {\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"roots/list\"}\n\n");
  McpHttpTransport transport(config_for(server));
  transport.call("initialize", {}, 2000ms);

  bool replied = false;
  for (const auto &body : server.bodies()) {
    if (body.contains("error") && body.value("id", 0) == 1) {
      EXPECT_EQ(body.at("error").at("code"), -32601);
      replied = true;
    }
  }
  EXPECT_TRUE(replied);
}

TEST(McpHttpTransport, MalformedBodiesAreTransportErrors) {
  FakeHttpMcpServer server;
  McpHttpTransport transport(config_for(server));

  server.set_override([](const httplib::Request &, httplib::Response &res) {
    res.set_content("{not json", "application/json");
    return true;
  });
  EXPECT_THROW(transport.call("tools/list", {}, 2000ms), TransportError);

  server.set_override([](const httplib::Request &, httplib::Response &res) {
    res.set_content("data: {oops\n\n", "text/event-stream");
    return true;
  });
  EXPECT_THROW(transport.call("tools/list", {}, 2000ms), TransportError);

  // Valid JSON-RPC, but for a different request id.
  server.set_override([](const httplib::Request &, httplib::Response &res) {
    res.set_content(R"({"jsonrpc":"2.0","id":999,"result":{}})",
                    "application/json");
    return true;
  });
  EXPECT_THROW(transport.call("tools/list", {}, 2000ms), TransportError);

  // 202 Accepted carries no response for a request.
  server.set_override([](const httplib::Request &, httplib::Response &res) {
    res.status = 202;
    return true;
  });
  EXPECT_THROW(transport.call("tools/list", {}, 2000ms), TransportError);
}

TEST(McpHttpTransport, JsonRpcErrorBecomesTransportError) {
  FakeHttpMcpServer server;
  McpHttpTransport transport(config_for(server));
  transport.call("initialize", {}, 2000ms);
  try {
    transport.call("bogus/method", {}, 2000ms);
    FAIL() << "expected TransportError";
  } catch (const TransportError &err) {
    EXPECT_NE(std::string(err.what()).find("no such method"),
              std::string::npos);
  }
}

TEST(McpHttpTransport, OversizedResponsesAreRejected) {
  FakeHttpMcpServer server;
  auto config = config_for(server);
  config.max_response_bytes = 1024;
  McpHttpTransport transport(config);

  const std::string big(64 * 1024, 'x');
  server.set_override([&](const httplib::Request &, httplib::Response &res) {
    res.set_content(R"({"jsonrpc":"2.0","id":1,"result":{"pad":")" + big +
                        R"("}})",
                    "application/json");
    return true;
  });
  EXPECT_THROW(transport.call("tools/list", {}, 2000ms), TransportError);

  server.set_override([&](const httplib::Request &, httplib::Response &res) {
    res.set_content("data: " + big + "\n\n", "text/event-stream");
    return true;
  });
  EXPECT_THROW(transport.call("tools/list", {}, 2000ms), TransportError);
}

TEST(McpHttpTransport, ErrorsNeverContainCredentials) {
  FakeHttpMcpServer server;
  auto config = config_for(server);
  config.url += "?api_key=URL-SECRET";
  config.bearer_token = "BEARER-SECRET";
  config.headers["X-Tenant"] = "HEADER-SECRET";
  McpHttpTransport transport(config);

  server.set_override([](const httplib::Request &, httplib::Response &res) {
    res.status = 401;
    res.set_content("echo Bearer BEARER-SECRET", "text/plain");
    return true;
  });
  try {
    transport.call("tools/list", {}, 2000ms);
    FAIL() << "expected TransportError";
  } catch (const TransportError &err) {
    const std::string what = err.what();
    EXPECT_NE(what.find("unauthorized"), std::string::npos) << what;
    EXPECT_EQ(what.find("SECRET"), std::string::npos) << what;
  }

  server.set_override([](const httplib::Request &, httplib::Response &res) {
    res.status = 500;
    return true;
  });
  try {
    transport.call("tools/list", {}, 2000ms);
    FAIL() << "expected TransportError";
  } catch (const TransportError &err) {
    const std::string what = err.what();
    EXPECT_NE(what.find("HTTP 500"), std::string::npos) << what;
    EXPECT_EQ(what.find("SECRET"), std::string::npos) << what;
  }

  server.stop();
  try {
    transport.call("tools/list", {}, 2000ms);
    FAIL() << "expected TransportError";
  } catch (const TransportError &err) {
    EXPECT_EQ(std::string(err.what()).find("SECRET"), std::string::npos)
        << err.what();
  }
}

TEST(McpHttpTransport, SlowServerTimesOut) {
  FakeHttpMcpServer server;
  McpHttpTransport transport(config_for(server));
  server.set_override([](const httplib::Request &, httplib::Response &res) {
    std::this_thread::sleep_for(1500ms);
    res.status = 500;
    return true;
  });
  const auto start = std::chrono::steady_clock::now();
  try {
    transport.call("tools/list", {}, 200ms);
    FAIL() << "expected TransportError";
  } catch (const TransportError &err) {
    EXPECT_NE(std::string(err.what()).find("timed out"), std::string::npos);
  }
  EXPECT_LT(std::chrono::steady_clock::now() - start, 1400ms);
}

TEST(McpHttpTransport, StopTokenCancelsInFlightCall) {
  FakeHttpMcpServer server;
  McpHttpTransport transport(config_for(server));
  server.set_override([](const httplib::Request &req, httplib::Response &res) {
    if (req.body.find("notifications/cancelled") != std::string::npos) {
      res.status = 202;
      return true;
    }
    std::this_thread::sleep_for(3000ms);
    res.status = 500;
    return true;
  });
  std::stop_source source;
  std::jthread canceller([&] {
    std::this_thread::sleep_for(150ms);
    source.request_stop();
  });
  const auto start = std::chrono::steady_clock::now();
  try {
    transport.call("tools/call", {}, 10000ms, source.get_token());
    FAIL() << "expected TransportError";
  } catch (const TransportError &err) {
    EXPECT_NE(std::string(err.what()).find("cancelled"), std::string::npos);
  }
  EXPECT_LT(std::chrono::steady_clock::now() - start, 2500ms);
}

TEST(McpHttpTransport, ExpiredSessionMarksTransportDead) {
  FakeHttpMcpServer server;
  McpHttpTransport transport(config_for(server));
  transport.call("initialize", {}, 2000ms);
  EXPECT_TRUE(transport.is_alive());

  server.set_session("sess-2");
  EXPECT_THROW(transport.call("tools/list", {}, 2000ms), TransportError);
  EXPECT_FALSE(transport.is_alive());
  EXPECT_THROW(transport.call("tools/list", {}, 2000ms), TransportError);
}

TEST(McpHttpTransport, DestructorTerminatesSession) {
  FakeHttpMcpServer server;
  {
    McpHttpTransport transport(config_for(server));
    transport.call("initialize", {}, 2000ms);
  }
  const auto deleted = server.deleted_sessions();
  ASSERT_EQ(deleted.size(), 1u);
  EXPECT_EQ(deleted[0], "sess-1");
}

TEST(McpHttpClient, ReconnectsAfterSessionExpiryAndMarksToolsStale) {
  FakeHttpMcpServer server;
  auto client = make_client(server, {});
  client->set_reconnect_backoff_for_testing(0ms);
  client->initialize(2000ms);
  ASSERT_EQ(client->list_tools().size(), 1u);
  EXPECT_FALSE(client->tools_stale());

  server.set_session("sess-2");
  EXPECT_THROW(client->call_tool("echo", {{"text", "x"}}, 2000ms),
               TransportError);
  EXPECT_FALSE(client->is_alive());

  std::string error;
  ASSERT_TRUE(client->reconnect(&error)) << error;
  EXPECT_TRUE(client->is_alive());
  EXPECT_TRUE(client->tools_stale());
  EXPECT_EQ(server.initialize_count(), 2);
  const auto result = client->call_tool("echo", {{"text", "hi"}}, 2000ms);
  EXPECT_EQ(result.at("content").at(0).at("text"), "hi");

  // Healthy client: reconnect is a no-op.
  ASSERT_TRUE(client->reconnect());
  EXPECT_EQ(server.initialize_count(), 2);
}

TEST(McpHttpClient, ReconnectFailureIsReportedAndThrottled) {
  FakeHttpMcpServer server;
  auto client = make_client(server, {});
  client->initialize(2000ms);
  server.set_session("sess-2");
  EXPECT_THROW(client->call_tool("echo", {{"text", "x"}}, 2000ms),
               TransportError);

  server.set_override([](const httplib::Request &, httplib::Response &res) {
    res.status = 503;
    return true;
  });
  std::string error;
  EXPECT_FALSE(client->reconnect(&error));
  EXPECT_NE(error.find("HTTP 503"), std::string::npos) << error;

  // Within the backoff window a second attempt does not hit the server.
  const auto posts = server.bodies().size();
  server.set_override(nullptr);
  std::string throttled;
  EXPECT_FALSE(client->reconnect(&throttled));
  EXPECT_EQ(throttled, error);
  EXPECT_EQ(server.bodies().size(), posts);
}

TEST(McpHttpClient, ListChangedNotificationMarksToolsStaleAndRelistPicksUpChange) {
  FakeHttpMcpServer server;
  auto client = make_client(server, {});
  client->initialize(2000ms);
  ASSERT_EQ(client->list_tools().size(), 1u);
  EXPECT_FALSE(client->tools_stale());

  server.set_tools({"echo", "extra"});
  server.set_sse(true);
  server.set_sse_prefix(
      "data: {\"jsonrpc\":\"2.0\",\"method\":\"notifications/tools/"
      "list_changed\"}\n\n");
  client->call_tool("echo", {{"text", "x"}}, 2000ms);
  EXPECT_TRUE(client->tools_stale());

  server.set_sse_prefix("");
  const auto tools = client->list_tools();
  EXPECT_EQ(tools.size(), 2u);
  EXPECT_FALSE(client->tools_stale());
}
