#include "core/providers/openai_codex_responses.h"

#include "core/agent.h"
#include "core/providers/faux.h"
#include "core/session/session_runtime.h"
#include "core/session/session_store.h"
#include "core/stream_diagnostics.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>
#include <source_location>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <variant>

#include <httplib.h>

TEST(OpenAICodexResponses, RequestParsingCompactionAndDiagnostics) {
  pi::core::Model model{.id = "gpt-5.3-codex",
                        .api = "openai-codex-responses",
                        .provider = "openai-codex",
                        .base_url = "https://chatgpt.com/backend-api",
                        .reasoning = true};
  pi::core::AgentContext context;
  context.system_prompt = "Be concise.";
  context.messages.emplace_back(pi::core::UserMessage{
      .content = {pi::core::TextContent{
          .text = "[pici runtime context; not user-authored]\n"
                  "mailbox agent_id=agt_a; session_id=sess_a; kind=root;\n"
                  "Use agents_self when you need the authoritative structured "
                  "identity."}}});
  context.messages.emplace_back(pi::core::UserMessage{
      .content = {pi::core::TextContent{.text = "hello"}}});
  pi::core::StreamOptions options;
  options.session_id = "session-1";
  options.reasoning = pi::core::ThinkingLevel::low;

  const auto request = pi::core::OpenAICodexResponsesClient::build_request_json(
      model, context, options);
  EXPECT_EQ(request.at("model"), nlohmann::json("gpt-5.3-codex"));
  EXPECT_EQ(request.at("instructions"), nlohmann::json("Be concise."));
  EXPECT_EQ(request.at("input").size(), std::size_t{2});
  EXPECT_EQ(request.at("input")[0].at("content")[0].at("type"),
            nlohmann::json("input_text"));
  EXPECT_TRUE(request.at("input")[0]
                  .at("content")[0]
                  .at("text")
                  .get<std::string>()
                  .starts_with("[pici runtime context; not user-authored]"));
  EXPECT_EQ(request.at("prompt_cache_key"), nlohmann::json("session-1"));
  EXPECT_EQ(request.at("reasoning").at("effort"), nlohmann::json("low"));
  EXPECT_TRUE(!request.contains("tools"));

  // A retained ContextCompactionMessage from this same api/provider/model
  // must round-trip into a `compaction` Responses input item carrying its
  // opaque encrypted_content unchanged.
  {
    pi::core::AgentContext compaction_context;
    compaction_context.model = model;
    compaction_context.messages.emplace_back(pi::core::UserMessage{
        .content = {pi::core::TextContent{.text = "hi"}}});
    pi::core::ContextCompactionMessage opaque;
    opaque.api = model.api;
    opaque.provider = model.provider;
    opaque.model = model.id;
    opaque.encrypted_content = "opaque-server-bytes";
    opaque.item_id = "cmp_1";
    compaction_context.messages.emplace_back(std::move(opaque));

    pi::core::StreamOptions compact_options;
    const auto compact_request =
        pi::core::OpenAICodexResponsesClient::build_request_json(
            model, compaction_context, compact_options);
    bool found_compaction_item = false;
    for (const auto &item : compact_request.at("input")) {
      if (item.value("type", "") == "compaction") {
        found_compaction_item = true;
        EXPECT_EQ(item.at("encrypted_content"),
                  nlohmann::json("opaque-server-bytes"));
        EXPECT_EQ(item.at("id"), nlohmann::json("cmp_1"));
      }
    }
    EXPECT_TRUE(found_compaction_item);
  }

  std::vector<pi::core::AssistantMessageEvent> events;
  pi::core::OpenAICodexResponsesParser parser(
      model, [&](const pi::core::AssistantMessageEvent &event) {
        events.push_back(event);
      });
  parser.feed_line("event: response.created");
  parser.feed_line(
      "data: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_1\"}}");
  parser.feed_line("event: response.output_item.added");
  parser.feed_line("data: "
                   "{\"type\":\"response.output_item.added\",\"output_index\":"
                   "0,\"item\":{\"type\":\"message\",\"id\":\"msg_1\"}}");
  parser.feed_line("event: response.output_text.delta");
  parser.feed_line("data: "
                   "{\"type\":\"response.output_text.delta\",\"output_index\":"
                   "0,\"delta\":\"hi\"}");
  parser.feed_line("event: response.output_item.done");
  parser.feed_line(
      "data: "
      "{\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{"
      "\"type\":\"message\",\"id\":\"msg_1\",\"content\":[{\"type\":\"output_"
      "text\",\"text\":\"hi there\"}]}}");
  parser.feed_line("event: response.completed");
  parser.feed_line("data: "
                   "{\"type\":\"response.completed\",\"response\":{\"id\":"
                   "\"resp_1\",\"status\":\"completed\",\"usage\":{\"input_"
                   "tokens\":4,\"output_tokens\":2,\"total_tokens\":6}}}");
  parser.finish();

  EXPECT_TRUE(!parser.failed());
  EXPECT_TRUE(parser.terminal_seen());
  EXPECT_EQ(parser.result()->response_id.value_or(""), std::string("resp_1"));
  EXPECT_EQ(parser.result()->usage.total_tokens, std::uint64_t{6});
  EXPECT_EQ(parser.result()->stop_reason, pi::core::StopReason::stop);
  EXPECT_EQ(
      std::get<pi::core::TextContent>(parser.result()->content.front()).text,
      std::string("hi there"));
  EXPECT_TRUE(std::get<pi::core::TextContent>(parser.result()->content.front())
                  .text_signature.has_value());

  pi::core::OpenAICodexResponsesParser incomplete(model, {});
  incomplete.feed_line("data: [DONE]");
  incomplete.finish();
  EXPECT_TRUE(incomplete.failed());

  // --- Compact endpoint: URL derivation ---
  EXPECT_EQ(
      pi::core::OpenAICodexResponsesClient::compact_endpoint_url(
          "https://chatgpt.com/backend-api/codex"),
      std::string("https://chatgpt.com/backend-api/codex/responses/compact"));
  EXPECT_EQ(
      pi::core::OpenAICodexResponsesClient::compact_endpoint_url(
          "https://chatgpt.com/backend-api/codex/responses"),
      std::string("https://chatgpt.com/backend-api/codex/responses/compact"));
  // Never double-append: an already-compact URL round-trips unchanged.
  EXPECT_EQ(
      pi::core::OpenAICodexResponsesClient::compact_endpoint_url(
          "https://chatgpt.com/backend-api/codex/responses/compact"),
      std::string("https://chatgpt.com/backend-api/codex/responses/compact"));

  // --- Compact endpoint: request shape ---
  {
    pi::core::AgentContext compact_context;
    compact_context.system_prompt = "Be concise.";
    compact_context.messages.emplace_back(pi::core::UserMessage{
        .content = {pi::core::TextContent{.text = "hello"}}});
    pi::core::CompactionOptions compact_options;
    compact_options.session_id = "session-1";
    compact_options.reasoning = pi::core::ThinkingLevel::low;

    const auto request =
        pi::core::OpenAICodexResponsesClient::build_compact_request_json(
            model, compact_context, compact_options);
    EXPECT_EQ(request.at("model"), nlohmann::json("gpt-5.3-codex"));
    EXPECT_EQ(request.at("instructions"), nlohmann::json("Be concise."));
    EXPECT_EQ(request.at("input").size(), std::size_t{1});
    EXPECT_EQ(request.at("prompt_cache_key"), nlohmann::json("session-1"));
    EXPECT_EQ(request.at("reasoning").at("effort"), nlohmann::json("low"));
    // Unlike a streaming request, the compact request must not carry
    // stream/store/include/tool_choice fields the endpoint does not
    // document.
    EXPECT_TRUE(!request.contains("stream"));
    EXPECT_TRUE(!request.contains("store"));
    EXPECT_TRUE(!request.contains("include"));
    EXPECT_TRUE(!request.contains("tool_choice"));
  }

  // --- Compact endpoint: response parsing ---
  {
    // Mirrors the reference implementation's mock: retained user/developer
    // input messages (developer is dropped, user is kept) plus a trailing
    // compaction item.
    auto response = nlohmann::json::parse(R"({
      "output": [
        {"type": "message", "role": "user",
         "content": [{"type": "input_text", "text": "please remember X"}]},
        {"type": "message", "role": "developer",
         "content": [{"type": "input_text", "text": "internal wrapper"}]},
        {"type": "function_call", "call_id": "call_1", "name": "bash",
         "arguments": "{}"},
        {"type": "compaction", "id": "cmp_1",
         "encrypted_content": "SERVER_COMPACTED_SUMMARY"}
      ]
    })");
    auto result = pi::core::parse_compact_response(model, response);
    EXPECT_EQ(result.messages.size(), std::size_t{2});
    EXPECT_TRUE(
        std::holds_alternative<pi::core::UserMessage>(result.messages[0]));
    EXPECT_EQ(
        std::get<pi::core::TextContent>(
            std::get<pi::core::UserMessage>(result.messages[0]).content[0])
            .text,
        std::string("please remember X"));
    EXPECT_TRUE(std::holds_alternative<pi::core::ContextCompactionMessage>(
        result.messages[1]));
    const auto &compaction_msg =
        std::get<pi::core::ContextCompactionMessage>(result.messages[1]);
    EXPECT_EQ(compaction_msg.encrypted_content,
              std::string("SERVER_COMPACTED_SUMMARY"));
    EXPECT_EQ(compaction_msg.item_id.value_or(""), std::string("cmp_1"));
    EXPECT_EQ(compaction_msg.api, model.api);
    EXPECT_EQ(compaction_msg.provider, model.provider);
    EXPECT_EQ(compaction_msg.model, model.id);
  }

  // --- Compact endpoint: malformed responses fail loudly ---
  {
    auto missing_output = nlohmann::json::parse(R"({"nothing": true})");
    bool threw = false;
    try {
      (void)pi::core::parse_compact_response(model, missing_output);
    } catch (const std::exception &) {
      threw = true;
    }
    EXPECT_TRUE(threw);

    auto bad_compaction = nlohmann::json::parse(
        R"({"output": [{"type": "compaction", "id": "cmp_1"}]})");
    threw = false;
    try {
      (void)pi::core::parse_compact_response(model, bad_compaction);
    } catch (const std::exception &) {
      threw = true;
    }
    EXPECT_TRUE(threw);
  }

  // --- Compact endpoint: empty output is valid but produces no messages ---
  {
    auto empty = nlohmann::json::parse(R"({"output": []})");
    auto result = pi::core::parse_compact_response(model, empty);
    EXPECT_TRUE(result.messages.empty());
  }

  // --- Phase 2 exit gate ---
  // Round-trips a canned /responses/compact fixture through:
  //   typed parsing (parse_compact_response)
  //     -> a fake LLMClient (FauxClient::compact) standing in for the real
  //        unary HTTP call
  //     -> durable journal write (SessionStore::append_compaction)
  //     -> reload (SessionStore::load)
  // This has no real CompactionManager yet (that is Phase 3): the
  // "filtering" stage the plan anticipates is Phase 3's
  // filter_compacted_history, which does not exist yet, so this proves the
  // seam up through what Phases 1-2 actually built. Phase 3 should extend
  // this exact fixture with a filtering step once that function lands.
  {
    auto fixture = nlohmann::json::parse(R"({
      "id": "resp_compact_1",
      "output": [
        {"type": "message", "role": "user",
         "content": [{"type": "input_text", "text": "retained user context"}]},
        {"type": "compaction", "id": "cmp_gate",
         "encrypted_content": "GATE_TEST_OPAQUE_SUMMARY"}
      ],
      "usage": {"input_tokens": 10, "output_tokens": 0, "total_tokens": 10}
    })");
    auto parsed = pi::core::parse_compact_response(model, fixture);
    EXPECT_EQ(parsed.messages.size(), std::size_t{2});

    pi::core::FauxClient faux_client(
        /*scripts=*/{}, /*compact_results=*/{
            pi::core::CompactionResult{.messages = parsed.messages}});
    pi::core::AgentContext ctx;
    auto compact_result =
        faux_client.compact(model, ctx, pi::core::CompactionOptions{}, {});
    EXPECT_EQ(compact_result.messages.size(), std::size_t{2});

    const auto dir =
        std::filesystem::temp_directory_path() / "pici-compaction-phase2-gate";
    std::filesystem::remove_all(dir);
    pi::core::SessionStore store(dir);
    pi::core::SessionHeader header{.id = "gate-session"};
    const auto session_id = store.create(header);

    pi::core::SessionCompactionRecord record;
    record.messages = compact_result.messages;
    record.provider = model.provider;
    record.model = model.id;
    record.summary = "server";
    store.append_compaction(session_id, record);

    auto reloaded = store.load(session_id);
    EXPECT_TRUE(reloaded.has_value());
    EXPECT_EQ(reloaded->messages.size(), std::size_t{2});
    EXPECT_TRUE(std::holds_alternative<pi::core::ContextCompactionMessage>(
        reloaded->messages[1]));
    EXPECT_EQ(
        std::get<pi::core::ContextCompactionMessage>(reloaded->messages[1])
            .encrypted_content,
        std::string("GATE_TEST_OPAQUE_SUMMARY"));

    std::filesystem::remove_all(dir);
  }

  // --- Compact endpoint: timeout multiplier (Phase 6 hardening) ---
  // compact_request_timeout_ms is the only place kCompactTimeoutIdleMultiplier
  // (4x, matching Codex's COMPACT_REQUEST_TIMEOUT_IDLE_MULTIPLIER) is applied;
  // exercise the formula directly rather than only observing it indirectly
  // through a real timed-out HTTP call, which would only prove "some timeout
  // fired," not that the 4x scaling and its saturating cast are correct.
  {
    EXPECT_EQ(pi::core::OpenAICodexResponsesClient::compact_request_timeout_ms(
                  std::optional<std::uint32_t>{1000}),
              std::uint32_t{4000});
    // Unset configured timeout falls back to HttpClient's own 600s default
    // before scaling, matching the documented kDefaultRequestTimeoutMs.
    EXPECT_EQ(pi::core::OpenAICodexResponsesClient::compact_request_timeout_ms(
                  std::nullopt),
              std::uint32_t{2400000});
    // Saturates at UINT32_MAX rather than overflowing/wrapping.
    EXPECT_EQ(pi::core::OpenAICodexResponsesClient::compact_request_timeout_ms(
                  std::optional<std::uint32_t>{
                      std::numeric_limits<std::uint32_t>::max()}),
              std::numeric_limits<std::uint32_t>::max());
  }

  // --- Compact endpoint: oversized response content round-trips intact ---
  // "Oversized" per the plan's test matrix is a response whose retained
  // opaque payload or item count is large, not a transport-layer size cap
  // (post_authenticated enforces none) — the requirement is that parsing
  // does not truncate, corrupt, or choke on it.
  {
    const std::string huge_payload(std::size_t{4} * 1024 * 1024, 'X'); // 4 MiB
    std::ostringstream body;
    body << R"({"output":[)";
    for (int i = 0; i < 500; ++i) {
      if (i > 0)
        body << ",";
      body << R"({"type":"message","role":"user",)"
           << R"("content":[{"type":"input_text","text":"item )" << i
           << R"("}]})";
    }
    body << R"(,{"type":"compaction","id":"cmp_huge","encrypted_content":")"
         << huge_payload << R"("}]})";
    auto oversized = nlohmann::json::parse(body.str());

    auto result = pi::core::parse_compact_response(model, oversized);
    EXPECT_EQ(result.messages.size(), std::size_t{501});
    EXPECT_TRUE(std::holds_alternative<pi::core::ContextCompactionMessage>(
        result.messages.back()));
    EXPECT_EQ(
        std::get<pi::core::ContextCompactionMessage>(result.messages.back())
            .encrypted_content.size(),
        huge_payload.size());
    EXPECT_EQ(
        std::get<pi::core::ContextCompactionMessage>(result.messages.back())
            .encrypted_content,
        huge_payload);
  }

  // --- Compact endpoint: real HTTP round trip (Phase 6 hardening) ---
  // Every other test above builds/parses JSON directly, never exercising
  // OpenAICodexResponsesClient::compact() itself over a real socket. Spin up
  // a loopback httplib::Server (the same library pici's own OAuth callback
  // server and ACP server use) so status codes, malformed bodies, and
  // no-response/timeout behavior are exercised through the real HTTP layer,
  // not simulated by constructing a CompactionResult by hand.
  {
    using Clock = std::chrono::steady_clock;

    // --- success ---
    {
      httplib::Server server;
      server.Post(
          "/codex/responses/compact",
          [](const httplib::Request &request, httplib::Response &response) {
            EXPECT_TRUE(request.get_header_value("OpenAI-Beta") ==
                        "responses=experimental");
            response.set_content(
                R"({"id":"resp_1","output":[)"
                R"({"type":"compaction","id":"cmp_1",)"
                R"("encrypted_content":"OPAQUE_SECRET_BLOB"}],)"
                R"("usage":{"input_tokens":5,"output_tokens":1,)"
                R"("total_tokens":6}})",
                "application/json");
          });
      const auto port = server.bind_to_any_port("127.0.0.1");
      EXPECT_TRUE(port > 0);
      std::thread listener([&server] { server.listen_after_bind(); });

      auto real_model = model;
      real_model.base_url = "http://127.0.0.1:" + std::to_string(port);
      pi::core::OpenAICodexResponsesClient client;
      pi::core::AgentContext ctx;
      ctx.messages.emplace_back(pi::core::UserMessage{
          .content = {pi::core::TextContent{.text = "hello"}}});

      const auto trace_path = (std::filesystem::temp_directory_path() /
                               "pici-compact-diag-success.jsonl")
                                  .string();
      {
        auto diagnostics =
            std::make_shared<pi::core::StreamDiagnostics>(trace_path);
        pi::core::CompactionOptions options;
        options.diagnostics = diagnostics;
        auto result = client.compact(real_model, ctx, options, {});
        EXPECT_TRUE(result.supported);
        EXPECT_TRUE(!result.error_message.has_value());
        EXPECT_EQ(result.messages.size(), std::size_t{1});
        EXPECT_EQ(result.response_id.value_or(""), std::string("resp_1"));
        EXPECT_EQ(result.usage.input, std::uint64_t{5});
      }
      // diagnostics destructor flushes; read the trace back.
      {
        std::ifstream trace(trace_path);
        std::ostringstream contents;
        contents << trace.rdbuf();
        const auto text = contents.str();
        EXPECT_TRUE(text.find("\"stage\":\"compact\"") != std::string::npos);
        EXPECT_TRUE(text.find("request_sent") != std::string::npos);
        EXPECT_TRUE(text.find("parsed_ok") != std::string::npos);
        // No-secret/no-payload requirement: the opaque content and request
        // text must never appear in the trace, only counts/labels.
        EXPECT_TRUE(text.find("OPAQUE_SECRET_BLOB") == std::string::npos);
        EXPECT_TRUE(text.find("hello") == std::string::npos);
      }
      std::filesystem::remove(trace_path);

      server.stop();
      listener.join();
    }

    // --- malformed JSON body over the wire ---
    {
      httplib::Server server;
      server.Post("/codex/responses/compact",
                  [](const httplib::Request &, httplib::Response &response) {
                    response.set_content("{not valid json", "application/json");
                  });
      const auto port = server.bind_to_any_port("127.0.0.1");
      EXPECT_TRUE(port > 0);
      std::thread listener([&server] { server.listen_after_bind(); });

      auto real_model = model;
      real_model.base_url = "http://127.0.0.1:" + std::to_string(port);
      pi::core::OpenAICodexResponsesClient client;
      pi::core::AgentContext ctx;
      auto result = client.compact(real_model, ctx, {}, {});
      EXPECT_TRUE(result.supported);
      EXPECT_TRUE(result.error_message.has_value());
      EXPECT_TRUE(result.error_message->find("not valid JSON") !=
                  std::string::npos);
      EXPECT_TRUE(!result.http_status.has_value());

      server.stop();
      listener.join();
    }

    // --- distinct non-2xx statuses ---
    for (const int status : {401, 429, 500}) {
      httplib::Server server;
      server.Post(
          "/codex/responses/compact",
          [status](const httplib::Request &, httplib::Response &response) {
            response.status = status;
            response.set_content(R"({"error":{"message":"nope"}})",
                                 "application/json");
          });
      const auto port = server.bind_to_any_port("127.0.0.1");
      EXPECT_TRUE(port > 0);
      std::thread listener([&server] { server.listen_after_bind(); });

      auto real_model = model;
      real_model.base_url = "http://127.0.0.1:" + std::to_string(port);
      pi::core::OpenAICodexResponsesClient client;
      pi::core::AgentContext ctx;
      auto result = client.compact(real_model, ctx, {}, {});
      EXPECT_TRUE(result.supported);
      EXPECT_TRUE(result.error_message.has_value());
      EXPECT_EQ(result.http_status.value_or(0), status);

      server.stop();
      listener.join();
    }

    // --- timeout / no-response ---
    {
      std::atomic<bool> release{false};
      httplib::Server server;
      server.Post(
          "/codex/responses/compact",
          [&release](const httplib::Request &, httplib::Response &response) {
            // Block well past the client's configured timeout so the
            // request aborts with no response, rather than racing a
            // sleep duration against wall-clock scheduling jitter.
            while (!release.load())
              std::this_thread::sleep_for(std::chrono::milliseconds(10));
            response.set_content(R"({"output":[]})", "application/json");
          });
      const auto port = server.bind_to_any_port("127.0.0.1");
      EXPECT_TRUE(port > 0);
      std::thread listener([&server] { server.listen_after_bind(); });

      auto real_model = model;
      real_model.base_url = "http://127.0.0.1:" + std::to_string(port);
      pi::core::OpenAICodexResponsesClient client;
      pi::core::AgentContext ctx;
      pi::core::CompactionOptions options;
      // compact_request_timeout_ms scales this by 4x; 100ms configured ->
      // 400ms actual, comfortably shorter than the handler's indefinite
      // block, so this is deterministic rather than timing-sensitive.
      options.timeout_ms = 100;
      const auto started = Clock::now();
      auto result = client.compact(real_model, ctx, options, {});
      const auto elapsed = Clock::now() - started;
      EXPECT_TRUE(result.supported);
      EXPECT_TRUE(result.error_message.has_value());
      EXPECT_TRUE(!result.cancelled);
      EXPECT_TRUE(elapsed < std::chrono::seconds(5));

      release.store(true);
      server.stop();
      listener.join();
    }

    // --- acceptance criterion #1: a real openai-codex-responses session
    // manually compacts through the dedicated endpoint, end to end ---
    // Every other manual-compaction test in this suite (test_compaction.cpp,
    // test_rpc_mode.cpp) drives SessionRuntime::compact_active_session()
    // through a FauxClient registered under a synthetic api id. This is the
    // one test that registers the *real* OpenAICodexResponsesClient under
    // its real "openai-codex-responses" api id, points it at a local
    // /codex/responses/compact mock, and drives the full manual transaction
    // (Agent::compact -> run_compaction -> journal write -> in-memory
    // install) through SessionRuntime, then reloads the session to confirm
    // durability — proving the dedicated endpoint is reachable through the
    // real provider identity, not just through a test double standing in
    // for "some provider."
    {
      httplib::Server server;
      server.Post("/codex/responses/compact", [](const httplib::Request &,
                                                 httplib::Response &response) {
        response.set_content(
            R"({"id":"resp_e2e","output":[)"
            R"({"type":"message","role":"user",)"
            R"("content":[{"type":"input_text","text":"retained"}]},)"
            R"({"type":"compaction","id":"cmp_e2e",)"
            R"("encrypted_content":"E2E_OPAQUE_SUMMARY"}],)"
            R"("usage":{"input_tokens":3,"output_tokens":0,)"
            R"("total_tokens":3}})",
            "application/json");
      });
      const auto port = server.bind_to_any_port("127.0.0.1");
      EXPECT_TRUE(port > 0);
      std::thread listener([&server] { server.listen_after_bind(); });

      pi::core::register_openai_codex_responses_client();

      const auto session_dir = std::filesystem::temp_directory_path() /
                               "pici-compact-e2e-codex-session";
      std::filesystem::remove_all(session_dir);
      auto store = std::make_shared<pi::core::SessionStore>(session_dir);

      pi::core::SessionRuntime::Config config;
      config.agent_options.model.id = "gpt-5.3-codex";
      config.agent_options.model.api = "openai-codex-responses";
      config.agent_options.model.provider = "openai-codex";
      config.agent_options.model.base_url =
          "http://127.0.0.1:" + std::to_string(port);
      config.session_store = store;
      pi::core::SessionRuntime session(std::move(config));

      pi::core::SessionHeader header;
      const auto session_id = session.create_session(header);
      session.agent().state().append_message(
          pi::core::Message{pi::core::UserMessage{
              .content = {pi::core::TextContent{.text = "pre-compaction"}}}});

      auto result = session.compact_active_session();
      EXPECT_TRUE(result.success);
      EXPECT_TRUE(!result.unsupported);
      EXPECT_TRUE(!result.error.has_value());
      EXPECT_EQ(result.retained_message_count, std::size_t{2});

      // Installed in memory immediately.
      EXPECT_EQ(session.agent().state().messages().size(), std::size_t{2});
      EXPECT_TRUE(std::holds_alternative<pi::core::ContextCompactionMessage>(
          session.agent().state().messages()[1]));

      // Survives save/reload: the replacement transcript and opaque item are
      // what a follow-up request would see, not the pre-compaction message.
      auto reloaded = store->load(session_id);
      EXPECT_TRUE(reloaded.has_value());
      if (reloaded) {
        EXPECT_EQ(reloaded->messages.size(), std::size_t{2});
        EXPECT_TRUE(std::holds_alternative<pi::core::ContextCompactionMessage>(
            reloaded->messages[1]));
        EXPECT_EQ(
            std::get<pi::core::ContextCompactionMessage>(reloaded->messages[1])
                .encrypted_content,
            std::string("E2E_OPAQUE_SUMMARY"));
      }

      store.reset();
      std::filesystem::remove_all(session_dir);
      server.stop();
      listener.join();
    }
  }
}
