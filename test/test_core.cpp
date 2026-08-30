#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/agent_state.h"
#include "core/event_json.h"

#include "core/event_types.h"
#include "core/message_types.h"
#include "core/models.h"
#include "core/providers/transform_messages.h"
#include "core/session/session_store.h"
#include "core/stream.h"
#include "core/stream_renderer.h"
#include "support/gtest_helpers.h"
#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace pi::core;

namespace {
Message make_user_message(std::string text) {
  UserMessage message;
  message.content.emplace_back(TextContent{.text = std::move(text)});
  return Message{std::move(message)};
}
} // namespace

// ─── StopReason tests ─────────────────────────────────────────────────────

TEST(StopReason, Conversion) {

  EXPECT_EQ(stop_reason_to_string(StopReason::stop), "stop");
  EXPECT_EQ(stop_reason_to_string(StopReason::length), "length");
  EXPECT_EQ(stop_reason_to_string(StopReason::tool_use), "toolUse");
  EXPECT_EQ(stop_reason_to_string(StopReason::error), "error");
  EXPECT_EQ(stop_reason_to_string(StopReason::aborted), "aborted");

  EXPECT_EQ(stop_reason_from_string("stop"), StopReason::stop);
  EXPECT_EQ(stop_reason_from_string("length"), StopReason::length);
  EXPECT_EQ(stop_reason_from_string("toolUse"), StopReason::tool_use);
  EXPECT_EQ(stop_reason_from_string("error"), StopReason::error);
  EXPECT_EQ(stop_reason_from_string("aborted"), StopReason::aborted);
}

TEST(StopReason, Output) {

  std::ostringstream oss;
  oss << StopReason::stop;
  EXPECT_EQ(oss.str(), "stop");
}

// ─── ThinkingLevel tests ──────────────────────────────────────────────────

TEST(ThinkingLevel, Conversion) {

  EXPECT_EQ(thinking_level_to_string(ThinkingLevel::off), "off");
  EXPECT_EQ(thinking_level_to_string(ThinkingLevel::minimal), "minimal");
  EXPECT_EQ(thinking_level_to_string(ThinkingLevel::low), "low");
  EXPECT_EQ(thinking_level_to_string(ThinkingLevel::medium), "medium");
  EXPECT_EQ(thinking_level_to_string(ThinkingLevel::high), "high");
  EXPECT_EQ(thinking_level_to_string(ThinkingLevel::xhigh), "xhigh");

  EXPECT_EQ(thinking_level_from_string("off"), ThinkingLevel::off);
  EXPECT_EQ(thinking_level_from_string("minimal"), ThinkingLevel::minimal);
  EXPECT_EQ(thinking_level_from_string("low"), ThinkingLevel::low);
  EXPECT_EQ(thinking_level_from_string("medium"), ThinkingLevel::medium);
  EXPECT_EQ(thinking_level_from_string("high"), ThinkingLevel::high);
  EXPECT_EQ(thinking_level_from_string("xhigh"), ThinkingLevel::xhigh);
  EXPECT_EQ(thinking_level_from_string("unknown"), ThinkingLevel::off);
}

// ─── Message JSON tests ───────────────────────────────────────────────────

TEST(Message, UserMessageJSONRoundTrip) {

  UserMessage msg;
  msg.timestamp = 1234567890;
  TextContent tc;
  tc.text = "Hello, world!";
  msg.content.push_back(std::move(tc));

  std::string json_str = json::to_json(msg);
  auto parsed = json::from_json(json_str);

  ASSERT_TRUE(parsed.has_value());
  ASSERT_THAT(parsed,
              testing::Optional(testing::VariantWith<UserMessage>(testing::_)));

  auto &parsed_msg = std::get<UserMessage>(*parsed);
  EXPECT_EQ(parsed_msg.timestamp, msg.timestamp);
  EXPECT_EQ(parsed_msg.content.size(), std::size_t(1));
  EXPECT_EQ(std::get<TextContent>(parsed_msg.content[0]).text, "Hello, world!");
}

TEST(Message, AssistantMessageJSONRoundTrip) {

  AssistantMessage msg;
  msg.api = "openai-completions";
  msg.provider = "openai";
  msg.model = "gpt-4";
  msg.stop_reason = StopReason::stop;
  msg.timestamp = 9876543210;

  TextContent tc;
  tc.text = "I can help you with that.";
  msg.content.push_back(std::move(tc));

  msg.usage.input = 100;
  msg.usage.output = 50;
  msg.usage.cache_read = 80;
  msg.usage.cache_write = 20;
  msg.usage.total_tokens = 150;

  std::string json_str = json::to_json(msg);
  auto parsed = json::from_json(json_str);

  ASSERT_TRUE(parsed.has_value());
  ASSERT_THAT(parsed, testing::Optional(
                          testing::VariantWith<AssistantMessage>(testing::_)));

  auto &parsed_msg = std::get<AssistantMessage>(*parsed);
  EXPECT_EQ(parsed_msg.api, "openai-completions");
  EXPECT_EQ(parsed_msg.provider, "openai");
  EXPECT_EQ(parsed_msg.model, "gpt-4");
  EXPECT_EQ(parsed_msg.stop_reason, StopReason::stop);
  EXPECT_EQ(parsed_msg.usage.input, 100ULL);
  EXPECT_EQ(parsed_msg.usage.output, 50ULL);
  EXPECT_EQ(parsed_msg.usage.cache_read, 80ULL);
  EXPECT_EQ(parsed_msg.usage.cache_write, 20ULL);
  EXPECT_EQ(parsed_msg.usage.total_tokens, 150ULL);
}

TEST(Message, ToolResultMessageJSONRoundTrip) {

  ToolResultMessage msg;
  msg.tool_call_id = "call_abc123";
  msg.tool_name = "bash";
  msg.is_error = false;
  msg.timestamp = 1111111111;

  TextContent tc;
  tc.text = "Command executed successfully";
  msg.content.push_back(std::move(tc));

  std::string json_str = json::to_json(msg);
  auto parsed = json::from_json(json_str);

  ASSERT_TRUE(parsed.has_value());
  ASSERT_THAT(parsed, testing::Optional(
                          testing::VariantWith<ToolResultMessage>(testing::_)));

  auto &parsed_msg = std::get<ToolResultMessage>(*parsed);
  EXPECT_EQ(parsed_msg.tool_call_id, "call_abc123");
  EXPECT_EQ(parsed_msg.tool_name, "bash");
  EXPECT_FALSE(parsed_msg.is_error);
}

TEST(Message, ContextCompactionMessageJSONRoundTrip) {

  ContextCompactionMessage msg;
  msg.api = "openai-codex-responses";
  msg.provider = "openai-codex";
  msg.model = "gpt-5.3-codex";
  msg.encrypted_content = "opaque-server-bytes-not-plaintext";
  msg.item_id = "cmp_123";
  msg.response_id = "resp_456";
  msg.timestamp = 42;

  std::string json_str = json::to_json(msg);
  // The opaque payload must round-trip byte-for-byte and must never be
  // silently dropped, truncated, or reinterpreted.
  EXPECT_THAT(json_str,
              testing::HasSubstr("opaque-server-bytes-not-plaintext"));

  auto parsed = json::from_json(json_str);
  ASSERT_TRUE(parsed.has_value());
  ASSERT_THAT(parsed,
              testing::Optional(
                  testing::VariantWith<ContextCompactionMessage>(testing::_)));

  auto &parsed_msg = std::get<ContextCompactionMessage>(*parsed);
  EXPECT_EQ(parsed_msg.api, "openai-codex-responses");
  EXPECT_EQ(parsed_msg.provider, "openai-codex");
  EXPECT_EQ(parsed_msg.model, "gpt-5.3-codex");
  EXPECT_EQ(parsed_msg.encrypted_content, "opaque-server-bytes-not-plaintext");
  ASSERT_TRUE(parsed_msg.item_id.has_value());
  EXPECT_EQ(*parsed_msg.item_id, "cmp_123");
  ASSERT_TRUE(parsed_msg.response_id.has_value());
  EXPECT_EQ(*parsed_msg.response_id, "resp_456");
  EXPECT_EQ(parsed_msg.timestamp, std::int64_t(42));

  // Also verify the compact jsonl form used for session persistence.
  auto line = json::to_jsonl_line(msg);
  auto parsed_line = json::from_json(line);
  ASSERT_TRUE(parsed_line.has_value());
  ASSERT_THAT(parsed_line,
              testing::Optional(
                  testing::VariantWith<ContextCompactionMessage>(testing::_)));
}

TEST(TransformMessages, DropsToolResultMessageWithNoMatchingToolCall) {

  // Simulates the state after compaction filtering has dropped the
  // AssistantMessage that owned this tool call: the orphaned result
  // must not survive, since sending it produces a bare
  // function_call_output with no matching function_call (rejected by
  // the Responses API).
  std::vector<Message> messages;
  messages.push_back(make_user_message("hello"));
  ToolResultMessage orphan;
  orphan.tool_call_id = "call_missing";
  orphan.tool_name = "bash";
  orphan.content = {TextContent{.text = "leftover output"}};
  messages.push_back(Message{orphan});
  messages.push_back(make_user_message("world"));

  Model model{.id = "gpt-5.3-codex",
              .api = "openai-codex-responses",
              .provider = "openai-codex"};
  auto result = transform_messages(messages, model);

  for (const auto &msg : result)
    EXPECT_FALSE(std::holds_alternative<ToolResultMessage>(msg));
  EXPECT_EQ(result.size(), std::size_t(2));
}

TEST(TransformMessages, KeepsToolResultMessagePairedWithItsToolCall) {

  std::vector<Message> messages;
  messages.push_back(make_user_message("hello"));
  AssistantMessage am;
  am.api = "openai-codex-responses";
  am.provider = "openai-codex";
  am.model = "gpt-5.3-codex";
  am.stop_reason = StopReason::tool_use;
  ToolCall call;
  call.id = "call_1";
  call.name = "bash";
  am.content.emplace_back(std::move(call));
  messages.push_back(Message{am});
  ToolResultMessage result_msg;
  result_msg.tool_call_id = "call_1";
  result_msg.tool_name = "bash";
  result_msg.content = {TextContent{.text = "ok"}};
  messages.push_back(Message{result_msg});

  Model model{.id = "gpt-5.3-codex",
              .api = "openai-codex-responses",
              .provider = "openai-codex"};
  auto result = transform_messages(messages, model);

  bool found = false;
  for (const auto &msg : result) {
    if (const auto *trm = std::get_if<ToolResultMessage>(&msg)) {
      EXPECT_EQ(trm->tool_call_id, "call_1");
      found = true;
    }
  }
  EXPECT_TRUE(found);
}

TEST(TransformMessages, DropsContextCompactionMessageForADifferentModel) {

  ContextCompactionMessage opaque;
  opaque.api = "openai-codex-responses";
  opaque.provider = "openai-codex";
  opaque.model = "gpt-5.3-codex";
  opaque.encrypted_content = "opaque-bytes";
  std::vector<Message> messages{Message{opaque}};

  Model same_model{.id = "gpt-5.3-codex",
                   .api = "openai-codex-responses",
                   .provider = "openai-codex"};
  auto kept = transform_messages(messages, same_model);
  ASSERT_THAT(kept, testing::SizeIs(1));
  ASSERT_THAT(kept[0],
              testing::VariantWith<ContextCompactionMessage>(testing::_));

  Model other_model{
      .id = "gpt-4", .api = "openai-completions", .provider = "openai"};
  auto dropped = transform_messages(messages, other_model);
  EXPECT_THAT(dropped, testing::IsEmpty());
}

namespace {
// Minimal recording Renderer used to prove opaque compaction content is
// never surfaced through the renderer dispatch path.
class RecordingRenderer : public Renderer {
public:
  std::string text_deltas;
  std::vector<std::string> requests_text;
  int message_end_count{0};

  void on_text_delta(std::string_view delta) override { text_deltas += delta; }
  void on_request(const RendererRequest &request) override {
    requests_text.push_back(request.text);
  }
  void on_message_end(const TokenUsage &) override { ++message_end_count; }
};
} // namespace

TEST(Renderer, ContextCompactionMessageIsNeverRendered) {

  ContextCompactionMessage opaque;
  opaque.api = "openai-codex-responses";
  opaque.provider = "openai-codex";
  opaque.model = "gpt-5.3-codex";
  opaque.encrypted_content = "top-secret-opaque-bytes";

  RecordingRenderer renderer;
  MessageStartEvent start(Message{opaque});
  dispatch_event(AgentEvent{start}, renderer);
  MessageEndEvent end(Message{opaque});
  dispatch_event(AgentEvent{end}, renderer);

  EXPECT_TRUE(renderer.text_deltas.empty());
  EXPECT_TRUE(renderer.requests_text.empty());
  // MessageEndEvent only calls on_message_end_presentation for
  // AssistantMessage; a compaction item must not trigger it either.
  EXPECT_EQ(renderer.message_end_count, 0);
}

TEST(TokenUsage, JSON) {

  TokenUsage usage;
  usage.input = 100;
  usage.output = 50;
  usage.cache_read = 80;
  usage.cache_write = 20;
  usage.total_tokens = 150;

  std::string json_str = json::to_json(usage);
  auto parsed = json::from_json(json_str);
  // TokenUsage doesn't have from_json, just verify to_json doesn't crash
  EXPECT_FALSE(json_str.empty());
}

// ─── Event stream tests ───────────────────────────────────────────────────

TEST(EventStream, PushAndConsume) {

  EventStream<AgentEvent, std::vector<Message>> stream(
      [](const AgentEvent &ev) {
        return std::holds_alternative<AgentEndEvent>(ev);
      },
      [](const AgentEvent &ev) -> std::vector<Message> {
        if (auto *e = std::get_if<AgentEndEvent>(&ev)) {
          return e->messages;
        }
        return {};
      });

  bool pushed = stream.push(AgentStartEvent());
  EXPECT_TRUE(pushed);

  auto ev = stream.next();
  ASSERT_TRUE(ev.has_value());
  ASSERT_THAT(
      ev, testing::Optional(testing::VariantWith<AgentStartEvent>(testing::_)));

  AgentEndEvent end_event(std::vector<Message>{});
  pushed = stream.push_and_check(std::move(end_event));
  EXPECT_FALSE(pushed); // Should return false since stream is done

  EXPECT_TRUE(stream.is_done());
}

TEST(EventStream, ForEach) {

  EventStream<AgentEvent, std::monostate> stream(
      [](const AgentEvent &) { return false; },
      [](const AgentEvent &) { return std::monostate{}; });

  int count = 0;

  std::thread t([&stream, &count]() {
    stream.push(AgentStartEvent());
    stream.push(TurnStartEvent());
    stream.finish();
  });

  stream.for_each([&count](const AgentEvent &) { count++; });
  t.join();

  EXPECT_GE(count, 2);
}

TEST(EventStream, WaitReturnsResult) {

  std::vector<Message> test_messages;
  UserMessage msg;
  msg.timestamp = 1;
  test_messages.push_back(std::move(msg));

  EventStream<AgentEvent, std::vector<Message>> stream(
      [](const AgentEvent &ev) {
        return std::holds_alternative<AgentEndEvent>(ev);
      },
      [](const AgentEvent &ev) -> std::vector<Message> {
        if (auto *e = std::get_if<AgentEndEvent>(&ev)) {
          return e->messages;
        }
        return {};
      });

  stream.push(AgentStartEvent());

  AgentEndEvent end_event(std::move(test_messages));
  stream.push_and_check(std::move(end_event));

  auto [result, error] = stream.wait();
  EXPECT_FALSE(error.has_value());
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->size(), std::size_t(1));
}

TEST(EventStream, ErrorPath) {

  EventStream<AgentEvent, std::monostate> stream(
      [](const AgentEvent &) { return false; },
      [](const AgentEvent &) { return std::monostate{}; });

  stream.finish_error("Something went wrong");

  auto [result, error] = stream.wait();
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(*error, "Something went wrong");
}

TEST(AsyncEventStream, PushAndWait) {

  AsyncEventStream stream;

  stream.push(AgentStartEvent());
  stream.push(TurnStartEvent());
  stream.push(AgentEndEvent(std::vector<Message>{}));

  stream.wait();
  EXPECT_TRUE(stream.is_complete());

  auto events = stream.drain();
  EXPECT_EQ(events.size(), std::size_t(3));
}

// ─── AgentState tests ─────────────────────────────────────────────────────

TEST(AgentState, BasicOperations) {

  AgentState state;

  EXPECT_TRUE(state.system_prompt().empty());
  EXPECT_FALSE(state.is_streaming());
  EXPECT_TRUE(state.messages().empty());
  EXPECT_TRUE(state.tools().empty());

  state.set_system_prompt("You are helpful");
  EXPECT_EQ(state.system_prompt(), "You are helpful");

  Model model;
  model.id = "test-model";
  model.name = "Test";
  state.set_model(model);
  EXPECT_EQ(state.model().id, "test-model");
}

TEST(AgentState, MessageOperations) {

  AgentState state;

  UserMessage msg;
  msg.timestamp = 1;
  TextContent tc;
  tc.text = "Hello";
  msg.content.push_back(std::move(tc));

  state.append_message(std::move(msg));
  EXPECT_EQ(state.messages().size(), std::size_t(1));

  AssistantMessage asm_;
  asm_.timestamp = 2;
  state.append_message(std::move(asm_));
  EXPECT_EQ(state.messages().size(), std::size_t(2));
}

TEST(AgentState, Reset) {

  AgentState state;
  state.set_system_prompt("test");
  state.append_message(UserMessage{});
  state.add_pending_tool_call("call_123");
  state.set_error_message("err");

  state.reset();

  EXPECT_TRUE(state.system_prompt().empty());
  EXPECT_TRUE(state.messages().empty());
  EXPECT_TRUE(state.pending_tool_calls().empty());
  EXPECT_FALSE(state.error_message().has_value());
  EXPECT_FALSE(state.is_streaming());
}

TEST(AgentState, ThreadSafety) {

  AgentState state;

  std::vector<std::thread> writers;
  for (int i = 0; i < 10; i++) {
    writers.emplace_back([&state, i]() {
      UserMessage msg;
      msg.timestamp = i;
      TextContent tc;
      tc.text = "Hello from thread " + std::to_string(i);
      msg.content.push_back(std::move(tc));
      state.append_message(std::move(msg));
    });
  }

  for (auto &t : writers) {
    t.join();
  }

  EXPECT_EQ(state.messages().size(), std::size_t(10));
}

TEST(FindModel, SlashHeavyIDMatchesBeforeSplitting) {

  // Regression: "accounts/fireworks/models/glm-5p2" was being split on the
  // first slash, yielding provider="accounts", id="fireworks/models/glm-5p2",
  // which never matched the registry and produced an empty base_url.
  auto m = find_model("accounts/fireworks/models/glm-5p2", "fireworks");
  ASSERT_TRUE(m.has_value());
  EXPECT_EQ(m->provider, "fireworks");
  EXPECT_FALSE(m->base_url.empty());
}

TEST(FindModel, SimpleIDWithProviderHint) {

  auto m = find_model("gpt-4o", "openai");
  ASSERT_TRUE(m.has_value());
  EXPECT_EQ(m->provider, "openai");
}

TEST(FindModel, ProviderIdShorthand) {

  auto m = find_model("openai/gpt-4o", "");
  ASSERT_TRUE(m.has_value());
  EXPECT_EQ(m->provider, "openai");
}

TEST(FindModel, UnknownIDProducesGenericWithEmptyBaseUrl) {

  auto m = find_model("no-such-model", "");
  ASSERT_TRUE(m.has_value());
  EXPECT_TRUE(m->base_url.empty());
}

TEST(ModelRegistry, ConfiguredResolutionAndMerge) {

  ProviderConfig local;
  local.id = "local";
  local.api = "registry-faux";
  local.base_url = "http://local.test/v1";
  local.auth = ProviderAuthPolicy::none;
  local.headers["X-Provider"] = "local";
  ConfiguredModel local_model;
  local_model.id = "same";
  local_model.headers["X-Model"] = "yes";
  local.models.push_back(local_model);
  ConfiguredModel slash_model;
  slash_model.id = "accounts/company/models/coder";
  local.models.push_back(slash_model);

  ProviderConfig remote;
  remote.id = "remote";
  remote.api = "registry-faux";
  remote.base_url = "http://remote.test/v1";
  remote.auth = ProviderAuthPolicy::none;
  ConfiguredModel remote_model;
  remote_model.id = "same";
  remote.models.push_back(remote_model);

  ModelRegistry registry({{"local", local}, {"remote", remote}});
  const auto *merged = registry.exact("LOCAL", "same");
  ASSERT_THAT(merged, testing::NotNull());
  EXPECT_EQ(merged->base_url, "http://local.test/v1");
  EXPECT_EQ(merged->headers.at("X-Provider"), "local");
  EXPECT_EQ(merged->headers.at("X-Model"), "yes");

  auto explicit_model = registry.resolve(
      {.provider = "local", .model = "local/same", .source = "test"});
  ASSERT_THAT(explicit_model.model, testing::Optional(testing::Field(
                                        &Model::id, testing::StrEq("same"))));

  auto ambiguous = registry.resolve({.model = "same", .source = "test"});
  EXPECT_FALSE(ambiguous);
  EXPECT_THAT(ambiguous.error, testing::HasSubstr("local/same"));
  EXPECT_THAT(ambiguous.error, testing::HasSubstr("remote/same"));

  auto slash = registry.resolve({.provider = "local",
                                 .model = "accounts/company/models/coder",
                                 .source = "test"});
  ASSERT_THAT(
      slash.model,
      testing::Optional(testing::Field(
          &Model::id, testing::StrEq("accounts/company/models/coder"))));

  auto unknown_provider = registry.resolve(
      {.provider = "missing", .model = "model", .source = "test"});
  EXPECT_FALSE(unknown_provider);
  auto custom = registry.resolve({.provider = "missing",
                                  .model = "model",
                                  .base_url = "http://missing.test/v1",
                                  .source = "test"});
  ASSERT_THAT(custom.model, testing::Optional(testing::Field(
                                &Model::provider, testing::StrEq("missing"))));
  EXPECT_EQ(custom.model->base_url, "http://missing.test/v1");

  ProviderConfig override;
  override.id = "openai";
  ConfiguredModel sparse;
  sparse.context_window = 999;
  override.model_overrides.emplace("gpt-4o", sparse);
  ModelRegistry overridden({{"openai", override}});
  const auto *gpt = overridden.exact("openai", "gpt-4o");
  ASSERT_THAT(gpt, testing::NotNull());
  EXPECT_EQ(gpt->context_window, 999ULL);
  EXPECT_EQ(gpt->max_tokens, 16384ULL);
  EXPECT_FALSE(gpt->base_url.empty());
}

TEST(ThinkingResolution, ClampsUnsupportedLevels) {

  Model model;
  model.provider = "local";
  model.id = "reasoning";
  model.reasoning = true;
  model.thinking_level_map = {{"off", std::nullopt},
                              {"low", std::string("low")},
                              {"high", std::string("high")}};
  const auto result = resolve_thinking_level(model, ThinkingLevel::medium);
  EXPECT_EQ(result.level, ThinkingLevel::low);
  EXPECT_THAT(result.warning, testing::Optional(testing::_));

  model.reasoning = false;
  model.thinking_level_map.clear();
  const auto off = resolve_thinking_level(model, ThinkingLevel::high);
  EXPECT_EQ(off.level, ThinkingLevel::off);
  EXPECT_THAT(off.warning, testing::Optional(testing::_));
}

TEST(Model, JSONSerialization) {

  Model model;
  model.id = "gpt-4";
  model.name = "GPT-4";
  model.api = "openai-completions";
  model.provider = "openai";
  model.base_url = "https://api.openai.com/v1";
  model.context_window = 128000;
  model.max_tokens = 4096;

  std::string json_str = json::to_json(model);
  EXPECT_FALSE(json_str.empty());
  EXPECT_THAT(json_str, testing::HasSubstr("\"id\""));
  EXPECT_THAT(json_str, testing::HasSubstr("\"gpt-4\""));
}

TEST(AgentEvent, CanonicalJSONEnvelope) {

  AgentStartEvent event;
  event.sequence = 42;
  auto value = event_to_json(event);

  EXPECT_EQ(value.value("type", ""), "event");
  EXPECT_EQ(value.value("event", ""), "agent_start");
  EXPECT_EQ(value.value("sequence", 0ULL), 42ULL);
  EXPECT_TRUE(value.contains("timestamp"));
  EXPECT_TRUE(value.contains("data"));
}

TEST(MessageStartEvent, RequestProvenanceJSON) {

  UserMessage user;
  user.content.emplace_back(TextContent{.text = "hello"});
  auto ordinary = event_to_json(MessageStartEvent{Message{user}});
  EXPECT_FALSE(ordinary["data"].contains("request"));

  AssistantMessage assistant;
  assistant.content.emplace_back(TextContent{.text = "answer"});
  const auto assistant_json =
      event_to_json(MessageStartEvent{Message{std::move(assistant)}});
  EXPECT_FALSE(assistant_json["data"].contains("request"));

  InputProvenance presentation{.source = InputProvenance::Source::mailbox,
                               .message_id = "msg-1",
                               .message_kind = "request",
                               .sender_agent_id = "agent-1",
                               .sender_session_id = "session-1",
                               .sender_task_path = "/task",
                               .sender_session_name = "session-name"};
  auto mailbox =
      event_to_json(MessageStartEvent{Message{std::move(user)}, presentation});
  const auto &request = mailbox["data"]["request"];
  EXPECT_EQ(request.value("source", ""), "mailbox");
  EXPECT_EQ(request.value("message_id", ""), "msg-1");
  EXPECT_EQ(request.value("message_kind", ""), "request");
  EXPECT_EQ(request.value("sender_agent_id", ""), "agent-1");
  EXPECT_EQ(request.value("sender_session_id", ""), "session-1");
  EXPECT_EQ(request.value("sender_task_path", ""), "/task");
  EXPECT_EQ(request.value("sender_session_name", ""), "session-name");
}

TEST(ToolEvent, StructuredStatusJSON) {

  ToolExecutionEndEvent event("call-1", "bash", nullptr, true,
                              ToolExecutionStatus::blocked);
  auto value = event_to_json(event);
  EXPECT_EQ(value["data"].value("status", ""), "blocked");
  EXPECT_TRUE(value["data"].value("is_error", false));
}

TEST(ToolPresentationEvent, MailboxReceiptJSON) {

  MailboxReplyQueuedNotice notice{.request_message_id = "request-1",
                                  .recipient_session_id = "session-b",
                                  .recipient_agent_id = "agent-b",
                                  .reply_text = "queued text"};
  ToolPresentationEvent event("call-7", notice);
  const auto value = event_to_json(event);
  EXPECT_EQ(value["event"].get<std::string>(), "tool_presentation");
  EXPECT_EQ(value["data"].value("kind", ""), "mailbox_reply_queued");
  EXPECT_EQ(value["data"].value("tool_call_id", ""), "call-7");
  EXPECT_EQ(value["data"].value("request_message_id", ""), "request-1");
  EXPECT_EQ(value["data"].value("state", ""), "queued");
  EXPECT_EQ(value["data"].value("recipient_agent_id", ""), "agent-b");
}

TEST(SessionStore, OrderedMessagesAndTruncationReplay) {

  pi::test::TemporaryDirectory directory("pici-session-journal-test");
  const auto &dir = directory.path();

  SessionStore store(dir);
  SessionHeader header{.id = "session-1"};
  const auto id = store.create(header);

  auto make_user = [](std::string text) {
    UserMessage message;
    message.content.emplace_back(TextContent{.text = std::move(text)});
    return Message{std::move(message)};
  };

  store.append_message(id, make_user("one"));
  store.append_message(id, make_user("two"));
  store.append_truncate(id, 1);
  store.append_message(id, make_user("three"));

  store.set_model(id, "remote", "accounts/company/models/coder");

  auto record = store.load(id);
  ASSERT_TRUE(record.has_value());
  EXPECT_EQ(record->header.provider, "remote");
  EXPECT_EQ(record->header.model, "accounts/company/models/coder");
  EXPECT_EQ(record->messages.size(), std::size_t(2));
  EXPECT_EQ(std::get<UserMessage>(record->messages[0]).content.size(),
            std::size_t(1));
  EXPECT_EQ(std::get<TextContent>(
                std::get<UserMessage>(record->messages[0]).content[0])
                .text,
            "one");
  EXPECT_EQ(std::get<TextContent>(
                std::get<UserMessage>(record->messages[1]).content[0])
                .text,
            "three");
  const auto listed = store.list();
  EXPECT_EQ(listed.size(), std::size_t(1));
  EXPECT_EQ(listed.front().provider, "remote");
  EXPECT_EQ(listed.front().model, "accounts/company/models/coder");
}

TEST(SessionStore, CompactionRecordReplacesTranscriptWholesale) {

  pi::test::TemporaryDirectory directory("pici-session-compaction-test");
  const auto &dir = directory.path();

  SessionStore store(dir);
  SessionHeader header{.id = "session-compact-1"};
  const auto id = store.create(header);

  store.append_message(id, make_user_message("one"));
  store.append_message(id, make_user_message("two"));
  store.append_message(id, make_user_message("three"));

  SessionCompactionRecord compaction;
  compaction.messages.push_back(make_user_message("retained summary"));
  ContextCompactionMessage opaque;
  opaque.api = "openai-codex-responses";
  opaque.provider = "openai-codex";
  opaque.model = "gpt-5.3-codex";
  opaque.encrypted_content = "opaque-bytes";
  compaction.messages.push_back(Message{opaque});
  compaction.provider = "openai-codex";
  compaction.model = "gpt-5.3-codex";
  compaction.summary = "server";
  compaction.timestamp = 100;
  store.append_compaction(id, compaction);

  store.append_message(id, make_user_message("after compaction"));

  auto record = store.load(id);
  ASSERT_TRUE(record.has_value());
  EXPECT_EQ(record->messages.size(), std::size_t(3));
  EXPECT_EQ(std::get<TextContent>(
                std::get<UserMessage>(record->messages[0]).content[0])
                .text,
            "retained summary");
  ASSERT_THAT(record->messages[1],
              testing::VariantWith<ContextCompactionMessage>(testing::_));
  EXPECT_EQ(
      std::get<ContextCompactionMessage>(record->messages[1]).encrypted_content,
      "opaque-bytes");
  EXPECT_EQ(std::get<TextContent>(
                std::get<UserMessage>(record->messages[2]).content[0])
                .text,
            "after compaction");
  EXPECT_EQ(record->header.min_schema_version, 2);
}

TEST(SessionStore, MalformedCompactionRecordFailsLoudly) {

  pi::test::TemporaryDirectory directory(
      "pici-session-compaction-malformed-test");
  const auto &dir = directory.path();

  SessionStore store(dir);
  SessionHeader header{.id = "session-compact-bad"};
  const auto id = store.create(header);
  store.append_message(id, make_user_message("one"));

  // Hand-write a malformed compaction record directly (missing
  // "messages"), bypassing append_compaction's validation, to
  // simulate a corrupted file on disk.
  {
    std::ofstream f(dir / (id + ".jsonl"), std::ios::app);
    f << "{\"type\":\"compaction\",\"minVersion\":2}\n";
  }

  bool threw = false;
  try {
    (void)store.load(id);
  } catch (const std::exception &) {
    threw = true;
  }
  EXPECT_TRUE(threw);
}

TEST(SessionStore, CompactionRecordFromANewerSchemaFailsClosed) {
  pi::test::TemporaryDirectory directory("pici-session-compaction-future-test");
  const auto &dir = directory.path();

  SessionStore store(dir);
  SessionHeader header{.id = "session-compact-future"};
  const auto id = store.create(header);

  {
    std::ofstream f(dir / (id + ".jsonl"), std::ios::app);
    f << "{\"type\":\"compaction\",\"minVersion\":99,\"messages\":[]}\n";
  }

  bool threw = false;
  std::string what;
  try {
    (void)store.load(id);
  } catch (const std::exception &e) {
    threw = true;
    what = e.what();
  }
  EXPECT_TRUE(threw);
  EXPECT_THAT(what, testing::HasSubstr("99"));
}

TEST(SessionStore, RefusesToCompactASessionWithForkedChildren) {
  pi::test::TemporaryDirectory directory("pici-session-compaction-fork-test");
  const auto &dir = directory.path();

  SessionStore store(dir);
  SessionHeader parent_header{.id = "session-parent"};
  const auto parent_id = store.create(parent_header);
  store.append_message(parent_id, make_user_message("parent msg"));

  EXPECT_FALSE(store.has_children(parent_id));

  SessionHeader child_header{.id = "session-child",
                             .parent_id = parent_id,
                             .parent_offset = std::size_t(1)};
  const auto child_id = store.create(child_header);
  (void)child_id;

  EXPECT_TRUE(store.has_children(parent_id));

  SessionCompactionRecord compaction;
  compaction.messages.push_back(make_user_message("summary"));
  bool threw = false;
  try {
    store.append_compaction(parent_id, compaction);
  } catch (const std::exception &) {
    threw = true;
  }
  EXPECT_TRUE(threw);

  // The child itself has no children of its own, so compacting it
  // (the supported "fork, then compact the fork" flow) succeeds and
  // does not touch the parent's file.
  auto parent_before = store.load(parent_id);
  store.append_compaction(child_id, compaction);
  auto parent_after = store.load(parent_id);
  ASSERT_TRUE(parent_before.has_value());
  ASSERT_TRUE(parent_after.has_value());
  EXPECT_EQ(parent_before->messages.size(), parent_after->messages.size());

  auto child_record = store.load(child_id);
  ASSERT_TRUE(child_record.has_value());
  EXPECT_EQ(child_record->messages.size(), std::size_t(1));
}
