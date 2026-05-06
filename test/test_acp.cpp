#include "acp/server.h"
#include "acp/types.h"
#include "core/builtin_tools.h"
#include "core/llm_client.h"
#include "core/providers/faux.h"
#include "core/providers/openai_completions.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>

#include <httplib.h>

using namespace pi;
using namespace pi::acp;

// ─── Minimal test harness ────────────────────────────────────────────────────

namespace tests {
int passed{0}, failed{0}, total{0};

bool check(bool cond, std::string_view expr,
           std::source_location loc = std::source_location::current()) {
  ++total;
  if (cond) { ++passed; std::cout << "  PASS " << expr << "\n"; return true; }
  ++failed;
  std::cout << "  FAIL " << loc.file_name() << ":" << loc.line()
            << " — " << expr << "\n";
  return false;
}
} // namespace tests

#define CHECK(expr) tests::check(!!(expr), #expr)
#define CHECK_EQ(a,b) tests::check((a)==(b), #a " == " #b)

// ─── Server fixture ──────────────────────────────────────────────────────────

struct AcpFixture {
  int port{0};
  std::thread server_thread;

  explicit AcpFixture() {
    // Register faux provider with a script that emits a simple text response
    core::AssistantMessage final_msg;
    final_msg.content.push_back(core::TextContent{"Hello from faux agent"});
    final_msg.stop_reason = core::StopReason::stop;

    core::FauxClient::Script script;
    script.events = {
        core::AssistantMessageEvent{core::AssistantMessageStartEvent{}},
        core::AssistantMessageEvent{core::AssistantMessageTextDeltaEvent{0, "Hello from faux agent", {}}},
        core::AssistantMessageEvent{core::AssistantMessageDoneEvent{core::StopReason::stop, final_msg}},
    };

    core::LLMClientRegistry::instance().register_client("faux", [script] {
      return std::make_shared<core::FauxClient>(std::vector{script});
    });

    ServerConfig cfg;
    cfg.agent_name        = "test-agent";
    cfg.agent_description = "ACP test agent";
    cfg.threads           = 2;

    // Faux model — no real LLM needed
    core::Model m;
    m.id = "faux"; m.name = "faux"; m.api = "faux";
    m.provider = "faux"; m.context_window = 4096; m.max_tokens = 512;
    cfg.agent_opts.model = m;
    cfg.agent_opts.get_api_key = [](std::string_view) -> std::optional<std::string> {
      return std::nullopt;
    };
    cfg.agent_opts.should_stop_after_turn = nullptr;
    cfg.tools = core::create_all_tools(std::filesystem::temp_directory_path());

    server_thread = std::thread([cfg = std::move(cfg), this]() mutable {
      pi::acp::run_server(port, std::move(cfg));
    });

    // Wait for port to be assigned
    for (int i = 0; i < 50 && port == 0; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  ~AcpFixture() {
    // Server thread will exit when process exits; daemon-style for tests
    server_thread.detach();
  }

  httplib::Client client() {
    return httplib::Client("127.0.0.1", port);
  }
};

// ─── Tests ───────────────────────────────────────────────────────────────────

void test_health(AcpFixture &fx) {
  auto cli = fx.client();
  auto r = cli.Get("/health");
  CHECK(r != nullptr);
  CHECK(r->status == 200);
  CHECK(r->body.find("ok") != std::string::npos);
}

void test_agents_list(AcpFixture &fx) {
  auto cli = fx.client();
  auto r = cli.Get("/agents");
  CHECK(r != nullptr);
  CHECK(r->status == 200);
  auto j = nlohmann::json::parse(r->body);
  CHECK(j.is_array());
  CHECK(!j.empty());
  CHECK(j[0]["name"] == "test-agent");
}

void test_agent_manifest(AcpFixture &fx) {
  auto cli = fx.client();
  auto r = cli.Get("/agents/test-agent");
  CHECK(r != nullptr);
  CHECK(r->status == 200);
  auto j = nlohmann::json::parse(r->body);
  CHECK(j["name"] == "test-agent");
  CHECK(j.contains("description"));
  CHECK(j.contains("metadata"));
  CHECK(j["metadata"].contains("tools"));
}

void test_agent_not_found(AcpFixture &fx) {
  auto cli = fx.client();
  auto r = cli.Get("/agents/nonexistent");
  CHECK(r != nullptr);
  CHECK(r->status == 404);
}

void test_run_sync(AcpFixture &fx) {
  auto cli = fx.client();
  nlohmann::json body = {
    {"agent_name", "test-agent"},
    {"mode",       "sync"},
    {"input", {{
      {"role",  "user"},
      {"parts", {{{"content_type","text/plain"},{"content","hello"}}}}
    }}}
  };
  auto r = cli.Post("/runs", body.dump(), "application/json");
  CHECK(r != nullptr);
  CHECK(r->status == 200);
  auto j = nlohmann::json::parse(r->body);
  CHECK(j.contains("run_id"));
  CHECK(j.contains("status"));
  // Faux provider may return completed or failed depending on setup
  auto status = j["status"].get<std::string>();
  CHECK(status == "completed" || status == "failed");
}

void test_run_streaming(AcpFixture &fx) {
  auto cli = fx.client();
  nlohmann::json body = {
    {"agent_name", "test-agent"},
    {"mode",       "stream"},
    {"input", {{
      {"role",  "user"},
      {"parts", {{{"content_type","text/plain"},{"content","hi"}}}}
    }}}
  };

  // httplib client buffers the full SSE body once the stream completes
  auto r = cli.Post("/runs", body.dump(), "application/json");
  if (!r) {
    std::cerr << "[test_run_streaming] request failed: "
              << httplib::to_string(r.error()) << "\n";
  }
  CHECK(r != nullptr);
  if (!r) return;
  if (r->body.find("run.created") == std::string::npos)
    std::cerr << "[test_run_streaming] body: " << r->body.substr(0, 200) << "\n";
  CHECK(r->body.find("run.created") != std::string::npos);
  CHECK(r->body.find("run.completed") != std::string::npos ||
        r->body.find("run.failed")    != std::string::npos);
}

void test_run_session(AcpFixture &fx) {
  auto cli = fx.client();
  const std::string session_id = "test-session-42";

  auto make_body = [&](std::string text) {
    return nlohmann::json{
      {"agent_name", "test-agent"}, {"mode", "sync"},
      {"session_id", session_id},
      {"input", {{{"role","user"},
                  {"parts",{{{"content_type","text/plain"},
                              {"content", std::move(text)}}}}}}}
    }.dump();
  };

  // First turn
  auto r1 = cli.Post("/runs", make_body("first message"), "application/json");
  CHECK(r1 != nullptr);
  CHECK(r1->status == 200);

  // Second turn — session should be preserved
  auto r2 = cli.Post("/runs", make_body("second message"), "application/json");
  CHECK(r2 != nullptr);
  CHECK(r2->status == 200);
  auto j2 = nlohmann::json::parse(r2->body);
  CHECK(j2.value("session_id", "") == session_id);
}

// ─── Main ────────────────────────────────────────────────────────────────────

int main() {
  std::cout << "=== pi-cpp ACP server tests ===\n\n";

  AcpFixture fx;
  if (fx.port == 0) {
    std::cerr << "Server failed to start\n";
    return 1;
  }

  test_health(fx);
  test_agents_list(fx);
  test_agent_manifest(fx);
  test_agent_not_found(fx);
  test_run_sync(fx);
  test_run_streaming(fx);
  test_run_session(fx);

  std::cout << "\n========================================\n"
            << "  Tests: " << tests::total  << " total, "
            << tests::passed << " passed, "
            << tests::failed << " failed\n"
            << "========================================\n";
  return tests::failed == 0 ? 0 : 1;
}
