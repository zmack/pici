#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include "core/agent.h"
#include "core/agent_state.h"
#include "core/auth_types.h"
#include "core/event_json.h"
#include "core/event_types.h"
#include "core/llm_client.h"
#include "core/message_types.h"
#include "core/session/agent_session.h"
#include "core/session/session_id.h"
#include "core/session/session_store.h"
#include "core/stream.h"
#include <gtest/gtest.h>

using namespace pi::core;

class TestSchema : public ToolSchema {
public:
  std::string serialize() const override { return R"({"type":"object"})"; }
  std::map<std::string, std::string> to_definition() const override {
    return {{"type", "object"}};
  }
};

class NamedTool : public ToolDefinition {
public:
  explicit NamedTool(std::string name) : name_(std::move(name)) {}

  std::string_view name() const override { return name_; }
  std::string_view description() const override { return "test tool"; }
  ToolSchema &schema() const override { return schema_; }

  class Result : public ToolResult {
  public:
    bool is_error() const override { return false; }
    std::string content() const override { return "ok"; }
    std::optional<std::string> details() const override { return std::nullopt; }
  };

  std::shared_ptr<ToolResult> execute(std::string_view, std::string_view,
                                      std::stop_token = std::stop_token{},
                                      ToolUpdateCallback = {}) const override {
    return std::make_shared<Result>();
  }

private:
  std::string name_;
  mutable TestSchema schema_;
};

class LifecycleClient : public LLMClient {
public:
  explicit LifecycleClient(std::shared_ptr<std::atomic<int>> calls)
      : calls_(std::move(calls)) {}

  std::shared_ptr<AssistantMessage>
  stream(const Model &model, const AgentContext &, const StreamOptions &,
         AssistantEventCallback, std::stop_token stop_tok) override {
    const auto call_number = calls_->fetch_add(1) + 1;
    auto message = std::make_shared<AssistantMessage>();
    message->api = model.api;
    message->provider = model.provider;
    message->model = model.id;

    if (call_number == 1) {
      while (!stop_tok.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      message->stop_reason = StopReason::aborted;
      message->error_message = "aborted";
      return message;
    }

    message->stop_reason = StopReason::stop;
    message->content.emplace_back(TextContent{.text = "second run"});
    return message;
  }

  std::string_view provider_name() const override { return "test"; }
  std::string_view api_id() const override { return "agent-lifecycle-test"; }

private:
  std::shared_ptr<std::atomic<int>> calls_;
};

// Blocks inside stream() until release() is called, signalling entered()
// first so a test can wait for the agent to genuinely be streaming before
// asserting idle-only rejection (lexicon invariant 10: session/model
// switches and transcript replacement are idle-only transitions).
class BlockingClient : public LLMClient {
public:
  BlockingClient(std::shared_ptr<std::atomic<bool>> entered,
                 std::shared_ptr<std::atomic<bool>> release)
      : entered_(std::move(entered)), release_(std::move(release)) {}

  std::shared_ptr<AssistantMessage>
  stream(const Model &model, const AgentContext &, const StreamOptions &,
         AssistantEventCallback, std::stop_token stop_tok) override {
    entered_->store(true);
    while (!release_->load() && !stop_tok.stop_requested()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    auto message = std::make_shared<AssistantMessage>();
    message->api = model.api;
    message->provider = model.provider;
    message->model = model.id;
    message->stop_reason = StopReason::stop;
    message->content.emplace_back(TextContent{.text = "released"});
    return message;
  }

  std::string_view provider_name() const override { return "test"; }
  std::string_view api_id() const override { return "agent-blocking-test"; }

private:
  std::shared_ptr<std::atomic<bool>> entered_;
  std::shared_ptr<std::atomic<bool>> release_;
};

class ImmediateClient : public LLMClient {
public:
  std::shared_ptr<AssistantMessage>
  stream(const Model &model, const AgentContext &, const StreamOptions &,
         AssistantEventCallback, std::stop_token) override {
    auto message = std::make_shared<AssistantMessage>();
    message->api = model.api;
    message->provider = model.provider;
    message->model = model.id;
    message->stop_reason = StopReason::stop;
    message->content.emplace_back(TextContent{.text = "session response"});
    return message;
  }

  std::string_view provider_name() const override { return "test"; }
  std::string_view api_id() const override { return "agent-session-test"; }
};

// ─── Agent tests ──────────────────────────────────────────────────────────

TEST(Agent, Agent_default_construction) {
  Agent agent;

  EXPECT_TRUE(agent.state().system_prompt().empty());
  EXPECT_TRUE(!agent.state().is_streaming());
  EXPECT_TRUE(agent.state().messages().empty());
}

TEST(Agent, Agent_construction_with_options) {
  Agent::Options opts;
  opts.system_prompt = "You are a helpful assistant";
  opts.model.id = "test-model";
  opts.model.name = "Test";
  opts.model.api = "test";
  opts.model.provider = "test";
  opts.thinking_level = ThinkingLevel::medium;
  opts.tool_execution = ToolExecutionMode::sequential;

  Agent agent(opts);

  EXPECT_TRUE(agent.state().system_prompt() == "You are a helpful assistant");
  EXPECT_TRUE(agent.state().model().id == "test-model");
  EXPECT_TRUE(agent.state().thinking_level() == ThinkingLevel::off);
}

TEST(Agent, Agent_add_tool) {
  Agent agent;
  auto tool = std::make_shared<NamedTool>("echo");
  agent.add_tool(tool);

  EXPECT_EQ(agent.state().tools().size(), std::size_t(1));
  EXPECT_EQ(agent.state().tools()[0]->name(), "echo");
}

TEST(Agent, Agent_set_tools) {
  Agent agent;
  auto tool1 = std::make_shared<NamedTool>("echo");
  auto tool2 = std::make_shared<NamedTool>("counter");

  agent.set_tools({tool1, tool2});
  EXPECT_EQ(agent.state().tools().size(), std::size_t(2));
}

TEST(Agent, Agent_steer_queue) {
  Agent agent;

  EXPECT_TRUE(agent.state().messages().empty());
  agent.steer({});
  EXPECT_TRUE(agent.state().messages().empty());
}

TEST(Agent, Agent_session_switch_drops_envelope_callbacks) {
  Agent agent;
  int accepted = 0;
  UserMessage message;
  message.content.push_back(TextContent{.text = "queued"});
  agent.steer_envelopes({AgentInput{
      .message = Message{std::move(message)},
      .on_accepted = [&accepted] { ++accepted; },
      .presentation = {.source = InputProvenance::Source::mailbox},
  }});

  agent.set_session_identity("new-session");

  EXPECT_EQ(accepted, 0);
}
TEST(Agent, Agent_ordinary_steering_blocks_session_switch) {
  Agent agent;
  UserMessage message;
  message.content.push_back(TextContent{.text = "ordinary"});
  agent.steer({Message{std::move(message)}});
  bool threw = false;
  try {
    agent.set_session_identity("new-session");
  } catch (const std::runtime_error &) {
    threw = true;
  }
  EXPECT_TRUE(threw);
  agent.clear_steering_queue();
  agent.set_session_identity("new-session");
}

TEST(Agent, Agent_follow_up_queue) {
  Agent agent;
  agent.follow_up({});
  EXPECT_TRUE(agent.state().messages().empty());
}

TEST(Agent, Agent_reset) {
  Agent agent;
  agent.set_tools({std::make_shared<NamedTool>("echo")});
  auto msg = UserMessage{};
  msg.timestamp = 1;
  TextContent tc;
  tc.text = "test";
  msg.content.push_back(std::move(tc));
  agent.state().set_messages({std::move(msg)});

  agent.reset();

  EXPECT_TRUE(agent.state().messages().empty());
  EXPECT_TRUE(agent.state().tools().empty());
  EXPECT_TRUE(!agent.state().is_streaming());
}

TEST(Agent, Agent_prompt_creates_stream) {
  Agent::Options opts;
  opts.model.id = "test-model";
  opts.model.api = "test";
  opts.model.provider = "test";

  Agent agent(opts);

  auto stream = agent.prompt("Hello");

  // Drain the stream until the async prompt turn completes.
  for (auto &ev : stream) {
    if (std::holds_alternative<AgentEndEvent>(ev)) {
      break;
    }
  }

  // AgentEndEvent is published before the worker thread flips
  // is_streaming() back to false, so wait for that explicitly instead
  // of racing it.
  agent.wait_for_idle();
  EXPECT_TRUE(!agent.state().is_streaming());
}

TEST(Agent, Agent_abort_and_reuse_run_lifecycle) {
  auto calls = std::make_shared<std::atomic<int>>(0);
  LLMClientRegistry::instance().register_client(
      "agent-lifecycle-test",
      [calls] { return std::make_shared<LifecycleClient>(calls); });

  Agent::Options opts;
  opts.model.id = "test-model";
  opts.model.api = "agent-lifecycle-test";
  opts.model.provider = "test";

  Agent agent(opts);
  auto first = agent.prompt("first");

  bool rejected_concurrent_prompt = false;
  try {
    auto concurrent = agent.prompt("concurrent");
    (void)concurrent;
  } catch (const std::exception &) {
    rejected_concurrent_prompt = true;
  }
  EXPECT_TRUE(rejected_concurrent_prompt);

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (calls->load() == 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_EQ(calls->load(), 1);

  agent.interrupt(TurnAbortReason::timeout);
  bool first_ended = false;
  int aborted_events = 0;
  std::optional<TurnAbortReason> abort_reason;
  for (auto &event : first) {
    if (std::holds_alternative<AgentEndEvent>(event)) {
      first_ended = true;
    }
    if (const auto *aborted = std::get_if<TurnAbortedEvent>(&event)) {
      ++aborted_events;
      abort_reason = aborted->reason;
    }
  }
  agent.wait_for_idle();
  EXPECT_TRUE(first_ended);
  EXPECT_EQ(aborted_events, 1);
  EXPECT_TRUE(abort_reason == TurnAbortReason::timeout);
  EXPECT_TRUE(!agent.is_streaming());

  auto second = agent.prompt("second");
  bool second_ended = false;
  for (auto &event : second) {
    if (std::holds_alternative<AgentEndEvent>(event)) {
      second_ended = true;
    }
  }
  agent.wait_for_idle();

  EXPECT_TRUE(second_ended);
  EXPECT_TRUE(!agent.is_streaming());
  EXPECT_EQ(calls->load(), 2);
  EXPECT_TRUE(!agent.state().error_message().has_value());
}

// Lexicon invariant 10 ("Session activation, model/sandbox changes, and
// transcript replacement are idle-only transitions") and the "Required
// flows" idle-only guard documented on Agent::with_idle_transition: a model
// switch attempted while the agent is genuinely streaming must be rejected,
// and must succeed once the agent returns to idle. set_model, restore_session,
// and compact() all share this one guard (with_idle_transition in agent.cpp),
// so this test exercises the mechanism both SessionRuntime (CLI/RPC) and ACP's
// per-run SessionRuntime rely on — see plans/session-runtime-migration.md
// Phase 1 item 2.

TEST(Agent, Agent_model_switch_rejected_while_streaming_succeeds_once_idle) {
  auto entered = std::make_shared<std::atomic<bool>>(false);
  auto release = std::make_shared<std::atomic<bool>>(false);
  LLMClientRegistry::instance().register_client(
      "agent-blocking-test", [entered, release] {
        return std::make_shared<BlockingClient>(entered, release);
      });

  Agent::Options opts;
  opts.model.id = "test-model";
  opts.model.api = "agent-blocking-test";
  opts.model.provider = "test";

  Agent agent(opts);
  auto stream = agent.prompt("first");

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!entered->load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_TRUE(entered->load());
  EXPECT_TRUE(agent.is_streaming());

  Model other_model;
  other_model.id = "other-model";
  other_model.api = "agent-blocking-test";
  other_model.provider = "test";

  bool rejected_while_streaming = false;
  std::string rejection_message;
  try {
    static_cast<void>(agent.set_model(other_model, ThinkingLevel::medium));
  } catch (const std::runtime_error &error) {
    rejected_while_streaming = true;
    rejection_message = error.what();
  }
  EXPECT_TRUE(rejected_while_streaming);
  EXPECT_TRUE(rejection_message.find("idle") != std::string::npos);
  // Rejection must not have applied the switch.
  EXPECT_EQ(agent.state().model().id, std::string("test-model"));

  release->store(true);
  for (auto &event : stream) {
    if (std::holds_alternative<AgentEndEvent>(event))
      break;
  }
  agent.wait_for_idle();
  EXPECT_TRUE(!agent.is_streaming());

  const auto switched = agent.set_model(other_model, ThinkingLevel::medium);
  EXPECT_EQ(switched.current.id, std::string("other-model"));
  EXPECT_EQ(agent.state().model().id, std::string("other-model"));
}

TEST(Agent, SessionRuntime_runs_and_persists_through_shared_runtime) {
  LLMClientRegistry::instance().register_client(
      "agent-session-test", [] { return std::make_shared<ImmediateClient>(); });

  const auto session_dir =
      std::filesystem::temp_directory_path() /
      ("pici-agent-session-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));

  auto store = std::make_shared<SessionStore>(session_dir);
  Agent::Options opts;
  opts.model.id = "test-model";
  opts.model.api = "agent-session-test";
  opts.model.provider = "test";

  {
    SessionRuntime runtime({.agent_options = opts, .session_store = store});
    SessionHeader header;
    header.id = "runtime-session";
    header.created =
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    header.model = opts.model.id;
    header.provider = opts.model.provider;
    EXPECT_EQ(runtime.create_session(header), "runtime-session");

    int agent_end_count = 0;
    auto result = runtime.run_prompt(
        "hello", [&agent_end_count](const AgentEvent &event) {
          if (std::holds_alternative<AgentEndEvent>(event))
            ++agent_end_count;
        });

    EXPECT_TRUE(!result.error.has_value());
    EXPECT_EQ(agent_end_count, 1);
    EXPECT_EQ(runtime.agent().state().messages().size(), std::size_t(2));
  }

  auto saved = store->load("runtime-session");
  EXPECT_TRUE(saved.has_value());
  EXPECT_EQ(saved->messages.size(), std::size_t(2));
  EXPECT_TRUE(std::holds_alternative<UserMessage>(saved->messages[0]));
  EXPECT_TRUE(std::holds_alternative<AssistantMessage>(saved->messages[1]));
  store.reset();
  std::filesystem::remove_all(session_dir);
}

TEST(Agent, SessionRuntime_model_switch_persists_and_resumes) {
  LLMClientRegistry::instance().register_client(
      "agent-session-switch-test",
      [] { return std::make_shared<ImmediateClient>(); });

  ProviderConfig provider_a;
  provider_a.id = "provider-a";
  provider_a.api = "agent-session-switch-test";
  provider_a.base_url = "http://a.test/v1";
  provider_a.auth = ProviderAuthPolicy::none;
  ConfiguredModel a_model;
  a_model.id = "model-a";
  provider_a.models.push_back(a_model);

  ProviderConfig provider_b;
  provider_b.id = "provider-b";
  provider_b.api = "agent-session-switch-test";
  provider_b.base_url = "http://b.test/v1";
  provider_b.auth = ProviderAuthPolicy::none;
  ConfiguredModel b_model;
  b_model.id = "model-b";
  provider_b.models.push_back(b_model);

  auto registry = std::make_shared<const ModelCatalog>(
      std::map<std::string, ProviderConfig>{{"provider-a", provider_a},
                                            {"provider-b", provider_b}});
  const auto a = registry->resolve(
      {.provider = "provider-a", .model = "model-a", .source = "test"});
  const auto b = registry->resolve(
      {.provider = "provider-b", .model = "model-b", .source = "test"});
  EXPECT_TRUE(a);
  EXPECT_TRUE(b);

  const auto session_dir =
      std::filesystem::temp_directory_path() /
      ("pici-agent-switch-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  auto store = std::make_shared<SessionStore>(session_dir);
  Agent::Options opts;
  opts.model = *a.model;
  opts.model_catalog = registry;
  opts.thinking_level = ThinkingLevel::high;

  SessionRuntime runtime({.agent_options = opts,
                          .model_catalog = registry,
                          .session_store = store});
  SessionHeader header{
      .id = "switch-session", .model = "model-a", .provider = "provider-a"};
  runtime.create_session(header);
  const auto switched = runtime.set_model(*b.model, ThinkingLevel::high);
  EXPECT_EQ(switched.previous.provider, "provider-a");
  EXPECT_EQ(switched.current.provider, "provider-b");
  EXPECT_EQ(runtime.agent().state().model().id, "model-b");

  const auto saved = store->load("switch-session");
  EXPECT_TRUE(saved.has_value());
  EXPECT_EQ(saved->header.provider, "provider-b");
  EXPECT_EQ(saved->header.model, "model-b");

  SessionRuntime resumed({.agent_options = opts,
                          .model_catalog = registry,
                          .session_store = store});
  resumed.activate_session(*saved);
  EXPECT_EQ(resumed.agent().state().model().provider, "provider-b");
  EXPECT_EQ(resumed.agent().state().model().id, "model-b");

  std::filesystem::remove_all(session_dir);
}

// Lexicon invariant 13: "Credentials never enter transcripts, mailbox
// payloads, or presentation." A resolved secret handed to the agent through
// get_auth/get_api_key must not end up persisted in the session journal or
// observed on the AgentEvent stream after an ordinary run. See
// plans/session-runtime-migration.md Phase 1 item 6.

TEST(Agent,
     SessionRuntime_resolved_credentials_never_appear_in_transcript_or_events) {
  const std::string secret = "sk-test-super-secret-credential-value";
  LLMClientRegistry::instance().register_client("agent-credentials-test", [] {
    return std::make_shared<ImmediateClient>();
  });

  const auto session_dir =
      std::filesystem::temp_directory_path() /
      ("pici-agent-credentials-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  auto store = std::make_shared<SessionStore>(session_dir);

  Agent::Options opts;
  opts.model.id = "test-model";
  opts.model.api = "agent-credentials-test";
  opts.model.provider = "test";
  opts.get_auth = [secret](std::string_view) -> std::optional<RequestAuth> {
    return RequestAuth{.bearer_token = secret};
  };
  opts.get_api_key = [secret](std::string_view) -> std::optional<std::string> {
    return secret;
  };

  SessionRuntime runtime({.agent_options = opts, .session_store = store});
  SessionHeader header;
  header.id = "credentials-session";
  header.created =
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  header.model = opts.model.id;
  header.provider = opts.model.provider;
  runtime.create_session(header);

  std::string observed_event_text;
  const auto result = runtime.run_prompt("hello", [&](const AgentEvent &event) {
    observed_event_text += event_to_json(event).dump();
  });
  EXPECT_TRUE(!result.error.has_value());
  EXPECT_TRUE(observed_event_text.find(secret) == std::string::npos);

  const auto saved = store->load("credentials-session");
  EXPECT_TRUE(saved.has_value());
  for (const auto &message : saved->messages) {
    if (const auto *assistant = std::get_if<AssistantMessage>(&message)) {
      for (const auto &block : assistant->content) {
        if (const auto *text = std::get_if<TextContent>(&block)) {
          EXPECT_TRUE(text->text.find(secret) == std::string::npos);
        }
      }
    }
  }

  std::filesystem::remove_all(session_dir);
}

TEST(Agent, SessionRuntime_create_session_clears_existing_messages) {
  LLMClientRegistry::instance().register_client(
      "agent-session-test", [] { return std::make_shared<ImmediateClient>(); });

  const auto session_dir =
      std::filesystem::temp_directory_path() /
      ("pici-agent-create-clear-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));

  auto store = std::make_shared<SessionStore>(session_dir);
  Agent::Options opts;
  opts.model.id = "test-model";
  opts.model.api = "agent-session-test";
  opts.model.provider = "test";

  {
    SessionRuntime runtime({.agent_options = opts, .session_store = store});

    SessionHeader header;
    header.id = "first-session";
    header.created =
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    header.model = opts.model.id;
    header.provider = opts.model.provider;
    EXPECT_EQ(runtime.create_session(header), "first-session");

    // Run a prompt to add messages
    auto result = runtime.run_prompt("hello", [](const AgentEvent &) {});
    EXPECT_TRUE(!result.error.has_value());
    EXPECT_EQ(runtime.agent().state().messages().size(), std::size_t(2));

    // Now create a fresh session — messages should be cleared
    SessionHeader fresh;
    fresh.id = "fresh-session";
    fresh.created =
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    fresh.model = opts.model.id;
    fresh.provider = opts.model.provider;
    EXPECT_EQ(runtime.create_session(fresh), "fresh-session");

    EXPECT_EQ(runtime.agent().state().messages().size(), std::size_t(0));
    EXPECT_EQ(runtime.active_session_id().has_value(), true);
    EXPECT_EQ(*runtime.active_session_id(), "fresh-session");
  }

  store.reset();
  std::filesystem::remove_all(session_dir);
}

// ─── Main ──────────────────────────────────────────────────────────────────
