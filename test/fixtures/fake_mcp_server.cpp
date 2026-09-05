// Standalone test fixture: a minimal MCP server speaking newline-delimited
// JSON-RPC 2.0 over stdio. Built as its own executable (`fake-mcp-server`,
// see CMakeLists.txt) and driven as a real child process by
// test_mcp_transport.cpp, test_mcp_client.cpp, test_mcp_tool.cpp, and
// test_pici_process.cpp, so the tests exercise a real process boundary
// (pipes, signals, EOF-on-close) rather than a mock.
//
// Behavior is controlled two ways:
//   - Env vars, read once at startup:
//       FAKE_MCP_SERVER_MODE: "normal" (default), "silent_init" (never
//         replies to "initialize" -- for the client handshake-timeout
//         test), or "malformed_init" (replies to "initialize" with a
//         response missing "capabilities" -- for the malformed-handshake
//         test).
//       FAKE_MCP_SERVER_INIT_DELAY_MS: if set and > 0, sleeps this many
//         milliseconds before replying to "initialize" (in "normal" mode
//         only) -- used by test_pici_process.cpp's concurrent-handshake
//         test to prove wall-clock latency scales with the slowest
//         configured server, not the sum of all of them.
//   - A handful of test-only JSON-RPC methods (see the dispatch below),
//     issued as ordinary requests/notifications by the test through the
//     transport under test, covering: echoing (with optional reply delay,
//     for the out-of-order and timeout/race tests), triggering a
//     server-initiated request with a caller-chosen id (for the demux
//     regression test), crashing, emitting non-JSON/oversized stdout (for
//     the reader-hardening tests), and serving tools/list + tools/call (for
//     McpTool/McpClient discovery+invocation tests -- see the fixed tool
//     list below).
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
//
// Fixed tools/list response (see build_tools_list_result() below):
//   - "test_tool": a flexible echo/control tool. Its tools/call arguments
//     drive the response shape rather than needing many distinct tool
//     names: {"text": "..."} -> single text content block; {"blocks":
//     [...]} -> one text content block per array entry (joined test);
//     {"fail": true, "fail_message": "..."} -> isError:true with that
//     message; {"include_image_block": true} -> an extra non-text "image"
//     content block appended (must be dropped by McpTool, not mapped);
//     {"delay_ms": N} -> reply after an N-millisecond delay (for the
//     concurrency test).
//   - "broken_schema_tool": advertises an inputSchema with an unresolvable
//     local $ref ("#/definitions/DoesNotExist"), which
//     nlohmann::json_schema::json_validator::set_root_schema() is confirmed
//     (by reading its source) to throw std::invalid_argument on -- the
//     fixture for the discovery-time schema-validation test.

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

int init_delay_ms_from_env() {
  const char *value = std::getenv( // NOLINT(concurrency-mt-unsafe)
      "FAKE_MCP_SERVER_INIT_DELAY_MS");
  if (value == nullptr)
    return 0;
  try {
    return std::stoi(value);
  } catch (...) {
    return 0;
  }
}

json canned_initialize_result() {
  return json{
      {"protocolVersion", "2024-11-05"},
      {"capabilities", json::object()},
      {"serverInfo", {{"name", "fake-mcp-server"}, {"version", "0.0.1"}}},
  };
}

json tools_list_result() {
  return json{
      {"tools",
       json::array({
           json{{"name", "test_tool"},
                {"description",
                 "Flexible echo/control tool driven by its own arguments; "
                 "see fake_mcp_server.cpp's file comment."},
                {"inputSchema",
                 json{{"type", "object"},
                      {"properties",
                       json{{"text", json{{"type", "string"}}},
                            {"blocks", json{{"type", "array"},
                                            {"items", json{{"type",
                                                            "string"}}}}},
                            {"fail", json{{"type", "boolean"}}},
                            {"fail_message", json{{"type", "string"}}},
                            {"include_image_block",
                             json{{"type", "boolean"}}},
                            {"delay_ms", json{{"type", "integer"}}}}}}}},
           json{{"name", "broken_schema_tool"},
                {"description",
                 "Advertises a malformed inputSchema (unresolvable $ref) -- "
                 "must be skipped at discovery time, never constructed into "
                 "an McpTool."},
                {"inputSchema",
                 json{{"type", "object"},
                      {"properties",
                       json{{"x", json{{"$ref",
                                        "#/definitions/DoesNotExist"}}}}}}}},
       })},
  };
}

json test_tool_call_result(const json &arguments) {
  json content = json::array();
  if (arguments.value("fail", false)) {
    content.push_back(json{
        {"type", "text"},
        {"text", arguments.value("fail_message", std::string("failed"))},
    });
  } else if (arguments.contains("blocks") &&
            arguments.at("blocks").is_array()) {
    for (const auto &block : arguments.at("blocks")) {
      if (block.is_string())
        content.push_back(
            json{{"type", "text"}, {"text", block.get<std::string>()}});
    }
  } else {
    content.push_back(json{
        {"type", "text"},
        {"text", arguments.value("text", std::string("ok"))},
    });
  }
  if (arguments.value("include_image_block", false)) {
    // Deliberately non-text: McpTool must drop this, not map it.
    content.push_back(json{{"type", "image"},
                           {"data", "YmFzZTY0Cg=="},
                           {"mimeType", "image/png"}});
  }

  json result = json{{"content", content}};
  if (arguments.value("fail", false)) {
    result["isError"] = true;
  }
  return result;
}

json tools_call_result(const json &params) {
  const std::string name = params.value("name", std::string());
  const json arguments =
      params.contains("arguments") ? params.at("arguments") : json::object();
  if (name == "test_tool")
    return test_tool_call_result(arguments);
  if (name == "broken_schema_tool")
    return json{{"content", json::array({json{{"type", "text"},
                                               {"text", "unreachable in "
                                                        "practice"}}})}};
  return json{{"isError", true},
             {"content", json::array({json{{"type", "text"},
                                           {"text", "unknown tool: " +
                                                        name}}})}};
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
      const int delay_ms = init_delay_ms_from_env();
      if (delay_ms > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
      write_json(json{{"jsonrpc", "2.0"},
                      {"id", id},
                      {"result", canned_initialize_result()}});
      continue;
    }

    if (method == "tools/list") {
      write_json(json{
          {"jsonrpc", "2.0"}, {"id", id}, {"result", tools_list_result()}});
      continue;
    }

    if (method == "tools/call") {
      const int delay_ms = params.contains("arguments")
                              ? params.at("arguments").value("delay_ms", 0)
                              : 0;
      if (delay_ms > 0) {
        std::thread([id, params, delay_ms]() mutable {
          std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
          write_json(json{{"jsonrpc", "2.0"},
                          {"id", id},
                          {"result", tools_call_result(params)}});
        }).detach();
      } else {
        write_json(json{{"jsonrpc", "2.0"},
                        {"id", id},
                        {"result", tools_call_result(params)}});
      }
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
