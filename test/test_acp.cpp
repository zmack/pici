#include "acp/server.h"
#include "acp/types.h"
#include "core/builtin_tools.h"
#include "core/llm_client.h"
#include "core/providers/faux.h"
#include "core/providers/openai_completions.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <map>
#include <memory>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>

#include <gtest/gtest.h>

#include <httplib.h>

using namespace pi;
using namespace pi::acp;

// ─── Server fixture ──────────────────────────────────────────────────────────

struct AcpFixture {
  std::atomic<int> port{0};
  std::thread server_thread;

  explicit AcpFixture() {
    // Register faux provider with a script that emits a simple text response
    core::AssistantMessage final_msg;
    final_msg.content.push_back(core::TextContent{"Hello from faux agent"});
    final_msg.stop_reason = core::StopReason::stop;

    core::FauxClient::Script script;
    script.events = {
        core::AssistantMessageEvent{core::AssistantMessageStartEvent{}},
        core::AssistantMessageEvent{core::AssistantMessageTextDeltaEvent{
            0, "Hello from faux agent", {}}},
        core::AssistantMessageEvent{
            core::AssistantMessageDoneEvent{core::StopReason::stop, final_msg}},
    };

    core::LLMClientRegistry::instance().register_client("faux", [script] {
      return std::make_shared<core::FauxClient>(std::vector{script});
    });

    ServerConfig cfg;
    cfg.agent_name = "test-agent";
    cfg.agent_description = "ACP test agent";
    cfg.threads = 2;

    // Faux model — no real LLM needed
    core::Model m;
    m.id = "faux";
    m.name = "faux";
    m.api = "faux";
    m.provider = "faux";
    m.context_window = 4096;
    m.max_tokens = 512;
    cfg.agent_opts.model = m;
    core::ProviderConfig faux_provider;
    faux_provider.id = "faux";
    faux_provider.api = "faux";
    faux_provider.base_url = "http://faux.test/v1";
    faux_provider.auth = core::ProviderAuthPolicy::none;
    core::ConfiguredModel faux_model;
    faux_model.id = "faux";
    faux_provider.models.push_back(faux_model);
    core::ProviderConfig alternate_provider;
    alternate_provider.id = "faux-b";
    alternate_provider.api = "faux";
    alternate_provider.base_url = "http://faux-b.test/v1";
    alternate_provider.auth = core::ProviderAuthPolicy::none;
    core::ConfiguredModel alternate_model;
    alternate_model.id = "other";
    alternate_provider.models.push_back(alternate_model);

    // A second, deliberately slow faux API: each turn sleeps between
    // scripted events, giving the Phase 7 concurrency tests below a wide,
    // reliable window to observe a run still in flight.
    core::AssistantMessage slow_final_msg;
    slow_final_msg.content.push_back(
        core::TextContent{"Hello from slow faux agent"});
    slow_final_msg.stop_reason = core::StopReason::stop;
    core::FauxClient::Script slow_script;
    slow_script.events = {
        core::AssistantMessageEvent{core::AssistantMessageStartEvent{}},
        core::AssistantMessageEvent{core::AssistantMessageTextDeltaEvent{
            0, "Hello from slow faux agent", {}}},
        core::AssistantMessageEvent{core::AssistantMessageDoneEvent{
            core::StopReason::stop, slow_final_msg}},
    };
    slow_script.delay_between = std::chrono::milliseconds(300);
    core::LLMClientRegistry::instance().register_client(
        "faux-slow", [slow_script] {
          return std::make_shared<core::FauxClient>(std::vector{slow_script});
        });
    core::ProviderConfig slow_provider;
    slow_provider.id = "faux-slow-provider";
    slow_provider.api = "faux-slow";
    slow_provider.base_url = "http://faux-slow.test/v1";
    slow_provider.auth = core::ProviderAuthPolicy::none;
    core::ConfiguredModel slow_model;
    slow_model.id = "slow";
    slow_provider.models.push_back(slow_model);

    cfg.model_registry = std::make_shared<const core::ModelRegistry>(
        std::map<std::string, core::ProviderConfig>{
            {"faux", faux_provider},
            {"faux-b", alternate_provider},
            {"faux-slow-provider", slow_provider}});
    cfg.agent_opts.model_registry = cfg.model_registry;
    cfg.agent_opts.get_api_key =
        [](std::string_view) -> std::optional<std::string> {
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

  httplib::Client client() { return httplib::Client("127.0.0.1", port); }
};

// All ACP cases share one in-process server. Starting one server per case
// would add substantial startup cost and would change the original suite's
// process-level lifecycle.
AcpFixture &shared_acp_fixture() {
  static AcpFixture fixture;
  return fixture;
}

// ─── Tests ───────────────────────────────────────────────────────────────────

void test_health(AcpFixture &fx) {
  auto cli = fx.client();
  auto r = cli.Get("/health");
  EXPECT_TRUE(r != nullptr);
  EXPECT_TRUE(r->status == 200);
  EXPECT_TRUE(r->body.find("ok") != std::string::npos);
}

void test_agents_list(AcpFixture &fx) {
  auto cli = fx.client();
  auto r = cli.Get("/agents");
  EXPECT_TRUE(r != nullptr);
  EXPECT_TRUE(r->status == 200);
  auto j = nlohmann::json::parse(r->body);
  EXPECT_TRUE(j.is_array());
  EXPECT_TRUE(!j.empty());
  EXPECT_TRUE(j[0]["name"] == "test-agent");
}

void test_agent_manifest(AcpFixture &fx) {
  auto cli = fx.client();
  auto r = cli.Get("/agents/test-agent");
  EXPECT_TRUE(r != nullptr);
  EXPECT_TRUE(r->status == 200);
  auto j = nlohmann::json::parse(r->body);
  EXPECT_TRUE(j["name"] == "test-agent");
  EXPECT_TRUE(j.contains("description"));
  EXPECT_TRUE(j.contains("metadata"));
  EXPECT_TRUE(j["metadata"].contains("tools"));
}

void test_agent_not_found(AcpFixture &fx) {
  auto cli = fx.client();
  auto r = cli.Get("/agents/nonexistent");
  EXPECT_TRUE(r != nullptr);
  EXPECT_TRUE(r->status == 404);
}

void test_run_sync(AcpFixture &fx) {
  auto cli = fx.client();
  nlohmann::json body = {
      {"agent_name", "test-agent"},
      {"mode", "sync"},
      {"input",
       {{{"role", "user"},
         {"parts",
          {{{"content_type", "text/plain"}, {"content", "hello"}}}}}}}};
  auto r = cli.Post("/runs", body.dump(), "application/json");
  EXPECT_TRUE(r != nullptr);
  EXPECT_TRUE(r->status == 200);
  auto j = nlohmann::json::parse(r->body);
  EXPECT_TRUE(j.contains("run_id"));
  EXPECT_TRUE(j.contains("status"));
  // Faux provider may return completed or failed depending on setup
  auto status = j["status"].get<std::string>();
  EXPECT_TRUE(status == "completed" || status == "failed");
}

void test_run_streaming(AcpFixture &fx) {
  auto cli = fx.client();
  nlohmann::json body = {
      {"agent_name", "test-agent"},
      {"mode", "stream"},
      {"input",
       {{{"role", "user"},
         {"parts", {{{"content_type", "text/plain"}, {"content", "hi"}}}}}}}};

  // httplib client buffers the full SSE body once the stream completes
  auto r = cli.Post("/runs", body.dump(), "application/json");
  if (!r) {
    std::cerr << "[test_run_streaming] request failed: "
              << httplib::to_string(r.error()) << "\n";
  }
  EXPECT_TRUE(r != nullptr);
  if (!r)
    return;
  if (r->body.find("run.created") == std::string::npos)
    std::cerr << "[test_run_streaming] body: " << r->body.substr(0, 200)
              << "\n";
  EXPECT_TRUE(r->body.find("run.created") != std::string::npos);
  EXPECT_TRUE(r->body.find("run.completed") != std::string::npos ||
              r->body.find("run.failed") != std::string::npos);
}

void test_run_session(AcpFixture &fx) {
  auto cli = fx.client();
  const std::string session_id = "test-session-42";

  auto make_body = [&](std::string text, bool alternate = false) {
    nlohmann::json body = {{"agent_name", "test-agent"},
                           {"mode", "sync"},
                           {"session_id", session_id},
                           {"input",
                            {{{"role", "user"},
                              {"parts",
                               {{{"content_type", "text/plain"},
                                 {"content", std::move(text)}}}}}}}};
    if (alternate) {
      body["provider"] = "faux-b";
      body["model"] = "other";
    }
    return body.dump();
  };

  // First turn
  auto r1 = cli.Post("/runs", make_body("first message"), "application/json");
  EXPECT_TRUE(r1 != nullptr);
  EXPECT_TRUE(r1->status == 200);

  // Second turn explicitly switches provider/model.
  auto r2 =
      cli.Post("/runs", make_body("second message", true), "application/json");
  EXPECT_TRUE(r2 != nullptr);
  EXPECT_TRUE(r2->status == 200);
  auto j2 = nlohmann::json::parse(r2->body);
  EXPECT_TRUE(j2.value("session_id", "") == session_id);
  EXPECT_TRUE(j2.value("provider", "") == "faux-b");
  EXPECT_TRUE(j2.value("model", "") == "other");

  // Third turn without selection restores the journaled model.
  auto r3 = cli.Post("/runs", make_body("third message"), "application/json");
  EXPECT_TRUE(r3 != nullptr);
  EXPECT_TRUE(r3->status == 200);
  auto j3 = nlohmann::json::parse(r3->body);
  EXPECT_TRUE(j3.value("provider", "") == "faux-b");
  EXPECT_TRUE(j3.value("model", "") == "other");
}

// plans/session-runtime-migration.md Phase 6, decision (a): successive
// /runs calls against the same session_id reuse one SessionRuntime, looked
// up in a session_id-keyed registry (see handlers.cpp's session_runtimes
// map). This test targets the actual regression risk of that registry --
// two different session_ids must never share state -- by giving each its
// own model selection and confirming a later unselected run on either one
// restores *that session's own* journaled model, not the other session's.
void test_run_session_isolation(AcpFixture &fx) {
  auto cli = fx.client();
  const std::string session_a = "isolation-session-a";
  const std::string session_b = "isolation-session-b";

  auto make_body = [](const std::string &session_id, std::string text,
                      bool select_alternate) {
    nlohmann::json body = {{"agent_name", "test-agent"},
                           {"mode", "sync"},
                           {"session_id", session_id},
                           {"input",
                            {{{"role", "user"},
                              {"parts",
                               {{{"content_type", "text/plain"},
                                 {"content", std::move(text)}}}}}}}};
    if (select_alternate) {
      body["provider"] = "faux-b";
      body["model"] = "other";
    }
    return body.dump();
  };

  // session_a explicitly selects the alternate model; session_b never does,
  // so it stays on the default "faux" model throughout.
  auto a1 =
      cli.Post("/runs", make_body(session_a, "a1", true), "application/json");
  EXPECT_TRUE(a1 != nullptr);
  EXPECT_TRUE(a1->status == 200);
  auto b1 =
      cli.Post("/runs", make_body(session_b, "b1", false), "application/json");
  EXPECT_TRUE(b1 != nullptr);
  EXPECT_TRUE(b1->status == 200);

  // Follow-up on each with no explicit selection must restore that
  // session's own journaled model -- if the registry ever mixed the two
  // SessionRuntimes up, session_b would incorrectly see "faux-b"/"other"
  // here.
  auto a2 =
      cli.Post("/runs", make_body(session_a, "a2", false), "application/json");
  EXPECT_TRUE(a2 != nullptr);
  EXPECT_TRUE(a2->status == 200);
  auto ja2 = nlohmann::json::parse(a2->body);
  EXPECT_TRUE(ja2.value("session_id", "") == session_a);
  EXPECT_TRUE(ja2.value("provider", "") == "faux-b");
  EXPECT_TRUE(ja2.value("model", "") == "other");

  auto b2 =
      cli.Post("/runs", make_body(session_b, "b2", false), "application/json");
  EXPECT_TRUE(b2 != nullptr);
  EXPECT_TRUE(b2->status == 200);
  auto jb2 = nlohmann::json::parse(b2->body);
  EXPECT_TRUE(jb2.value("session_id", "") == session_b);
  EXPECT_TRUE(jb2.value("provider", "") == "faux");
  EXPECT_TRUE(jb2.value("model", "") == "faux");
}

// plans/session-runtime-migration.md Phase 7: removing durable_run_mutex
// means /runs calls against different session_ids must run fully
// concurrently rather than being serialized process-wide. Prove it: start a
// deliberately slow run (faux-slow-provider/slow, ~900ms end to end) in a
// background thread against one session, then -- while it's still in
// flight -- run a fast request against a *different* session from the main
// thread and confirm it completes quickly rather than queuing behind the
// slow one.
void test_run_concurrent_sessions_no_blocking(AcpFixture &fx) {
  auto slow_client = fx.client();
  auto fast_client = fx.client();

  nlohmann::json slow_body = {
      {"agent_name", "test-agent"},
      {"mode", "sync"},
      {"session_id", "concurrent-slow"},
      {"provider", "faux-slow-provider"},
      {"model", "slow"},
      {"input",
       {{{"role", "user"},
         {"parts", {{{"content_type", "text/plain"}, {"content", "slow"}}}}}}}};

  std::optional<httplib::Result> slow_result;
  std::thread slow_thread([&] {
    slow_result =
        slow_client.Post("/runs", slow_body.dump(), "application/json");
  });

  // Give the slow run a head start so it's genuinely in flight (past
  // session lookup/construction and into Agent::prompt()'s streaming
  // window) before the fast request races it.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  nlohmann::json fast_body = {
      {"agent_name", "test-agent"},
      {"mode", "sync"},
      {"session_id", "concurrent-fast"},
      {"input",
       {{{"role", "user"},
         {"parts", {{{"content_type", "text/plain"}, {"content", "fast"}}}}}}}};

  const auto fast_start = std::chrono::steady_clock::now();
  auto fast_result =
      fast_client.Post("/runs", fast_body.dump(), "application/json");
  const auto fast_elapsed = std::chrono::steady_clock::now() - fast_start;

  slow_thread.join();

  EXPECT_TRUE(fast_result != nullptr);
  if (fast_result) {
    EXPECT_TRUE(fast_result->status == 200);
    auto jf = nlohmann::json::parse(fast_result->body);
    EXPECT_TRUE(jf.value("session_id", "") == "concurrent-fast");
    EXPECT_TRUE(jf.value("status", "") == "completed");
  }
  // The slow run's script alone takes ~900ms (three scripted events, 300ms
  // apart); the fast request must not have queued behind it.
  EXPECT_TRUE(fast_elapsed < std::chrono::milliseconds(500));

  EXPECT_TRUE(slow_result.has_value() && *slow_result != nullptr);
  if (slow_result && *slow_result) {
    EXPECT_TRUE((*slow_result)->status == 200);
    auto js = nlohmann::json::parse((*slow_result)->body);
    EXPECT_TRUE(js.value("session_id", "") == "concurrent-slow");
    EXPECT_TRUE(js.value("status", "") == "completed");
  }
}

// Same scenario, but the second request targets the *same* session_id as
// the in-flight slow run. core::Agent already rejects a second concurrent
// run against one instance (see the pre-removal audit above
// durable_run_mutex's old declaration); this confirms handlers.cpp turns
// that into an explicit 409 rather than an uncaught exception, and that the
// original run still completes normally afterward.
void test_run_concurrent_same_session_conflict(AcpFixture &fx) {
  auto slow_client = fx.client();
  auto conflict_client = fx.client();

  const std::string session_id = "concurrent-conflict";
  nlohmann::json slow_body = {
      {"agent_name", "test-agent"},
      {"mode", "sync"},
      {"session_id", session_id},
      {"provider", "faux-slow-provider"},
      {"model", "slow"},
      {"input",
       {{{"role", "user"},
         {"parts", {{{"content_type", "text/plain"}, {"content", "slow"}}}}}}}};

  std::optional<httplib::Result> slow_result;
  std::thread slow_thread([&] {
    slow_result =
        slow_client.Post("/runs", slow_body.dump(), "application/json");
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  nlohmann::json conflict_body = {
      {"agent_name", "test-agent"},
      {"mode", "sync"},
      {"session_id", session_id},
      {"input",
       {{{"role", "user"},
         {"parts",
          {{{"content_type", "text/plain"}, {"content", "conflict"}}}}}}}};
  auto conflict_result =
      conflict_client.Post("/runs", conflict_body.dump(), "application/json");

  slow_thread.join();

  EXPECT_TRUE(conflict_result != nullptr);
  if (conflict_result)
    EXPECT_TRUE(conflict_result->status == 409);

  EXPECT_TRUE(slow_result.has_value() && *slow_result != nullptr);
  if (slow_result && *slow_result) {
    EXPECT_TRUE((*slow_result)->status == 200);
    auto js = nlohmann::json::parse((*slow_result)->body);
    EXPECT_TRUE(js.value("session_id", "") == session_id);
    EXPECT_TRUE(js.value("status", "") == "completed");
  }
}

void test_agent_tasks(AcpFixture &fx) {
  auto cli = fx.client();
  nlohmann::json body = {
      {"task_name", "review"},
      {"prompt", "review the change"},
  };
  auto spawn = cli.Post("/tasks", body.dump(), "application/json");
  EXPECT_TRUE(spawn != nullptr);
  EXPECT_TRUE(spawn && spawn->status == 202);
  if (!spawn)
    return;

  auto task = nlohmann::json::parse(spawn->body);
  const auto task_id = task.value("id", std::string{});
  EXPECT_TRUE(!task_id.empty());
  EXPECT_TRUE(task.value("task_path", "") == "/root/review");

  auto events = cli.Get("/tasks/events?timeout_ms=0");
  EXPECT_TRUE(events != nullptr);
  if (events) {
    EXPECT_TRUE(events->status == 200);
    const auto event_body = nlohmann::json::parse(events->body);
    EXPECT_TRUE(event_body["events"].is_array());
    bool saw_spawn = false;
    for (const auto &record : event_body["events"]) {
      if (record.value("event", nlohmann::json::object()).value("type", "") ==
              "task.spawned" &&
          record["event"].value("task_id", "") == task_id)
        saw_spawn = true;
    }
    EXPECT_TRUE(saw_spawn);
  }

  auto wait_for_terminal = [&](std::uint64_t &generation) {
    nlohmann::json current;
    for (int attempt = 0; attempt < 20; ++attempt) {
      auto get = cli.Get("/tasks/" + task_id);
      EXPECT_TRUE(get != nullptr);
      if (!get)
        return current;
      current = nlohmann::json::parse(get->body);
      generation = current.value("generation", generation);
      const auto status = current.value("status", "");
      if (status == "completed" || status == "errored" ||
          status == "interrupted")
        return current;

      auto wait = cli.Post("/tasks/wait",
                           nlohmann::json{{"targets", {task_id}},
                                          {"after_generation", generation},
                                          {"timeout_ms", 1000}}
                               .dump(),
                           "application/json");
      EXPECT_TRUE(wait != nullptr);
      EXPECT_TRUE(wait && wait->status == 200);
      if (wait && wait->status == 200) {
        const auto changed = nlohmann::json::parse(wait->body)["changed"];
        if (changed.is_array() && !changed.empty())
          generation = changed[0].value("generation", generation);
      }
    }
    return current;
  };

  std::uint64_t generation = task.value("generation", 0ULL);
  auto completed = wait_for_terminal(generation);
  const auto first_status = completed.value("status", "");
  EXPECT_TRUE(first_status == "completed" || first_status == "errored" ||
              first_status == "interrupted");
  EXPECT_TRUE(completed.contains("result"));

  auto follow = cli.Post("/tasks/" + task_id + "/follow-up",
                         nlohmann::json{{"message", "follow up"}}.dump(),
                         "application/json");
  EXPECT_TRUE(follow != nullptr);
  EXPECT_TRUE(follow && follow->status == 202);
  auto followed = wait_for_terminal(generation);
  EXPECT_TRUE(followed.value("status", "") == "completed" ||
              followed.value("status", "") == "errored" ||
              followed.value("status", "") == "interrupted");

  auto list = cli.Get("/tasks");
  EXPECT_TRUE(list != nullptr);
  EXPECT_TRUE(list && list->status == 200);
  if (list) {
    const auto values = nlohmann::json::parse(list->body);
    EXPECT_TRUE(values.is_array());
    bool found = false;
    for (const auto &value : values)
      found = found || value.value("id", "") == task_id;
    EXPECT_TRUE(found);
  }

  auto close =
      cli.Post("/tasks/" + task_id + "/close", "{}", "application/json");
  EXPECT_TRUE(close != nullptr);
  EXPECT_TRUE(close && close->status == 200);
  auto gone = cli.Get("/tasks/" + task_id);
  EXPECT_TRUE(gone != nullptr);
  EXPECT_TRUE(gone && gone->status == 404);
}

TEST(Acp, Health) {
  auto &fx = shared_acp_fixture();
  ASSERT_GT(fx.port.load(), 0);
  test_health(fx);
}

TEST(Acp, AgentsList) {
  auto &fx = shared_acp_fixture();
  ASSERT_GT(fx.port.load(), 0);
  test_agents_list(fx);
}

TEST(Acp, AgentManifest) {
  auto &fx = shared_acp_fixture();
  ASSERT_GT(fx.port.load(), 0);
  test_agent_manifest(fx);
}

TEST(Acp, AgentNotFound) {
  auto &fx = shared_acp_fixture();
  ASSERT_GT(fx.port.load(), 0);
  test_agent_not_found(fx);
}

TEST(Acp, RunSync) {
  auto &fx = shared_acp_fixture();
  ASSERT_GT(fx.port.load(), 0);
  test_run_sync(fx);
}

TEST(Acp, RunStreaming) {
  auto &fx = shared_acp_fixture();
  ASSERT_GT(fx.port.load(), 0);
  test_run_streaming(fx);
}

TEST(Acp, RunSession) {
  auto &fx = shared_acp_fixture();
  ASSERT_GT(fx.port.load(), 0);
  test_run_session(fx);
}

TEST(Acp, RunSessionIsolation) {
  auto &fx = shared_acp_fixture();
  ASSERT_GT(fx.port.load(), 0);
  test_run_session_isolation(fx);
}

TEST(Acp, RunConcurrentSessionsNoBlocking) {
  auto &fx = shared_acp_fixture();
  ASSERT_GT(fx.port.load(), 0);
  test_run_concurrent_sessions_no_blocking(fx);
}

TEST(Acp, RunConcurrentSameSessionConflict) {
  auto &fx = shared_acp_fixture();
  ASSERT_GT(fx.port.load(), 0);
  test_run_concurrent_same_session_conflict(fx);
}

TEST(Acp, AgentTasks) {
  auto &fx = shared_acp_fixture();
  ASSERT_GT(fx.port.load(), 0);
  test_agent_tasks(fx);
}
