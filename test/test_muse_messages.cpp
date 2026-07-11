#include "core/providers/muse_messages.h"

#include <functional>
#include <fstream>
#include <filesystem>
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

namespace tests {

int passed{0};
int failed{0};
int total{0};
int current_failed{0};

bool check_impl(bool condition, std::string_view expression,
                std::source_location location =
                    std::source_location::current()) {
  if (condition) {
    return true;
  }
  ++current_failed;
  std::cerr << "  FAIL " << location.file_name() << ":" << location.line()
            << " - " << expression << "\n";
  return false;
}

#define CHECK(condition)                                                       \
  (::tests::check_impl(static_cast<bool>(condition), #condition,              \
                       std::source_location::current()))

#define CHECK_EQ(left, right)                                                  \
  (::tests::check_impl((left) == (right), #left " == " #right,                \
                       std::source_location::current()))

void run(std::string name, std::function<void()> test) {
  ++total;
  current_failed = 0;
  test();
  if (current_failed == 0) {
    ++passed;
    std::cout << "  PASS " << name << "\n";
  } else {
    ++failed;
    std::cout << "  FAIL " << name << "\n";
  }
}

} // namespace tests

static Model make_model() {
  Model model;
  model.id = "muse-spark-1.1";
  model.name = "Muse Spark 1.1";
  model.api = "muse-messages";
  model.provider = "meta";
  model.base_url = "https://api.meta.ai";
  model.input_capabilities = {"text", "image"};
  model.max_tokens = 4096;
  model.thinking_level_map = {
      {"off", std::nullopt}, {"minimal", "low"}, {"low", "low"},
      {"medium", "medium"}, {"high", "high"}, {"xhigh", "xhigh"}};
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
  std::shared_ptr<ToolResult>
  execute(std::string_view, std::string_view,
          std::stop_token = std::stop_token{},
          ToolUpdateCallback = {}) const override {
    return nullptr;
  }

private:
  mutable TestSchema schema_;
};

int main() {
  tests::run("stream diagnostics: privacy-safe JSONL", [] {
    const auto path = std::filesystem::temp_directory_path() /
                      "pici-stream-diagnostics-test.jsonl";
    std::filesystem::remove(path);
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
    CHECK(!trace.empty());
    CHECK(trace.find("secret") == std::string::npos);
    CHECK(trace.find("transport") != std::string::npos);
    CHECK(trace.find("parser") != std::string::npos);
    CHECK(trace.find("renderer") != std::string::npos);
    std::filesystem::remove(path);
  });

  tests::run("build_request_json: required shape", [] {
    auto model = make_model();
    AgentContext context;
    context.system_prompt = "Be concise";
    UserMessage user;
    user.content.emplace_back(TextContent{.text = "hello"});
    context.messages.emplace_back(std::move(user));

    StreamOptions options;
    auto request = MuseMessagesClient::build_request_json(model, context,
                                                           options);

    CHECK_EQ(request["model"].get<std::string>(), "muse-spark-1.1");
    CHECK(request["messages"].is_array());
    CHECK_EQ(request["messages"].size(), 1U);
    CHECK_EQ(request["messages"][0]["content"].get<std::string>(), "hello");
    CHECK_EQ(request["system"].get<std::string>(), "Be concise");
    CHECK_EQ(request["max_tokens"].get<std::uint32_t>(), 4096U);
    CHECK(request["stream"].get<bool>());
    CHECK(!request.contains("thinking"));
    CHECK(!request.contains("tool_choice"));
    CHECK(!request.contains("top_p"));
    CHECK(!request.contains("stop_sequences"));
    CHECK(!request.contains("top_k"));
    CHECK(!request.contains("container"));
    CHECK(!request.contains("inference_geo"));
  });

  tests::run("build_request_json: options and metadata", [] {
    auto model = make_model();
    AgentContext context;
    StreamOptions options;
    options.max_tokens = 123;
    options.temperature = 0.25;
    options.metadata = {{"request_id", "test-1"}, {"source", "unit"}};

    auto request = MuseMessagesClient::build_request_json(model, context,
                                                           options);
    CHECK_EQ(request["max_tokens"].get<std::uint32_t>(), 123U);
    CHECK_EQ(request["temperature"].get<double>(), 0.25);
    CHECK_EQ(request["metadata"]["request_id"].get<std::string>(), "test-1");
  });

  tests::run("build_request_json: thinking effort mapping", [] {
    auto model = make_model();
    AgentContext context;

    StreamOptions minimal;
    minimal.reasoning = ThinkingLevel::minimal;
    auto minimal_request =
        MuseMessagesClient::build_request_json(model, context, minimal);
    CHECK_EQ(minimal_request["thinking"]["type"].get<std::string>(),
             "adaptive");
    CHECK_EQ(minimal_request["output_config"]["effort"].get<std::string>(),
             "low");

    StreamOptions high;
    high.reasoning = ThinkingLevel::high;
    auto high_request =
        MuseMessagesClient::build_request_json(model, context, high);
    CHECK_EQ(high_request["thinking"]["type"].get<std::string>(), "adaptive");
    CHECK_EQ(high_request["output_config"]["effort"].get<std::string>(),
             "high");

    auto off_request =
        MuseMessagesClient::build_request_json(model, context, {});
    CHECK(!off_request.contains("output_config"));
  });

  tests::run("build_request_json: invalid metadata and missing max_tokens", [] {
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
    CHECK(metadata_threw);

    model.max_tokens = 0;
    bool max_tokens_threw = false;
    try {
      (void)MuseMessagesClient::build_request_json(model, context, {});
    } catch (const std::invalid_argument &) {
      max_tokens_threw = true;
    }
    CHECK(max_tokens_threw);
  });

  tests::run("build_request_json: image conversion", [] {
    auto model = make_model();
    AgentContext context;
    UserMessage user;
    user.content = {TextContent{.text = "describe this"},
                    ImageContent{.data = "aGVsbG8=", .mime_type = "image/png"}};
    context.messages.emplace_back(std::move(user));

    auto request = MuseMessagesClient::build_request_json(model, context, {});
    const auto &content = request["messages"][0]["content"];
    CHECK(content.is_array());
    CHECK_EQ(content[1]["type"].get<std::string>(), "image");
    CHECK_EQ(content[1]["source"]["media_type"].get<std::string>(),
             "image/png");
    CHECK_EQ(content[1]["source"]["data"].get<std::string>(), "aGVsbG8=");
  });

  tests::run("build_request_json: thinking replay", [] {
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
    CHECK(content.is_array());
    CHECK_EQ(content.size(), 3U);
    CHECK_EQ(content[0]["type"].get<std::string>(), "thinking");
    CHECK_EQ(content[0]["thinking"].get<std::string>(), "summary");
    CHECK_EQ(content[0]["signature"].get<std::string>(),
             "visible-signature");
    CHECK_EQ(content[1]["type"].get<std::string>(), "redacted_thinking");
    CHECK_EQ(content[1]["data"].get<std::string>(), "encrypted-payload");
    CHECK_EQ(content[2]["type"].get<std::string>(), "text");

    auto other_model = model;
    other_model.id = "other-model";
    auto downgraded =
        MuseMessagesClient::build_request_json(other_model, context, {});
    const auto &downgraded_content = downgraded["messages"][0]["content"];
    CHECK(downgraded_content.is_array());
    CHECK_EQ(downgraded_content[0]["text"].get<std::string>(), "summary");
    CHECK_EQ(downgraded_content[1]["text"].get<std::string>(), "answer");
  });

  tests::run("build_request_json: tools and coalesced tool results", [] {
    auto model = make_model();
    AgentContext context;
    context.tools.push_back(std::make_shared<TestTool>());

    AssistantMessage assistant;
    assistant.api = model.api;
    assistant.provider = model.provider;
    assistant.model = model.id;
    assistant.content.emplace_back(ToolCall{
        .id = "call-1",
        .name = "lookup_city",
        .arguments = nlohmann::json{{"city", "Paris"}}});
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
    CHECK(request["tools"].is_array());
    CHECK_EQ(request["tools"].size(), 1U);
    CHECK_EQ(request["tools"][0]["type"].get<std::string>(), "custom");
    CHECK_EQ(request["tools"][0]["name"].get<std::string>(), "lookup_city");
    CHECK_EQ(request["tools"][0]["input_schema"]["type"].get<std::string>(),
             "object");
    CHECK(!request["tools"][0]["strict"].get<bool>());

    CHECK_EQ(request["messages"].size(), 2U);
    const auto &assistant_content = request["messages"][0]["content"];
    CHECK_EQ(assistant_content[0]["type"].get<std::string>(), "tool_use");
    CHECK_EQ(assistant_content[0]["id"].get<std::string>(), "call-1");
    CHECK_EQ(assistant_content[0]["input"]["city"].get<std::string>(),
             "Paris");

    const auto &tool_results = request["messages"][1]["content"];
    CHECK_EQ(tool_results.size(), 2U);
    CHECK_EQ(tool_results[0]["type"].get<std::string>(), "tool_result");
    CHECK_EQ(tool_results[0]["content"].get<std::string>(), "sunny");
    CHECK_EQ(tool_results[1]["tool_use_id"].get<std::string>(), "call-2");
    CHECK(tool_results[1]["is_error"].get<bool>());
    CHECK_EQ(tool_results[1]["content"][0]["type"].get<std::string>(),
             "image");
  });

  tests::run("SSE parser: thinking, text, usage, and block indices", [] {
    auto result = std::make_shared<AssistantMessage>();
    std::vector<AssistantMessageEvent> events;
    MuseMessagesSseParser parser(result, [&](const AssistantMessageEvent &event) {
      events.push_back(event);
    });

    parser.feed_line("event: message_start\r");
    parser.feed_line(
        "data: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\",\"model\":\"muse-spark-1.1\",\"usage\":{\"input_tokens\":12}}}\r");
    parser.feed_line("event: content_block_start");
    parser.feed_line(
        "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"thinking\"}}");
    parser.feed_line(
        "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"hidden\"}}");
    parser.feed_line(
        "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"signature_delta\",\"signature\":\"sig\"}}");
    parser.feed_line(
        "data: {\"type\":\"content_block_stop\",\"index\":0}");
    parser.feed_line(
        "data: {\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}");
    parser.feed_line(
        "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_delta\",\"text\":\"Hello\"}}");
    parser.feed_line(
        "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_delta\",\"text\":\" world\"}}");
    parser.feed_line(
        "data: {\"type\":\"content_block_stop\",\"index\":1}");
    parser.feed_line(
        "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":{\"output_tokens\":3}}");
    parser.feed_line("data: {\"type\":\"message_stop\"}");
    parser.finish();

    CHECK(!parser.error());
    CHECK_EQ(result->response_id.value_or(""), "msg_1");
    CHECK_EQ(result->content.size(), 2U);
    CHECK_EQ(std::get<ThinkingContent>(result->content[0]).thinking, "hidden");
    CHECK_EQ(std::get<ThinkingContent>(result->content[0])
                 .thinking_signature.value_or(""),
             "sig");
    CHECK_EQ(std::get<TextContent>(result->content[1]).text, "Hello world");
    CHECK_EQ(result->usage.input, 12U);
    CHECK_EQ(result->usage.output, 3U);
    CHECK_EQ(result->usage.total_tokens, 15U);
    CHECK_EQ(result->stop_reason, StopReason::stop);
    CHECK(!events.empty());
    CHECK(std::holds_alternative<AssistantMessageDoneEvent>(events.back()));
  });

  tests::run("SSE parser: redacted thinking", [] {
    auto result = std::make_shared<AssistantMessage>();
    MuseMessagesSseParser parser(result);
    parser.feed_line("event: content_block_start\r\n");
    parser.feed_line(
        "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"redacted_thinking\",\"data\":\"encrypted\"}}\r\n");
    parser.feed_line(
        "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"redacted_thinking_delta\",\"data\":\"-tail\"}}\r\n");
    parser.feed_line(
        "data: {\"type\":\"content_block_stop\",\"index\":0}\r\n");
    parser.feed_line("data: {\"type\":\"message_stop\"}\r\n");
    parser.finish();

    CHECK(!parser.error());
    CHECK_EQ(result->content.size(), 1U);
    const auto &thinking = std::get<ThinkingContent>(result->content[0]);
    CHECK(thinking.redacted);
    CHECK_EQ(thinking.thinking, "");
    CHECK_EQ(thinking.thinking_signature.value_or(""), "encrypted-tail");
  });

  tests::run("SSE parser: tool use and partial JSON", [] {
    auto result = std::make_shared<AssistantMessage>();
    std::vector<AssistantMessageEvent> events;
    MuseMessagesSseParser parser(result, [&](const AssistantMessageEvent &event) {
      events.push_back(event);
    });
    parser.feed_line(
        "data: {\"type\":\"content_block_start\",\"index\":2,\"content_block\":{\"type\":\"tool_use\",\"id\":\"call-2\",\"name\":\"lookup_city\",\"input\":{}}}");
    parser.feed_line(
        "data: {\"type\":\"content_block_delta\",\"index\":2,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"city\\\":\\\"Par\"}}");
    parser.feed_line(
        "data: {\"type\":\"content_block_delta\",\"index\":2,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"is\\\"}\"}}");
    parser.feed_line(
        "data: {\"type\":\"content_block_stop\",\"index\":2}");
    parser.feed_line(
        "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\"}}");
    parser.feed_line("data: {\"type\":\"message_stop\"}");
    parser.finish();

    CHECK(!parser.error());
    CHECK_EQ(result->content.size(), 1U);
    const auto &tool_call = std::get<ToolCall>(result->content[0]);
    CHECK_EQ(tool_call.id, "call-2");
    CHECK_EQ(tool_call.name, "lookup_city");
    CHECK_EQ(tool_call.arguments["city"].get<std::string>(), "Paris");
    CHECK(tool_call.partial_json.empty());
    CHECK_EQ(result->stop_reason, StopReason::tool_use);
    CHECK(std::holds_alternative<AssistantMessageToolCallStartEvent>(events[0]));
    CHECK(std::holds_alternative<AssistantMessageToolCallDeltaEvent>(events[1]));
    CHECK(std::holds_alternative<AssistantMessageToolCallDeltaEvent>(events[2]));
    CHECK(std::holds_alternative<AssistantMessageToolCallEndEvent>(events[3]));
    CHECK(std::holds_alternative<AssistantMessageDoneEvent>(events.back()));
  });

  tests::run("SSE parser: transport error envelope", [] {
    auto result = std::make_shared<AssistantMessage>();
    MuseMessagesSseParser parser(result);
    parser.feed_line(
        R"({"error":{"message":"HTTP status 503: overloaded"}})");
    parser.finish();
    CHECK(parser.error().has_value());
    CHECK_EQ(*parser.error(), "HTTP status 503: overloaded");
    CHECK_EQ(result->error_message.value_or(""),
             "HTTP status 503: overloaded");
  });

  tests::run("SSE parser: Muse error and fallback preservation", [] {
    auto result = std::make_shared<AssistantMessage>();
    MuseMessagesSseParser parser(result);
    parser.feed_line("event: error\r");
    parser.feed_line(
        R"(data: {"type":"error","error":{"type":"invalid_request_error","message":"bad input"}})");
    parser.feed_line(
        R"({"error":{"message":"HTTP status 400: fallback"}})");
    parser.finish();
    CHECK(parser.error().has_value());
    CHECK_EQ(*parser.error(), "bad input");
    CHECK_EQ(result->error_message.value_or(""), "bad input");
  });

  tests::run("SSE parser: malformed and duplicate frames", [] {
    auto result = std::make_shared<AssistantMessage>();
    std::vector<AssistantMessageEvent> events;
    MuseMessagesSseParser parser(result, [&](const AssistantMessageEvent &event) {
      events.push_back(event);
    });
    parser.feed_line("event: unknown_event");
    parser.feed_line("data: {not valid json}");
    parser.feed_line("event: ping");
    parser.feed_line("data: {}");
    parser.feed_line(
        "data: {\"type\":\"content_block_start\",\"index\":3,\"content_block\":{\"type\":\"text\"}}");
    parser.feed_line(
        "data: {\"type\":\"content_block_delta\",\"index\":3,\"delta\":{\"type\":\"unknown_delta\",\"value\":\"ignored\"}}");
    parser.feed_line(
        "data: {\"type\":\"content_block_delta\",\"index\":3,\"delta\":{\"type\":\"text_delta\",\"text\":\"ok\"}}");
    parser.feed_line("data: {\"type\":\"content_block_stop\",\"index\":3}");
    parser.feed_line("data: {\"type\":\"content_block_stop\",\"index\":3}");
    parser.feed_line("data: {\"type\":\"message_stop\"}");
    parser.finish();

    CHECK(!parser.error());
    CHECK_EQ(std::get<TextContent>(result->content[0]).text, "ok");
    int text_end_events = 0;
    for (const auto &event : events) {
      if (std::holds_alternative<AssistantMessageTextEndEvent>(event))
        ++text_end_events;
    }
    CHECK_EQ(text_end_events, 1);
    CHECK(std::holds_alternative<AssistantMessageDoneEvent>(events.back()));
  });

  tests::run("SSE parser: refusal is an error", [] {
    auto result = std::make_shared<AssistantMessage>();
    MuseMessagesSseParser parser(result);
    parser.feed_line(
        "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"refusal\"}}");
    parser.finish();
    CHECK_EQ(result->stop_reason, StopReason::error);
    CHECK(parser.error().has_value());
    CHECK_EQ(*parser.error(), "Muse refused the request");
    CHECK_EQ(result->error_message.value_or(""), "Muse refused the request");
  });

  tests::run("map_stop_reason", [] {
    CHECK_EQ(MuseMessagesClient::map_stop_reason("end_turn"),
             StopReason::stop);
    CHECK_EQ(MuseMessagesClient::map_stop_reason("tool_use"),
             StopReason::tool_use);
    CHECK_EQ(MuseMessagesClient::map_stop_reason("max_tokens"),
             StopReason::length);
    CHECK_EQ(MuseMessagesClient::map_stop_reason("refusal"),
             StopReason::error);
    CHECK_EQ(MuseMessagesClient::map_stop_reason("unknown"),
             StopReason::error);
  });

  std::cout << "\n========================================\n"
            << "  Tests: " << tests::total << " total, " << tests::passed
            << " passed, " << tests::failed << " failed\n"
            << "========================================\n";
  return tests::failed == 0 ? 0 : 1;
}
