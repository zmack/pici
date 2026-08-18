#include "core/providers/openai_codex_responses.h"

#include "core/providers/faux.h"
#include "core/session/session_store.h"

#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>
#include <source_location>
#include <string_view>
#include <variant>

namespace tests {
int passed{0};
int failed{0};
int total{0};

bool check(bool condition, std::string_view expression,
           std::source_location location = std::source_location::current()) {
  ++total;
  if (condition) {
    ++passed;
    return true;
  }
  ++failed;
  std::cout << "  FAIL " << location.file_name() << ":" << location.line()
            << " — " << expression << "\n";
  return false;
}
} // namespace tests

#define CHECK(expression) tests::check((expression), #expression)
#define CHECK_EQ(left, right) tests::check((left) == (right), #left " == " #right)

int main() {
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

  const auto request =
      pi::core::OpenAICodexResponsesClient::build_request_json(model, context,
                                                               options);
  CHECK_EQ(request.at("model"), nlohmann::json("gpt-5.3-codex"));
  CHECK_EQ(request.at("instructions"), nlohmann::json("Be concise."));
  CHECK_EQ(request.at("input").size(), std::size_t{2});
  CHECK_EQ(request.at("input")[0].at("content")[0].at("type"),
           nlohmann::json("input_text"));
  CHECK(request.at("input")[0]
            .at("content")[0]
            .at("text")
            .get<std::string>()
            .starts_with("[pici runtime context; not user-authored]"));
  CHECK_EQ(request.at("prompt_cache_key"), nlohmann::json("session-1"));
  CHECK_EQ(request.at("reasoning").at("effort"), nlohmann::json("low"));
  CHECK(!request.contains("tools"));

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
    const auto compact_request = pi::core::OpenAICodexResponsesClient::
        build_request_json(model, compaction_context, compact_options);
    bool found_compaction_item = false;
    for (const auto &item : compact_request.at("input")) {
      if (item.value("type", "") == "compaction") {
        found_compaction_item = true;
        CHECK_EQ(item.at("encrypted_content"),
                 nlohmann::json("opaque-server-bytes"));
        CHECK_EQ(item.at("id"), nlohmann::json("cmp_1"));
      }
    }
    CHECK(found_compaction_item);
  }

  std::vector<pi::core::AssistantMessageEvent> events;
  pi::core::OpenAICodexResponsesParser parser(
      model, [&](const pi::core::AssistantMessageEvent &event) {
        events.push_back(event);
      });
  parser.feed_line("event: response.created");
  parser.feed_line("data: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_1\"}}");
  parser.feed_line("event: response.output_item.added");
  parser.feed_line("data: {\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"type\":\"message\",\"id\":\"msg_1\"}}");
  parser.feed_line("event: response.output_text.delta");
  parser.feed_line("data: {\"type\":\"response.output_text.delta\",\"output_index\":0,\"delta\":\"hi\"}");
  parser.feed_line("event: response.output_item.done");
  parser.feed_line("data: {\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{\"type\":\"message\",\"id\":\"msg_1\",\"content\":[{\"type\":\"output_text\",\"text\":\"hi there\"}]}}");
  parser.feed_line("event: response.completed");
  parser.feed_line("data: {\"type\":\"response.completed\",\"response\":{\"id\":\"resp_1\",\"status\":\"completed\",\"usage\":{\"input_tokens\":4,\"output_tokens\":2,\"total_tokens\":6}}}");
  parser.finish();

  CHECK(!parser.failed());
  CHECK(parser.terminal_seen());
  CHECK_EQ(parser.result()->response_id.value_or(""), std::string("resp_1"));
  CHECK_EQ(parser.result()->usage.total_tokens, std::uint64_t{6});
  CHECK_EQ(parser.result()->stop_reason, pi::core::StopReason::stop);
  CHECK_EQ(std::get<pi::core::TextContent>(parser.result()->content.front()).text,
           std::string("hi there"));
  CHECK(std::get<pi::core::TextContent>(parser.result()->content.front())
            .text_signature.has_value());

  pi::core::OpenAICodexResponsesParser incomplete(model, {});
  incomplete.feed_line("data: [DONE]");
  incomplete.finish();
  CHECK(incomplete.failed());

  // --- Compact endpoint: URL derivation ---
  CHECK_EQ(pi::core::OpenAICodexResponsesClient::compact_endpoint_url(
               "https://chatgpt.com/backend-api/codex"),
           std::string("https://chatgpt.com/backend-api/codex/responses/compact"));
  CHECK_EQ(pi::core::OpenAICodexResponsesClient::compact_endpoint_url(
               "https://chatgpt.com/backend-api/codex/responses"),
           std::string("https://chatgpt.com/backend-api/codex/responses/compact"));
  // Never double-append: an already-compact URL round-trips unchanged.
  CHECK_EQ(pi::core::OpenAICodexResponsesClient::compact_endpoint_url(
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
    CHECK_EQ(request.at("model"), nlohmann::json("gpt-5.3-codex"));
    CHECK_EQ(request.at("instructions"), nlohmann::json("Be concise."));
    CHECK_EQ(request.at("input").size(), std::size_t{1});
    CHECK_EQ(request.at("prompt_cache_key"), nlohmann::json("session-1"));
    CHECK_EQ(request.at("reasoning").at("effort"), nlohmann::json("low"));
    // Unlike a streaming request, the compact request must not carry
    // stream/store/include/tool_choice fields the endpoint does not
    // document.
    CHECK(!request.contains("stream"));
    CHECK(!request.contains("store"));
    CHECK(!request.contains("include"));
    CHECK(!request.contains("tool_choice"));
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
    CHECK_EQ(result.messages.size(), std::size_t{2});
    CHECK(std::holds_alternative<pi::core::UserMessage>(result.messages[0]));
    CHECK_EQ(std::get<pi::core::TextContent>(
                 std::get<pi::core::UserMessage>(result.messages[0]).content[0])
                 .text,
             std::string("please remember X"));
    CHECK(std::holds_alternative<pi::core::ContextCompactionMessage>(
        result.messages[1]));
    const auto &compaction_msg =
        std::get<pi::core::ContextCompactionMessage>(result.messages[1]);
    CHECK_EQ(compaction_msg.encrypted_content,
             std::string("SERVER_COMPACTED_SUMMARY"));
    CHECK_EQ(compaction_msg.item_id.value_or(""), std::string("cmp_1"));
    CHECK_EQ(compaction_msg.api, model.api);
    CHECK_EQ(compaction_msg.provider, model.provider);
    CHECK_EQ(compaction_msg.model, model.id);
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
    CHECK(threw);

    auto bad_compaction = nlohmann::json::parse(
        R"({"output": [{"type": "compaction", "id": "cmp_1"}]})");
    threw = false;
    try {
      (void)pi::core::parse_compact_response(model, bad_compaction);
    } catch (const std::exception &) {
      threw = true;
    }
    CHECK(threw);
  }

  // --- Compact endpoint: empty output is valid but produces no messages ---
  {
    auto empty = nlohmann::json::parse(R"({"output": []})");
    auto result = pi::core::parse_compact_response(model, empty);
    CHECK(result.messages.empty());
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
    CHECK_EQ(parsed.messages.size(), std::size_t{2});

    pi::core::FauxClient faux_client(
        /*scripts=*/{}, /*compact_results=*/{
            pi::core::CompactionResult{.messages = parsed.messages}});
    pi::core::AgentContext ctx;
    auto compact_result =
        faux_client.compact(model, ctx, pi::core::CompactionOptions{}, {});
    CHECK_EQ(compact_result.messages.size(), std::size_t{2});

    const auto dir = std::filesystem::temp_directory_path() /
                     "pici-compaction-phase2-gate";
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
    CHECK(reloaded.has_value());
    CHECK_EQ(reloaded->messages.size(), std::size_t{2});
    CHECK(std::holds_alternative<pi::core::ContextCompactionMessage>(
        reloaded->messages[1]));
    CHECK_EQ(std::get<pi::core::ContextCompactionMessage>(reloaded->messages[1])
                 .encrypted_content,
             std::string("GATE_TEST_OPAQUE_SUMMARY"));

    std::filesystem::remove_all(dir);
  }

  std::cout << "\nTests: " << tests::total << " total, " << tests::passed
            << " passed, " << tests::failed << " failed\n";
  return tests::failed == 0 ? 0 : 1;
}
