#include "core/providers/muse_messages.h"

#include <functional>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <source_location>
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

int main() {
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
    CHECK_EQ(request["thinking"]["type"].get<std::string>(), "adaptive");
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
    CHECK_EQ(minimal_request["output_config"]["effort"].get<std::string>(),
             "low");

    StreamOptions high;
    high.reasoning = ThinkingLevel::high;
    auto high_request =
        MuseMessagesClient::build_request_json(model, context, high);
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

  tests::run("SSE parser: transport error envelope", [] {
    auto result = std::make_shared<AssistantMessage>();
    MuseMessagesSseParser parser(result);
    parser.feed_line(
        R"({"error":{"message":"HTTP status 503: overloaded"}})");
    parser.finish();
    CHECK(parser.error().has_value());
    CHECK_EQ(*parser.error(), "HTTP status 503: overloaded");
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
