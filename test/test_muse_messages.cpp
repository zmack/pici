#include "core/providers/muse_messages.h"
#include "support/gtest_helpers.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <source_location>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

using namespace pi::core;

static Model make_model() {
  Model model;
  model.id = "muse-spark-1.1";
  model.name = "Muse Spark 1.1";
  model.api = "muse-messages";
  model.provider = "meta";
  model.base_url = "https://api.meta.ai";
  model.input_capabilities = {"text", "image"};
  model.max_tokens = 4096;
  model.thinking_level_map = {{"off", std::nullopt}, {"minimal", "low"},
                              {"low", "low"},        {"medium", "medium"},
                              {"high", "high"},      {"xhigh", "xhigh"}};
  return model;
}

class TestSchema final : public ToolSchema {
public:
  std::string serialize() const override {
    return R"({"type":"object","properties":{"city":{"type":"string"}}})";
  }

  std::map<std::string, std::string> to_definition() const override {
    return {{"type", "object"}};
  }
};

class TestTool final : public ToolDefinition {
public:
  std::string_view name() const override { return "lookup_city"; }
  std::string_view description() const override {
    return "Look up a city's information";
  }
  ToolSchema &schema() const override { return schema_; }
  std::shared_ptr<ToolResult> execute(std::string_view, std::string_view,
                                      std::stop_token = std::stop_token{},
                                      ToolUpdateCallback = {}) const override {
    return nullptr;
  }

private:
  mutable TestSchema schema_;
};

class MuseMessagesTest : public testing::Test {
protected:
  pi::test::TemporaryDirectory directory{"pici-muse"};
};

TEST_F(MuseMessagesTest, StreamDiagnosticsRedactsPayload) {
  { // stream diagnostics: privacy-safe JSONL
    const auto path = directory.path() / "stream-diagnostics-test.jsonl";
    {
      StreamDiagnostics diagnostics(path.string());
      diagnostics.record_transport_chunk(42);
      diagnostics.record_transport_line("event: content_block_delta\r");
      diagnostics.record_transport_line(
          R"(data: {"type":"content_block_delta","delta":{"text":"secret"}})");
      diagnostics.record_parser_event("text_delta", 6);
      diagnostics.record_renderer_event("text_delta", 6);
    }

    std::ifstream input(path);
    std::string trace((std::istreambuf_iterator<char>(input)),
                      std::istreambuf_iterator<char>());
    EXPECT_TRUE(!trace.empty());
    EXPECT_TRUE(trace.find("secret") == std::string::npos);
    EXPECT_TRUE(trace.find("transport") != std::string::npos);
    EXPECT_TRUE(trace.find("parser") != std::string::npos);
    EXPECT_TRUE(trace.find("renderer") != std::string::npos);
  }
}

TEST_F(MuseMessagesTest, BuildRequestRequiredShape) {
  { // build_request_json: required shape
    auto model = make_model();
    AgentContext context;
    context.system_prompt = "Be concise";
    UserMessage identity;
    identity.content.emplace_back(TextContent{
        .text = "[pici runtime context; not user-authored]\n"
                "mailbox agent_id=agt_a; session_id=sess_a; kind=root;\n"
                "Use agents_self when you need the authoritative structured "
                "identity."});
    context.messages.emplace_back(std::move(identity));
    UserMessage user;
    user.content.emplace_back(TextContent{.text = "hello"});
    context.messages.emplace_back(std::move(user));

    StreamOptions options;
    auto request =
        MuseMessagesClient::build_request_json(model, context, options);

    EXPECT_EQ(request["model"].get<std::string>(), "muse-spark-1.1");
    EXPECT_TRUE(request["messages"].is_array());
    EXPECT_EQ(request["messages"].size(), 2U);
    EXPECT_TRUE(
        request["messages"][0]["content"].get<std::string>().starts_with(
            "[pici runtime context; not user-authored]"));
    EXPECT_EQ(request["messages"][1]["content"].get<std::string>(), "hello");
    EXPECT_EQ(request["system"].get<std::string>(), "Be concise");
    EXPECT_EQ(request["max_tokens"].get<std::uint32_t>(), 4096U);
    EXPECT_TRUE(request["stream"].get<bool>());
    EXPECT_TRUE(!request.contains("thinking"));
    EXPECT_TRUE(!request.contains("tool_choice"));
    EXPECT_TRUE(!request.contains("top_p"));
    EXPECT_TRUE(!request.contains("stop_sequences"));
    EXPECT_TRUE(!request.contains("top_k"));
    EXPECT_TRUE(!request.contains("container"));
    EXPECT_TRUE(!request.contains("inference_geo"));
    EXPECT_TRUE(!request.contains("prompt_cache_key"));
  }
}

TEST_F(MuseMessagesTest, BuildRequestOptionsAndMetadata) {
  { // build_request_json: options and metadata
    auto model = make_model();
    AgentContext context;
    StreamOptions options;
    options.max_tokens = 123;
    options.temperature = 0.25;
    options.metadata = {{"request_id", "test-1"}, {"source", "unit"}};

    auto request =
        MuseMessagesClient::build_request_json(model, context, options);
    EXPECT_EQ(request["max_tokens"].get<std::uint32_t>(), 123U);
    EXPECT_EQ(request["temperature"].get<double>(), 0.25);
    EXPECT_EQ(request["metadata"]["request_id"].get<std::string>(), "test-1");
  }
}

TEST_F(MuseMessagesTest, BuildRequestThinkingEffort) {
  { // build_request_json: thinking effort mapping
    auto model = make_model();
    AgentContext context;

    StreamOptions minimal;
    minimal.reasoning = ThinkingLevel::minimal;
    auto minimal_request =
        MuseMessagesClient::build_request_json(model, context, minimal);
    EXPECT_EQ(minimal_request["thinking"]["type"].get<std::string>(),
              "adaptive");
    EXPECT_EQ(minimal_request["output_config"]["effort"].get<std::string>(),
              "low");

    StreamOptions high;
    high.reasoning = ThinkingLevel::high;
    auto high_request =
        MuseMessagesClient::build_request_json(model, context, high);
    EXPECT_EQ(high_request["thinking"]["type"].get<std::string>(), "adaptive");
    EXPECT_EQ(high_request["output_config"]["effort"].get<std::string>(),
              "high");

    auto off_request =
        MuseMessagesClient::build_request_json(model, context, {});
    EXPECT_TRUE(!off_request.contains("output_config"));
  }
}

TEST_F(MuseMessagesTest, BuildRequestRejectsInvalidMetadataAndMissingTokens) {
  { // build_request_json: invalid metadata and missing max_tokens
    auto model = make_model();
    AgentContext context;
    StreamOptions invalid_metadata;
    invalid_metadata.metadata = {{"request_id", 7}};
    bool metadata_threw = false;
    try {
      (void)MuseMessagesClient::build_request_json(model, context,
                                                   invalid_metadata);
    } catch (const std::invalid_argument &) {
      metadata_threw = true;
    }
    EXPECT_TRUE(metadata_threw);

    model.max_tokens = 0;
    bool max_tokens_threw = false;
    try {
      (void)MuseMessagesClient::build_request_json(model, context, {});
    } catch (const std::invalid_argument &) {
      max_tokens_threw = true;
    }
    EXPECT_TRUE(max_tokens_threw);
  }
}

TEST_F(MuseMessagesTest, BuildRequestImageConversion) {
  { // build_request_json: image conversion
    auto model = make_model();
    AgentContext context;
    UserMessage user;
    user.content = {TextContent{.text = "describe this"},
                    ImageContent{.data = "aGVsbG8=", .mime_type = "image/png"}};
    context.messages.emplace_back(std::move(user));

    auto request = MuseMessagesClient::build_request_json(model, context, {});
    const auto &content = request["messages"][0]["content"];
    EXPECT_TRUE(content.is_array());
    EXPECT_EQ(content[1]["type"].get<std::string>(), "image");
    EXPECT_EQ(content[1]["source"]["media_type"].get<std::string>(),
              "image/png");
    EXPECT_EQ(content[1]["source"]["data"].get<std::string>(), "aGVsbG8=");
  }
}

TEST_F(MuseMessagesTest, BuildRequestThinkingReplay) {
  { // build_request_json: thinking replay
    auto model = make_model();
    AgentContext context;
    AssistantMessage assistant;
    assistant.api = model.api;
    assistant.provider = model.provider;
    assistant.model = model.id;
    ThinkingContent visible{.thinking = "summary",
                            .thinking_signature = "visible-signature"};
    ThinkingContent redacted;
    redacted.redacted = true;
    redacted.thinking_signature = "encrypted-payload";
    assistant.content = {visible, redacted, TextContent{.text = "answer"}};
    context.messages.emplace_back(std::move(assistant));

    auto request = MuseMessagesClient::build_request_json(model, context, {});
    const auto &content = request["messages"][0]["content"];
    EXPECT_TRUE(content.is_array());
    EXPECT_EQ(content.size(), 3U);
    EXPECT_EQ(content[0]["type"].get<std::string>(), "thinking");
    EXPECT_EQ(content[0]["thinking"].get<std::string>(), "summary");
    EXPECT_EQ(content[0]["signature"].get<std::string>(), "visible-signature");
    EXPECT_EQ(content[1]["type"].get<std::string>(), "redacted_thinking");
    EXPECT_EQ(content[1]["data"].get<std::string>(), "encrypted-payload");
    EXPECT_EQ(content[2]["type"].get<std::string>(), "text");

    auto other_model = model;
    other_model.id = "other-model";
    auto downgraded =
        MuseMessagesClient::build_request_json(other_model, context, {});
    const auto &downgraded_content = downgraded["messages"][0]["content"];
    EXPECT_TRUE(downgraded_content.is_array());
    EXPECT_EQ(downgraded_content[0]["text"].get<std::string>(), "summary");
    EXPECT_EQ(downgraded_content[1]["text"].get<std::string>(), "answer");
  }
}

TEST_F(MuseMessagesTest, BuildRequestToolsAndToolResults) {
  { // build_request_json: tools and coalesced tool results
    auto model = make_model();
    AgentContext context;
    context.tools.push_back(std::make_shared<TestTool>());

    AssistantMessage assistant;
    assistant.api = model.api;
    assistant.provider = model.provider;
    assistant.model = model.id;
    assistant.content.emplace_back(
        ToolCall{.id = "call-1",
                 .name = "lookup_city",
                 .arguments = nlohmann::json{{"city", "Paris"}}});
    assistant.content.emplace_back(
        ToolCall{.id = "call-2",
                 .name = "lookup_city",
                 .arguments = nlohmann::json{{"city", "Berlin"}}});
    context.messages.emplace_back(std::move(assistant));

    ToolResultMessage first;
    first.tool_call_id = "call-1";
    first.tool_name = "lookup_city";
    first.content.emplace_back(TextContent{.text = "sunny"});
    context.messages.emplace_back(std::move(first));

    ToolResultMessage second;
    second.tool_call_id = "call-2";
    second.tool_name = "lookup_city";
    second.is_error = true;
    second.content.emplace_back(
        ImageContent{.data = "aGVsbG8=", .mime_type = "image/png"});
    context.messages.emplace_back(std::move(second));

    auto request = MuseMessagesClient::build_request_json(model, context, {});
    EXPECT_TRUE(request["tools"].is_array());
    EXPECT_EQ(request["tools"].size(), 1U);
    EXPECT_EQ(request["tools"][0]["type"].get<std::string>(), "custom");
    EXPECT_EQ(request["tools"][0]["name"].get<std::string>(), "lookup_city");
    EXPECT_EQ(request["tools"][0]["input_schema"]["type"].get<std::string>(),
              "object");
    EXPECT_TRUE(!request["tools"][0]["strict"].get<bool>());

    EXPECT_EQ(request["messages"].size(), 2U);
    const auto &assistant_content = request["messages"][0]["content"];
    EXPECT_EQ(assistant_content[0]["type"].get<std::string>(), "tool_use");
    EXPECT_EQ(assistant_content[0]["id"].get<std::string>(), "call-1");
    EXPECT_EQ(assistant_content[0]["input"]["city"].get<std::string>(),
              "Paris");

    const auto &tool_results = request["messages"][1]["content"];
    EXPECT_EQ(tool_results.size(), 2U);
    EXPECT_EQ(tool_results[0]["type"].get<std::string>(), "tool_result");
    EXPECT_EQ(tool_results[0]["content"].get<std::string>(), "sunny");
    EXPECT_EQ(tool_results[1]["tool_use_id"].get<std::string>(), "call-2");
    EXPECT_TRUE(tool_results[1]["is_error"].get<bool>());
    EXPECT_EQ(tool_results[1]["content"][0]["type"].get<std::string>(),
              "image");
  }
}

TEST_F(MuseMessagesTest, ParseSseThinkingTextUsage) {
  { // SSE parser: thinking, text, usage, and block indices
    auto result = std::make_shared<AssistantMessage>();
    std::vector<AssistantMessageEvent> events;
    MuseMessagesSseParser parser(
        result,
        [&](const AssistantMessageEvent &event) { events.push_back(event); });

    parser.feed_line("event: message_start\r");
    parser.feed_line(
        "data: "
        "{\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\",\"model\":"
        "\"muse-spark-1.1\",\"usage\":{\"input_tokens\":12}}}\r");
    parser.feed_line("event: content_block_start");
    parser.feed_line("data: "
                     "{\"type\":\"content_block_start\",\"index\":0,\"content_"
                     "block\":{\"type\":\"thinking\"}}");
    parser.feed_line("data: "
                     "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{"
                     "\"type\":\"thinking_delta\",\"thinking\":\"hidden\"}}");
    parser.feed_line("data: "
                     "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{"
                     "\"type\":\"signature_delta\",\"signature\":\"sig\"}}");
    parser.feed_line("data: {\"type\":\"content_block_stop\",\"index\":0}");
    parser.feed_line("data: "
                     "{\"type\":\"content_block_start\",\"index\":1,\"content_"
                     "block\":{\"type\":\"text\",\"text\":\"\"}}");
    parser.feed_line("data: "
                     "{\"type\":\"content_block_delta\",\"index\":1,\"delta\":{"
                     "\"type\":\"text_delta\",\"text\":\"Hello\"}}");
    parser.feed_line("data: "
                     "{\"type\":\"content_block_delta\",\"index\":1,\"delta\":{"
                     "\"type\":\"text_delta\",\"text\":\" world\"}}");
    parser.feed_line("data: {\"type\":\"content_block_stop\",\"index\":1}");
    parser.feed_line("data: "
                     "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":"
                     "\"end_turn\"},\"usage\":{\"output_tokens\":3}}");
    parser.feed_line("data: {\"type\":\"message_stop\"}");
    parser.finish();

    EXPECT_TRUE(!parser.error());
    EXPECT_EQ(result->response_id.value_or(""), "msg_1");
    EXPECT_EQ(result->content.size(), 2U);
    EXPECT_EQ(std::get<ThinkingContent>(result->content[0]).thinking, "hidden");
    EXPECT_EQ(std::get<ThinkingContent>(result->content[0])
                  .thinking_signature.value_or(""),
              "sig");
    EXPECT_EQ(std::get<TextContent>(result->content[1]).text, "Hello world");
    EXPECT_EQ(result->usage.input, 12U);
    EXPECT_EQ(result->usage.output, 3U);
    EXPECT_EQ(result->usage.total_tokens, 15U);
    EXPECT_EQ(result->stop_reason, StopReason::stop);
    EXPECT_TRUE(!events.empty());
    EXPECT_TRUE(
        std::holds_alternative<AssistantMessageDoneEvent>(events.back()));
  }
}

TEST_F(MuseMessagesTest, ParseSseRedactedThinking) {
  { // SSE parser: redacted thinking
    auto result = std::make_shared<AssistantMessage>();
    MuseMessagesSseParser parser(result);
    parser.feed_line("event: content_block_start\r\n");
    parser.feed_line(
        "data: "
        "{\"type\":\"content_block_start\",\"index\":0,\"content_block\":{"
        "\"type\":\"redacted_thinking\",\"data\":\"encrypted\"}}\r\n");
    parser.feed_line(
        "data: "
        "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
        "\"redacted_thinking_delta\",\"data\":\"-tail\"}}\r\n");
    parser.feed_line("data: {\"type\":\"content_block_stop\",\"index\":0}\r\n");
    parser.feed_line("data: {\"type\":\"message_stop\"}\r\n");
    parser.finish();

    EXPECT_TRUE(!parser.error());
    EXPECT_EQ(result->content.size(), 1U);
    const auto &thinking = std::get<ThinkingContent>(result->content[0]);
    EXPECT_TRUE(thinking.redacted);
    EXPECT_EQ(thinking.thinking, "");
    EXPECT_EQ(thinking.thinking_signature.value_or(""), "encrypted-tail");
  }
}

TEST_F(MuseMessagesTest, ParseSseToolUsePartialJson) {
  { // SSE parser: tool use and partial JSON
    auto result = std::make_shared<AssistantMessage>();
    std::vector<AssistantMessageEvent> events;
    MuseMessagesSseParser parser(
        result,
        [&](const AssistantMessageEvent &event) { events.push_back(event); });
    parser.feed_line("data: "
                     "{\"type\":\"content_block_start\",\"index\":2,\"content_"
                     "block\":{\"type\":\"tool_use\",\"id\":\"call-2\","
                     "\"name\":\"lookup_city\",\"input\":{}}}");
    parser.feed_line(
        "data: "
        "{\"type\":\"content_block_delta\",\"index\":2,\"delta\":{\"type\":"
        "\"input_json_delta\",\"partial_json\":\"{\\\"city\\\":\\\"Par\"}}");
    parser.feed_line(
        "data: "
        "{\"type\":\"content_block_delta\",\"index\":2,\"delta\":{\"type\":"
        "\"input_json_delta\",\"partial_json\":\"is\\\"}\"}}");
    parser.feed_line("data: {\"type\":\"content_block_stop\",\"index\":2}");
    parser.feed_line("data: "
                     "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":"
                     "\"tool_use\"}}");
    parser.feed_line("data: {\"type\":\"message_stop\"}");
    parser.finish();

    EXPECT_TRUE(!parser.error());
    EXPECT_EQ(result->content.size(), 1U);
    const auto &tool_call = std::get<ToolCall>(result->content[0]);
    EXPECT_EQ(tool_call.id, "call-2");
    EXPECT_EQ(tool_call.name, "lookup_city");
    EXPECT_EQ(tool_call.arguments["city"].get<std::string>(), "Paris");
    EXPECT_TRUE(tool_call.partial_json.empty());
    EXPECT_EQ(result->stop_reason, StopReason::tool_use);
    EXPECT_TRUE(
        std::holds_alternative<AssistantMessageToolCallStartEvent>(events[0]));
    EXPECT_TRUE(
        std::holds_alternative<AssistantMessageToolCallDeltaEvent>(events[1]));
    EXPECT_TRUE(
        std::holds_alternative<AssistantMessageToolCallDeltaEvent>(events[2]));
    EXPECT_TRUE(
        std::holds_alternative<AssistantMessageToolCallEndEvent>(events[3]));
    EXPECT_TRUE(
        std::holds_alternative<AssistantMessageDoneEvent>(events.back()));
  }
}

TEST_F(MuseMessagesTest, ParseSseTransportError) {
  { // SSE parser: transport error envelope
    auto result = std::make_shared<AssistantMessage>();
    MuseMessagesSseParser parser(result);
    parser.feed_line(R"({"error":{"message":"HTTP status 503: overloaded"}})");
    parser.finish();
    ASSERT_TRUE(parser.error().has_value());
    EXPECT_EQ(*parser.error(), "HTTP status 503: overloaded");
    EXPECT_EQ(result->error_message.value_or(""),
              "HTTP status 503: overloaded");
  }
}

TEST_F(MuseMessagesTest, ParseSseMuseErrorPreservesPrimary) {
  { // SSE parser: Muse error and fallback preservation
    auto result = std::make_shared<AssistantMessage>();
    MuseMessagesSseParser parser(result);
    parser.feed_line("event: error\r");
    parser.feed_line(
        R"(data: {"type":"error","error":{"type":"invalid_request_error","message":"bad input"}})");
    parser.feed_line(R"({"error":{"message":"HTTP status 400: fallback"}})");
    parser.finish();
    ASSERT_TRUE(parser.error().has_value());
    EXPECT_EQ(*parser.error(), "bad input");
    EXPECT_EQ(result->error_message.value_or(""), "bad input");
  }
}

TEST_F(MuseMessagesTest, ParseSseMalformedAndDuplicateFrames) {
  { // SSE parser: malformed and duplicate frames
    auto result = std::make_shared<AssistantMessage>();
    std::vector<AssistantMessageEvent> events;
    MuseMessagesSseParser parser(
        result,
        [&](const AssistantMessageEvent &event) { events.push_back(event); });
    parser.feed_line("event: unknown_event");
    parser.feed_line("data: {not valid json}");
    parser.feed_line("event: ping");
    parser.feed_line("data: {}");
    parser.feed_line("data: "
                     "{\"type\":\"content_block_start\",\"index\":3,\"content_"
                     "block\":{\"type\":\"text\"}}");
    parser.feed_line("data: "
                     "{\"type\":\"content_block_delta\",\"index\":3,\"delta\":{"
                     "\"type\":\"unknown_delta\",\"value\":\"ignored\"}}");
    parser.feed_line("data: "
                     "{\"type\":\"content_block_delta\",\"index\":3,\"delta\":{"
                     "\"type\":\"text_delta\",\"text\":\"ok\"}}");
    parser.feed_line("data: {\"type\":\"content_block_stop\",\"index\":3}");
    parser.feed_line("data: {\"type\":\"content_block_stop\",\"index\":3}");
    parser.feed_line("data: {\"type\":\"message_stop\"}");
    parser.finish();

    EXPECT_TRUE(!parser.error());
    EXPECT_EQ(std::get<TextContent>(result->content[0]).text, "ok");
    int text_end_events = 0;
    for (const auto &event : events) {
      if (std::holds_alternative<AssistantMessageTextEndEvent>(event))
        ++text_end_events;
    }
    EXPECT_EQ(text_end_events, 1);
    EXPECT_TRUE(
        std::holds_alternative<AssistantMessageDoneEvent>(events.back()));
  }
}

TEST_F(MuseMessagesTest, ParseSseRefusal) {
  { // SSE parser: refusal is an error
    auto result = std::make_shared<AssistantMessage>();
    MuseMessagesSseParser parser(result);
    parser.feed_line(
        "data: "
        "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"refusal\"}}");
    parser.finish();
    EXPECT_EQ(result->stop_reason, StopReason::error);
    ASSERT_TRUE(parser.error().has_value());
    EXPECT_EQ(*parser.error(), "Muse refused the request");
    EXPECT_EQ(result->error_message.value_or(""), "Muse refused the request");
  }
}

TEST_F(MuseMessagesTest, MapStopReasons) {
  { // map_stop_reason
    EXPECT_EQ(MuseMessagesClient::map_stop_reason("end_turn"),
              StopReason::stop);
    EXPECT_EQ(MuseMessagesClient::map_stop_reason("tool_use"),
              StopReason::tool_use);
    EXPECT_EQ(MuseMessagesClient::map_stop_reason("max_tokens"),
              StopReason::length);
    EXPECT_EQ(MuseMessagesClient::map_stop_reason("refusal"),
              StopReason::error);
    EXPECT_EQ(MuseMessagesClient::map_stop_reason("unknown"),
              StopReason::error);
  }
}
