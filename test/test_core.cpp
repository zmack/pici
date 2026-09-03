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
#include <nlohmann/json.hpp>

using namespace pi::core;

namespace {
Message make_user_message(std::string text) {
  UserMessage message;
  message.content.emplace_back(TextContent{.text = std::move(text)});
  return Message{std::move(message)};
}

// SessionStore::append_truncate was removed in Phase 2 (no new "truncate"
// records are ever written again -- see SessionStore::branch), but Phase 1's
// read path must still replay old ones forever. Tests that want to exercise
// that legacy replay path hand-write the raw JSONL line directly, same as
// this file's other hand-written malformed/legacy fixtures.
void append_raw_legacy_truncate(const std::filesystem::path &dir,
                                const std::string &session_id,
                                std::size_t through) {
  std::ofstream f(dir / (session_id + ".jsonl"), std::ios::app);
  f << "{\"type\":\"truncate\",\"through\":" << through << "}\n";
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

TEST(ModelCatalog, ConfiguredResolutionAndMerge) {

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

  ModelCatalog registry({{"local", local}, {"remote", remote}});
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
  ModelCatalog overridden({{"openai", override}});
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
  append_raw_legacy_truncate(dir, id, 1);
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
  // As of Phase 2, kSessionSchemaVersion is 3 (real entry ids + the
  // "kind" compaction discriminator), and append_compaction always writes
  // minVersion == kSessionSchemaVersion.
  EXPECT_EQ(record->header.min_schema_version, kSessionSchemaVersion);
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

// Phase 3 narrowed append_compaction's children-guard to only block on
// children whose anchor is `none` (this legacy count-slice case) or an
// unresolvable `entry` anchor -- see UnresolvableAnchorStillBlocksCompaction
// and ForkParentEntryIdSurvivesLaterParentCompaction for those two other
// cases. This test (originally
// RefusesToCompactASessionWithForkedChildren, from before Phase 3) now
// exists specifically to document/pin that the `none` case -- a child
// header written with only the legacy parentOffset, no anchor fields at
// all (i.e. simulating a pre-Phase-3 fork, since child_header below never
// sets parent_entry_anchor and it defaults to `none`) -- still blocks.
TEST(SessionStore, LegacyOffsetOnlyForkStillBlocksCompaction) {
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
  ASSERT_EQ(child_header.parent_entry_anchor, ParentEntryAnchor::none);
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

TEST(SessionStore, LegacyEntryIdsAreLineOrdinalsAndSkipMetaLines) {
  pi::test::TemporaryDirectory directory("pici-session-legacy-ids-test");
  const auto &dir = directory.path();

  SessionStore store(dir);
  SessionHeader header{.id = "session-legacy-ids"};
  const auto id = store.create(header);

  // Hand-write raw legacy-format lines (no "id"/"parentId" keys) directly,
  // bypassing the store's (now id-minting) append_message/set_name, to pin
  // Phase 1's legacy-id derivation in isolation from Phase 2's real-id
  // write path (see AppendedMessagesGetCollisionCheckedRandomEntryIds and
  // MixedLegacyAndRealEntryIdsChainCorrectly for that).
  {
    std::ofstream f(dir / (id + ".jsonl"), std::ios::app);
    // Header is physical line 1, first message is line 2.
    f << "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":"
         "\"first\"}]}\n";
    // A meta line (line 3) must not consume an entry id or become a link
    // in the parent chain.
    f << "{\"type\":\"meta\",\"name\":\"renamed\"}\n";
    // Second message is physical line 4.
    f << "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":"
         "\"second\"}]}\n";
  }

  auto record = store.load(id);
  ASSERT_TRUE(record.has_value());
  ASSERT_EQ(record->messages.size(), std::size_t(2));
  ASSERT_EQ(record->entry_ids.size(), std::size_t(2));
  EXPECT_EQ(record->entry_ids[0], "legacy-2");
  // Not off-by-one from the meta line at physical line 3: the second
  // message is physical line 4, not line 3.
  EXPECT_EQ(record->entry_ids[1], "legacy-4");
  EXPECT_EQ(record->leaf_entry_id, std::optional<std::string>("legacy-4"));
}

TEST(SessionStore, CompactionEntryGetsASingleLegacyId) {
  pi::test::TemporaryDirectory directory("pici-session-compaction-id-test");
  const auto &dir = directory.path();

  SessionStore store(dir);
  SessionHeader header{.id = "session-compaction-id"};
  const auto id = store.create(header);

  // Hand-write raw legacy-format lines directly (no "id"/"parentId" keys),
  // bypassing the store's (now id-minting) append_message/append_compaction,
  // to pin Phase 1's legacy-id derivation for a compaction record in
  // isolation from Phase 2's real-id write path.
  {
    std::ofstream f(dir / (id + ".jsonl"), std::ios::app);
    f << "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":"
         "\"one\"}]}\n";
    f << "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":"
         "\"two\"}]}\n";
    // Compaction record is physical line 4 (header=1, one=2, two=3).
    f << "{\"type\":\"compaction\",\"minVersion\":" << kSessionSchemaVersion
      << ",\"provider\":\"remote\",\"model\":\"some-model\",\"summary\":"
         "\"server\",\"timestamp\":42,\"messages\":["
         "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":"
         "\"retained-a\"}]},"
         "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":"
         "\"retained-b\"}]}]}\n";
  }

  auto record = store.load(id);
  ASSERT_TRUE(record.has_value());
  ASSERT_EQ(record->messages.size(), std::size_t(2));
  ASSERT_EQ(record->entry_ids.size(), std::size_t(2));
  // Both messages the compaction contributed wholesale share its single
  // legacy id.
  EXPECT_EQ(record->entry_ids[0], "legacy-4");
  EXPECT_EQ(record->entry_ids[1], "legacy-4");
  EXPECT_EQ(record->leaf_entry_id, std::optional<std::string>("legacy-4"));
}

TEST(SessionStore, ForkedSessionWithParentTruncationReplaysIdentically) {
  pi::test::TemporaryDirectory directory("pici-session-fork-truncate-test");
  const auto &dir = directory.path();

  SessionStore store(dir);
  SessionHeader parent_header{.id = "session-parent-truncate"};
  const auto parent_id = store.create(parent_header);

  store.append_message(parent_id, make_user_message("p0"));
  store.append_message(parent_id, make_user_message("p1"));
  store.append_message(parent_id, make_user_message("p2"));
  append_raw_legacy_truncate(dir, parent_id, 1);
  store.append_message(parent_id, make_user_message("p3"));

  auto parent_record = store.load(parent_id);
  ASSERT_TRUE(parent_record.has_value());
  // Parent replay: [p0,p1,p2] -> truncate(1) -> [p0] -> push p3 -> [p0,p3].
  ASSERT_EQ(parent_record->messages.size(), std::size_t(2));

  // Fork landing exactly at the truncated boundary (offset 1: just "p0").
  SessionHeader child_at_boundary{.id = "session-child-at-boundary",
                                  .parent_id = parent_id,
                                  .parent_offset = std::size_t(1)};
  const auto child_at_boundary_id = store.create(child_at_boundary);
  auto child_at_boundary_record = store.load(child_at_boundary_id);
  ASSERT_TRUE(child_at_boundary_record.has_value());
  ASSERT_EQ(child_at_boundary_record->messages.size(), std::size_t(1));
  EXPECT_EQ(std::get<TextContent>(
                std::get<UserMessage>(child_at_boundary_record->messages[0])
                    .content[0])
                .text,
            "p0");

  // Fork landing past the boundary (offset 2: "p0" and "p3").
  SessionHeader child_past_boundary{.id = "session-child-past-boundary",
                                    .parent_id = parent_id,
                                    .parent_offset = std::size_t(2)};
  const auto child_past_boundary_id = store.create(child_past_boundary);
  auto child_past_boundary_record = store.load(child_past_boundary_id);
  ASSERT_TRUE(child_past_boundary_record.has_value());
  ASSERT_EQ(child_past_boundary_record->messages.size(), std::size_t(2));
  EXPECT_EQ(std::get<TextContent>(
                std::get<UserMessage>(child_past_boundary_record->messages[0])
                    .content[0])
                .text,
            "p0");
  EXPECT_EQ(std::get<TextContent>(
                std::get<UserMessage>(child_past_boundary_record->messages[1])
                    .content[0])
                .text,
            "p3");
}

TEST(SessionStore, CompactionFollowedByTruncateReplaysIdentically) {
  pi::test::TemporaryDirectory directory(
      "pici-session-compaction-then-truncate-test");
  const auto &dir = directory.path();

  SessionStore store(dir);
  SessionHeader header{.id = "session-compaction-then-truncate"};
  const auto id = store.create(header);

  store.append_message(id, make_user_message("m0"));
  store.append_message(id, make_user_message("m1"));
  store.append_message(id, make_user_message("m2"));

  SessionCompactionRecord compaction;
  compaction.messages.push_back(make_user_message("c0"));
  compaction.provider = "remote";
  compaction.model = "some-model";
  compaction.summary = "server";
  compaction.timestamp = 7;
  store.append_compaction(id, compaction);

  store.append_message(id, make_user_message("m3"));
  store.append_message(id, make_user_message("m4"));

  // Post-compaction vector before this truncate is [c0, m3, m4]; truncate
  // through 2 must resize *that* vector, not reinterpret 2 as an entry
  // index into the whole file.
  append_raw_legacy_truncate(dir, id, 2);

  auto record = store.load(id);
  ASSERT_TRUE(record.has_value());
  ASSERT_EQ(record->messages.size(), std::size_t(2));
  EXPECT_EQ(std::get<TextContent>(
                std::get<UserMessage>(record->messages[0]).content[0])
                .text,
            "c0");
  EXPECT_EQ(std::get<TextContent>(
                std::get<UserMessage>(record->messages[1]).content[0])
                .text,
            "m3");
}

TEST(SessionStore, CycleInParentPointersThrows) {
  pi::test::TemporaryDirectory directory("pici-session-entry-cycle-test");
  const auto &dir = directory.path();

  SessionStore store(dir);
  SessionHeader header{.id = "session-entry-cycle"};
  const auto id = store.create(header);

  // Hand-write two messages whose explicit ids/parentIds form a cycle,
  // bypassing append_message's (id-less) writer to simulate a corrupted
  // file, same style as MalformedCompactionRecordFailsLoudly.
  {
    std::ofstream f(dir / (id + ".jsonl"), std::ios::app);
    f << "{\"role\":\"user\",\"id\":\"a\",\"parentId\":\"b\",\"content\":[{"
         "\"type\":\"text\",\"text\":\"x\"}]}\n";
    f << "{\"role\":\"user\",\"id\":\"b\",\"parentId\":\"a\",\"content\":[{"
         "\"type\":\"text\",\"text\":\"y\"}]}\n";
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
  EXPECT_THAT(what, testing::HasSubstr("cycle"));
}

TEST(SessionStore, DanglingParentIdThrows) {
  pi::test::TemporaryDirectory directory("pici-session-dangling-parent-test");
  const auto &dir = directory.path();

  SessionStore store(dir);
  SessionHeader header{.id = "session-dangling-parent"};
  const auto id = store.create(header);

  {
    std::ofstream f(dir / (id + ".jsonl"), std::ios::app);
    f << "{\"role\":\"user\",\"id\":\"a\",\"parentId\":\"does-not-exist\","
         "\"content\":[{\"type\":\"text\",\"text\":\"x\"}]}\n";
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
  EXPECT_THAT(what, testing::HasSubstr("does-not-exist"));
}

namespace {
std::vector<std::string> read_nonempty_lines(const std::filesystem::path &path) {
  std::ifstream f(path);
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(f, line))
    if (!line.empty())
      lines.push_back(line);
  return lines;
}
} // namespace

TEST(SessionStore, AppendedMessagesGetCollisionCheckedRandomEntryIds) {
  pi::test::TemporaryDirectory directory("pici-session-random-ids-test");
  const auto &dir = directory.path();

  SessionStore store(dir);
  SessionHeader header{.id = "session-random-ids"};
  const auto id = store.create(header);

  store.append_message(id, make_user_message("one"));
  store.append_message(id, make_user_message("two"));
  store.append_message(id, make_user_message("three"));

  auto record = store.load(id);
  ASSERT_TRUE(record.has_value());
  ASSERT_EQ(record->entry_ids.size(), std::size_t(3));
  for (const auto &eid : record->entry_ids) {
    EXPECT_FALSE(eid.empty());
    EXPECT_THAT(eid, testing::Not(testing::StartsWith("legacy-")));
  }
  EXPECT_NE(record->entry_ids[0], record->entry_ids[1]);
  EXPECT_NE(record->entry_ids[1], record->entry_ids[2]);
  EXPECT_NE(record->entry_ids[0], record->entry_ids[2]);
  EXPECT_EQ(record->leaf_entry_id,
           std::optional<std::string>(record->entry_ids[2]));

  // Raw file: each message line's own "parentId" chains to the previous
  // message's "id", and the first real message's "parentId" is null (root).
  auto lines = read_nonempty_lines(dir / (id + ".jsonl"));
  ASSERT_EQ(lines.size(), std::size_t(4)); // header + 3 messages
  auto j1 = nlohmann::json::parse(lines[1]);
  auto j2 = nlohmann::json::parse(lines[2]);
  auto j3 = nlohmann::json::parse(lines[3]);
  EXPECT_TRUE(j1["parentId"].is_null());
  EXPECT_EQ(j2["parentId"].get<std::string>(), j1["id"].get<std::string>());
  EXPECT_EQ(j3["parentId"].get<std::string>(), j2["id"].get<std::string>());
}

TEST(SessionStore,
    ReopeningAnExistingSessionInANewProcessResumesLeafWithoutCollision) {
  pi::test::TemporaryDirectory directory("pici-session-reopen-test");
  const auto &dir = directory.path();

  SessionStore store_a(dir);
  SessionHeader header{.id = "session-reopen"};
  const auto id = store_a.create(header);
  store_a.append_message(id, make_user_message("a0"));
  store_a.append_message(id, make_user_message("a1"));

  // A second SessionStore instance over the same directory, simulating a
  // different process reopening the file: its leaf_ids_ cache starts empty
  // for this session.
  SessionStore store_b(dir);
  store_b.append_message(id, make_user_message("b0"));

  auto record = store_a.load(id);
  ASSERT_TRUE(record.has_value());
  ASSERT_EQ(record->messages.size(), std::size_t(3));
  ASSERT_EQ(record->entry_ids.size(), std::size_t(3));
  EXPECT_NE(record->entry_ids[2], record->entry_ids[0]);
  EXPECT_NE(record->entry_ids[2], record->entry_ids[1]);

  auto lines = read_nonempty_lines(dir / (id + ".jsonl"));
  ASSERT_EQ(lines.size(), std::size_t(4));
  auto j_prev = nlohmann::json::parse(lines[2]);
  auto j_last = nlohmann::json::parse(lines[3]);
  ASSERT_TRUE(j_last["parentId"].is_string());
  EXPECT_EQ(j_last["parentId"].get<std::string>(),
           j_prev["id"].get<std::string>());
}

TEST(SessionStore, MixedLegacyAndRealEntryIdsChainCorrectly) {
  pi::test::TemporaryDirectory directory(
      "pici-session-mixed-legacy-real-test");
  const auto &dir = directory.path();

  SessionStore store(dir);
  SessionHeader header{.id = "session-mixed-legacy-real"};
  const auto id = store.create(header);

  // Hand-write one legacy message (no "id"/"parentId" keys) at physical
  // line 2 (header is line 1) -- Phase 1's legacy-id scheme gives it the id
  // "legacy-2".
  {
    std::ofstream f(dir / (id + ".jsonl"), std::ios::app);
    f << "{\"role\":\"user\",\"content\":[{\"type\":\"text\",\"text\":"
         "\"legacy\"}]}\n";
  }

  store.append_message(id, make_user_message("real"));

  // Pin the format: the new (real) line's raw on-disk "parentId" is
  // literally "legacy-2" -- a format-stability regression test for Phase
  // 1's legacy-id definition, not just a functional replay check.
  auto lines = read_nonempty_lines(dir / (id + ".jsonl"));
  ASSERT_EQ(lines.size(), std::size_t(3));
  auto j_real = nlohmann::json::parse(lines[2]);
  ASSERT_TRUE(j_real["parentId"].is_string());
  EXPECT_EQ(j_real["parentId"].get<std::string>(), "legacy-2");

  auto record = store.load(id);
  ASSERT_TRUE(record.has_value());
  ASSERT_EQ(record->messages.size(), std::size_t(2));
  EXPECT_EQ(record->entry_ids[0], "legacy-2");
}

TEST(SessionStore, BranchIsNonDestructiveAndMovesTheLeaf) {
  pi::test::TemporaryDirectory directory("pici-session-branch-test");
  const auto &dir = directory.path();

  SessionStore store(dir);
  SessionHeader header{.id = "session-branch"};
  const auto id = store.create(header);

  store.append_message(id, make_user_message("m0"));
  store.append_message(id, make_user_message("m1"));
  store.append_message(id, make_user_message("m2"));

  auto before = store.load(id);
  ASSERT_TRUE(before.has_value());
  ASSERT_EQ(before->entry_ids.size(), std::size_t(3));
  const auto target = before->entry_ids[0]; // branch back to just after m0

  auto branched_messages = store.branch(id, target);
  ASSERT_EQ(branched_messages.size(), std::size_t(1));
  EXPECT_EQ(std::get<TextContent>(
                std::get<UserMessage>(branched_messages[0]).content[0])
                .text,
            "m0");

  store.append_message(id, make_user_message("m3-after-branch"));

  auto after = store.load(id);
  ASSERT_TRUE(after.has_value());
  ASSERT_EQ(after->messages.size(), std::size_t(2));
  EXPECT_EQ(std::get<TextContent>(
                std::get<UserMessage>(after->messages[0]).content[0])
                .text,
            "m0");
  EXPECT_EQ(std::get<TextContent>(
                std::get<UserMessage>(after->messages[1]).content[0])
                .text,
            "m3-after-branch");

  // Non-destructive: m1/m2 are still present verbatim in the raw file even
  // though they're no longer part of the live (replayed) chain, and the
  // branch record itself is a "kind":"branch" compaction, not a new record
  // type.
  std::ifstream f(dir / (id + ".jsonl"));
  std::string contents((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
  EXPECT_THAT(contents, testing::HasSubstr("\"m1\""));
  EXPECT_THAT(contents, testing::HasSubstr("\"m2\""));
  EXPECT_THAT(contents, testing::HasSubstr("\"kind\":\"branch\""));
  EXPECT_THAT(contents, testing::HasSubstr("\"type\":\"compaction\""));
}

TEST(SessionStore, BranchToRootResetsLeafWhenTargetIsNullopt) {
  pi::test::TemporaryDirectory directory("pici-session-branch-root-test");
  const auto &dir = directory.path();

  SessionStore store(dir);
  SessionHeader header{.id = "session-branch-root"};
  const auto id = store.create(header);

  store.append_message(id, make_user_message("m0"));
  store.append_message(id, make_user_message("m1"));

  auto branched = store.branch(id, std::nullopt);
  EXPECT_TRUE(branched.empty());

  auto record = store.load(id);
  ASSERT_TRUE(record.has_value());
  EXPECT_TRUE(record->messages.empty());

  store.append_message(id, make_user_message("fresh-start"));
  auto after = store.load(id);
  ASSERT_TRUE(after.has_value());
  ASSERT_EQ(after->messages.size(), std::size_t(1));
  EXPECT_EQ(std::get<TextContent>(
                std::get<UserMessage>(after->messages[0]).content[0])
                .text,
            "fresh-start");
}

TEST(SessionStore, BranchThrowsOnUnknownTargetEntryId) {
  pi::test::TemporaryDirectory directory("pici-session-branch-unknown-test");
  const auto &dir = directory.path();

  SessionStore store(dir);
  SessionHeader header{.id = "session-branch-unknown"};
  const auto id = store.create(header);
  store.append_message(id, make_user_message("m0"));

  bool threw = false;
  std::string what;
  try {
    (void)store.branch(id, std::string("does-not-exist"));
  } catch (const std::exception &e) {
    threw = true;
    what = e.what();
  }
  EXPECT_TRUE(threw);
  EXPECT_THAT(what, testing::HasSubstr("does-not-exist"));
}

TEST(SessionStore, BranchRecordIsGatedByExistingCompactionMinVersionCheck) {
  pi::test::TemporaryDirectory directory(
      "pici-session-branch-version-gate-test");
  const auto &dir = directory.path();

  SessionStore store(dir);
  SessionHeader header{.id = "session-branch-version-gate"};
  const auto id = store.create(header);

  {
    std::ofstream f(dir / (id + ".jsonl"), std::ios::app);
    f << "{\"type\":\"compaction\",\"minVersion\":"
      << (kSessionSchemaVersion + 1)
      << ",\"kind\":\"branch\",\"messages\":[]}\n";
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
  EXPECT_THAT(what,
             testing::HasSubstr(std::to_string(kSessionSchemaVersion + 1)));
}

TEST(SessionStore, ForkRecordsEntryAnchorAlongsideLegacyOffset) {
  pi::test::TemporaryDirectory directory("pici-session-fork-anchor-test");
  const auto &dir = directory.path();

  SessionStore store(dir);
  SessionHeader parent_header{.id = "session-fork-anchor-parent"};
  const auto parent_id = store.create(parent_header);
  store.append_message(parent_id, make_user_message("p0"));
  store.append_message(parent_id, make_user_message("p1"));

  auto anchor = store.current_leaf_anchor(parent_id);
  EXPECT_EQ(anchor.anchor, ParentEntryAnchor::entry);
  ASSERT_TRUE(anchor.entry_id.has_value());

  SessionHeader child_header{.id = "session-fork-anchor-child",
                             .parent_id = parent_id,
                             .parent_offset = std::size_t(2),
                             .parent_entry_anchor = anchor.anchor,
                             .parent_entry_id = anchor.entry_id};
  const auto child_id = store.create(child_header);

  // Both the legacy parentOffset and the new anchor fields are present on
  // the raw header line -- the anchor is written ALONGSIDE, not instead
  // of, the legacy field.
  auto lines = read_nonempty_lines(dir / (child_id + ".jsonl"));
  ASSERT_FALSE(lines.empty());
  auto header_json = nlohmann::json::parse(lines[0]);
  ASSERT_TRUE(header_json["parentOffset"].is_number_unsigned());
  EXPECT_EQ(header_json["parentOffset"].get<std::size_t>(), std::size_t(2));
  ASSERT_TRUE(header_json.contains("parentEntryAnchor"));
  EXPECT_EQ(header_json["parentEntryAnchor"].get<std::string>(), "entry");
  ASSERT_TRUE(header_json.contains("parentEntryId"));
  EXPECT_EQ(header_json["parentEntryId"].get<std::string>(), *anchor.entry_id);

  auto child_record = store.load(child_id);
  ASSERT_TRUE(child_record.has_value());
  EXPECT_EQ(child_record->header.parent_entry_anchor, ParentEntryAnchor::entry);
  EXPECT_EQ(child_record->header.parent_entry_id, anchor.entry_id);
  ASSERT_EQ(child_record->messages.size(), std::size_t(2));
}

TEST(SessionStore, ForkParentEntryIdSurvivesLaterParentCompaction) {
  pi::test::TemporaryDirectory directory(
      "pici-session-fork-survives-compaction-test");
  const auto &dir = directory.path();

  SessionStore store(dir);
  SessionHeader parent_header{.id = "session-survives-parent"};
  const auto parent_id = store.create(parent_header);
  store.append_message(parent_id, make_user_message("p0"));
  store.append_message(parent_id, make_user_message("p1"));

  auto anchor = store.current_leaf_anchor(parent_id);
  ASSERT_EQ(anchor.anchor, ParentEntryAnchor::entry);
  ASSERT_TRUE(anchor.entry_id.has_value());

  SessionHeader child_header{.id = "session-survives-child",
                             .parent_id = parent_id,
                             .parent_offset = std::size_t(2),
                             .parent_entry_anchor = anchor.anchor,
                             .parent_entry_id = anchor.entry_id};
  const auto child_id = store.create(child_header);

  auto before = store.load(child_id);
  ASSERT_TRUE(before.has_value());
  ASSERT_EQ(before->messages.size(), std::size_t(2));

  // Compacting the parent must NOT be blocked (the child's anchor still
  // resolves against the parent's post-compaction index -- entries are
  // permanent, compaction only appends), and the child must still load
  // its unchanged inherited prefix afterward. This is the key regression
  // proof for the Phase 3 guard-narrowing.
  SessionCompactionRecord compaction;
  compaction.messages.push_back(make_user_message("summary"));
  store.append_compaction(parent_id, compaction);

  auto after = store.load(child_id);
  ASSERT_TRUE(after.has_value());
  ASSERT_EQ(after->messages.size(), std::size_t(2));
  EXPECT_EQ(std::get<TextContent>(
                std::get<UserMessage>(after->messages[0]).content[0])
                .text,
            "p0");
  EXPECT_EQ(std::get<TextContent>(
                std::get<UserMessage>(after->messages[1]).content[0])
                .text,
            "p1");

  // The parent itself now replays as just the compacted summary -- the
  // child's messages are unchanged even though the parent's own live view
  // shrank.
  auto parent_after = store.load(parent_id);
  ASSERT_TRUE(parent_after.has_value());
  ASSERT_EQ(parent_after->messages.size(), std::size_t(1));
}

TEST(SessionStore, ForkFallsBackToParentOffsetWhenAnchorIsNone) {
  pi::test::TemporaryDirectory directory(
      "pici-session-fork-legacy-fallback-test");
  const auto &dir = directory.path();

  SessionStore store(dir);
  SessionHeader parent_header{.id = "session-legacy-fallback-parent"};
  const auto parent_id = store.create(parent_header);
  store.append_message(parent_id, make_user_message("p0"));
  store.append_message(parent_id, make_user_message("p1"));
  store.append_message(parent_id, make_user_message("p2"));

  // Hand-write a child header with ONLY "parentOffset" -- no
  // "parentEntryAnchor"/"parentEntryId" keys at all -- simulating a fork
  // written by a pre-Phase-3 binary.
  const std::string child_id = "session-legacy-fallback-child";
  {
    std::ofstream f(dir / (child_id + ".jsonl"), std::ios::app);
    nlohmann::json header = nlohmann::json::object();
    header["type"] = "session";
    header["id"] = child_id;
    header["parentId"] = parent_id;
    header["parentOffset"] = 2;
    header["created"] = 0;
    header["model"] = "";
    header["provider"] = "";
    f << header.dump() << "\n";
  }

  auto record = store.load(child_id);
  ASSERT_TRUE(record.has_value());
  EXPECT_EQ(record->header.parent_entry_anchor, ParentEntryAnchor::none);
  ASSERT_EQ(record->messages.size(), std::size_t(2));
  EXPECT_EQ(std::get<TextContent>(
                std::get<UserMessage>(record->messages[0]).content[0])
                .text,
            "p0");
  EXPECT_EQ(std::get<TextContent>(
                std::get<UserMessage>(record->messages[1]).content[0])
                .text,
            "p1");
}

TEST(SessionStore, UnresolvableAnchorStillBlocksCompaction) {
  pi::test::TemporaryDirectory directory(
      "pici-session-unresolvable-anchor-test");
  const auto &dir = directory.path();

  SessionStore store(dir);
  SessionHeader parent_header{.id = "session-unresolvable-parent"};
  const auto parent_id = store.create(parent_header);
  store.append_message(parent_id, make_user_message("p0"));

  // Hand-corrupt a child's "parentEntryId" to an id that does not exist in
  // the parent's current index -- an unresolvable anchor must still block
  // compaction, not silently allow it.
  const std::string child_id = "session-unresolvable-child";
  {
    std::ofstream f(dir / (child_id + ".jsonl"), std::ios::app);
    nlohmann::json header = nlohmann::json::object();
    header["type"] = "session";
    header["id"] = child_id;
    header["parentId"] = parent_id;
    header["parentOffset"] = 1;
    header["parentEntryAnchor"] = "entry";
    header["parentEntryId"] = "does-not-exist";
    header["created"] = 0;
    header["model"] = "";
    header["provider"] = "";
    f << header.dump() << "\n";
  }

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

  // Confirm the parent really wasn't touched.
  auto parent_after = store.load(parent_id);
  ASSERT_TRUE(parent_after.has_value());
  ASSERT_EQ(parent_after->messages.size(), std::size_t(1));
}

TEST(SessionStore, ForkOfAForkWithNoOwnEntriesGetsRootAnchorNotNone) {
  pi::test::TemporaryDirectory directory(
      "pici-session-fork-of-fork-root-anchor-test");
  const auto &dir = directory.path();

  SessionStore store(dir);
  SessionHeader a_header{.id = "session-a"};
  const auto a_id = store.create(a_header);
  store.append_message(a_id, make_user_message("a0"));
  store.append_message(a_id, make_user_message("a1"));

  auto a_anchor = store.current_leaf_anchor(a_id);
  ASSERT_EQ(a_anchor.anchor, ParentEntryAnchor::entry);

  // Fork B from A.
  SessionHeader b_header{.id = "session-b",
                         .parent_id = a_id,
                         .parent_offset = std::size_t(2),
                         .parent_entry_anchor = a_anchor.anchor,
                         .parent_entry_id = a_anchor.entry_id};
  const auto b_id = store.create(b_header);

  // B never gets any of its own entries appended -- fork C from B
  // immediately. This is the tri-state design's regression proof: B's OWN
  // file has zero entries of its own, but B is NOT anchor-less -- a bare
  // optional<string> anchor would have collapsed "B has no own entries"
  // and "B has no anchor at all" into the same nullopt, incorrectly
  // telling C's header-parser to fall back to legacy parentOffset
  // count-slicing against B (which itself has 0 messages of its own
  // recorded, only an inherited count). The tri-state anchor instead
  // reports `root`: "C's fork point is before B's first own entry",
  // distinct from `none`.
  auto b_anchor = store.current_leaf_anchor(b_id);
  EXPECT_EQ(b_anchor.anchor, ParentEntryAnchor::root);
  EXPECT_FALSE(b_anchor.entry_id.has_value());

  SessionHeader c_header{.id = "session-c",
                         .parent_id = b_id,
                         .parent_offset = std::size_t(2),
                         .parent_entry_anchor = b_anchor.anchor,
                         .parent_entry_id = b_anchor.entry_id};
  const auto c_id = store.create(c_header);

  auto c_record = store.load(c_id);
  ASSERT_TRUE(c_record.has_value());
  EXPECT_EQ(c_record->header.parent_entry_anchor, ParentEntryAnchor::root);
  EXPECT_NE(c_record->header.parent_entry_anchor, ParentEntryAnchor::none);

  // A `root` anchor's inherited prefix is empty by this phase's explicit
  // design (see load_recursive's ParentEntryAnchor::root case) -- C does
  // not transitively re-inherit B's own inherited-from-A prefix. The
  // regression this test guards against is specifically the anchor being
  // `none` (which would instead try, and in general fail or silently
  // misbehave, to legacy-offset-slice B's near-empty message vector).
  EXPECT_TRUE(c_record->messages.empty());
}
