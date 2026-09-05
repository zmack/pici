// Standalone test fixture: a minimal MCP server speaking newline-delimited
// JSON-RPC 2.0 over stdio. Built as its own executable (`fake-mcp-server`,
// see CMakeLists.txt) and driven as a real child process by
// test_mcp_transport.cpp and test_mcp_client.cpp, so the tests exercise a
// real process boundary (pipes, signals, EOF-on-close) rather than a mock.
//
// Behavior is controlled two ways:
//   - FAKE_MCP_SERVER_MODE env var, read once at startup: "normal" (default),
//     "silent_init" (never replies to "initialize" -- for the client
//     handshake-timeout test), or "malformed_init" (replies to "initialize"
//     with a response missing "capabilities" -- for the malformed-handshake
//     test).
//   - A handful of test-only JSON-RPC methods (see the dispatch below),
//     issued as ordinary requests/notifications by the test through the
//     transport under test, covering: echoing (with optional reply delay,
//     for the out-of-order and timeout/race tests), triggering a
//     server-initiated request with a caller-chosen id (for the demux
//     regression test), crashing, and emitting non-JSON/oversized stdout
//     (for the reader-hardening tests).
//
// This process itself also implements the SAME method-presence-first demux
// rule the client under test implements (see mcp_protocol.h's discriminator
// comment), on the one incoming shape it needs to recognize as a reply to
// its OWN outstanding request: an incoming line with an "id" but no
// "method". This is what lets it confirm -- and report back via a
// notification the test's NotificationCallback can observe -- that the
// client answered our server-initiated request correctly instead of
// cross-wiring it with a real pending client call that happens to share the
// same id.

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace {

using nlohmann::json;

std::mutex g_stdout_mutex;

void write_line(const std::string &line) {
  std::lock_guard<std::mutex> lock(g_stdout_mutex);
  std::fwrite(line.data(), 1, line.size(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

void write_json(const json &message) { write_line(message.dump()); }

std::string mode_from_env() {
  const char *value =
      std::getenv("FAKE_MCP_SERVER_MODE"); // NOLINT(concurrency-mt-unsafe)
  return value != nullptr ? std::string(value) : std::string("normal");
}

json canned_initialize_result() {
  return json{
      {"protocolVersion", "2024-11-05"},
      {"capabilities", json::object()},
      {"serverInfo", {{"name", "fake-mcp-server"}, {"version", "0.0.1"}}},
  };
}

} // namespace

int main() {
  const std::string mode = mode_from_env();

  // The id of a server-initiated request we're waiting to see a reply to
  // (see the file comment) -- nullopt when we have none outstanding.
  std::optional<long long> outstanding_server_request_id;

  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty())
      continue;

    const auto parsed = json::parse(line, nullptr, false);
    if (parsed.is_discarded())
      continue; // ignore malformed input from the test driver

    const bool has_method =
        parsed.contains("method") && parsed.at("method").is_string();
    const bool has_id = parsed.contains("id") && !parsed.at("id").is_null();

    if (!has_method && has_id) {
      // A reply to one of OUR OWN outstanding requests (id, no method) --
      // mirrors the client's own demux rule, applied here to the one shape
      // this fixture needs to recognize.
      if (outstanding_server_request_id &&
          parsed.at("id").is_number_integer() &&
          parsed.at("id").get<long long>() == *outstanding_server_request_id) {
        const bool got_not_found =
            parsed.contains("error") &&
            parsed.at("error").value("code", 0) == -32601;
        write_json(json{
            {"jsonrpc", "2.0"},
            {"method", "test/serverRequestNotFoundConfirmed"},
            {"params", {{"gotNotFound", got_not_found}}},
        });
        outstanding_server_request_id.reset();
      }
      continue;
    }

    if (!has_method)
      continue; // neither a recognized request nor a reply -- drop

    const std::string method = parsed.at("method").get<std::string>();
    const json params =
        parsed.contains("params") ? parsed.at("params") : json::object();

    if (!has_id) {
      // Notification: no reply expected/possible.
      if (method == "notifications/initialized") {
        // Handshake complete; nothing to do.
      } else if (method == "test/crash") {
        std::_Exit(1); // abrupt, unclean exit -- simulates a crashed server
      } else if (method == "test/emit_oversized") {
        // ~40MB with no trailing newline: exercises the reader's
        // unterminated-line cap. Written directly (not via write_json,
        // which appends a newline) and deliberately left running
        // afterward so the process keeps its stdin open in case anything
        // else is queued.
        const std::string huge(40ULL * 1024 * 1024, 'x');
        std::lock_guard<std::mutex> lock(g_stdout_mutex);
        std::fwrite(huge.data(), 1, huge.size(), stdout);
        std::fflush(stdout);
      }
      continue;
    }

    const json id = parsed.at("id");

    if (method == "initialize") {
      if (mode == "silent_init")
        continue; // deliberately never reply -- exercises the timeout path
      if (mode == "malformed_init") {
        write_json(json{{"jsonrpc", "2.0"},
                        {"id", id},
                        {"result", json{{"protocolVersion", "2024-11-05"}}}});
        continue;
      }
      write_json(json{{"jsonrpc", "2.0"},
                      {"id", id},
                      {"result", canned_initialize_result()}});
      continue;
    }

    if (method == "test/echo") {
      const int delay_ms = params.value("delay_ms", 0);
      json result = params;
      if (delay_ms > 0) {
        std::thread([id, result, delay_ms]() mutable {
          std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
          write_json(json{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
        }).detach();
      } else {
        write_json(json{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
      }
      continue;
    }

    if (method == "test/trigger_server_request") {
      // Sends a server-initiated request using a caller-chosen id (so the
      // test can deliberately make it collide with a real pending client
      // request's id), then acks this control call immediately.
      const long long request_id = params.value("id", 0LL);
      const std::string request_method =
          params.value("method", std::string("roots/list"));
      outstanding_server_request_id = request_id;
      write_json(json{{"jsonrpc", "2.0"},
                      {"id", request_id},
                      {"method", request_method},
                      {"params", json::object()}});
      write_json(
          json{{"jsonrpc", "2.0"}, {"id", id}, {"result", json{{"ok", true}}}});
      continue;
    }

    if (method == "test/emit_garbage_then_reply") {
      // A non-JSON "startup banner" line, immediately followed by the real
      // reply -- exercises "skip malformed lines, don't treat as fatal".
      write_line("not-json-at-all-startup-banner");
      write_json(json{{"jsonrpc", "2.0"}, {"id", id}, {"result", params}});
      continue;
    }

    // Unknown method: reply with a generic not-found so nothing hangs
    // waiting on a response we'll never otherwise send.
    write_json(
        json{{"jsonrpc", "2.0"},
             {"id", id},
             {"error", {{"code", -32601}, {"message", "Method not found"}}}});
  }

  return 0;
}
