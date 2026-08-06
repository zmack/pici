#include "core/providers/openai_codex_responses.h"

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
      .content = {pi::core::TextContent{.text = "hello"}}});
  pi::core::StreamOptions options;
  options.session_id = "session-1";
  options.reasoning = pi::core::ThinkingLevel::low;

  const auto request =
      pi::core::OpenAICodexResponsesClient::build_request_json(model, context,
                                                               options);
  CHECK_EQ(request.at("model"), nlohmann::json("gpt-5.3-codex"));
  CHECK_EQ(request.at("instructions"), nlohmann::json("Be concise."));
  CHECK_EQ(request.at("input")[0].at("content")[0].at("type"),
           nlohmann::json("input_text"));
  CHECK_EQ(request.at("prompt_cache_key"), nlohmann::json("session-1"));
  CHECK_EQ(request.at("reasoning").at("effort"), nlohmann::json("low"));
  CHECK(!request.contains("tools"));

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

  std::cout << "\nTests: " << tests::total << " total, " << tests::passed
            << " passed, " << tests::failed << " failed\n";
  return tests::failed == 0 ? 0 : 1;
}
