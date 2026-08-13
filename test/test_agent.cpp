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
#include <vector>

#include "core/agent.h"
#include "core/agent_state.h"
#include "core/event_types.h"
#include "core/llm_client.h"
#include "core/message_types.h"
#include "core/session/agent_session.h"
#include "core/session/session_id.h"
#include "core/session/session_store.h"
#include "core/stream.h"

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
    ToolSchema& schema() const override { return schema_; }

    class Result : public ToolResult {
    public:
        bool is_error() const override { return false; }
        std::string content() const override { return "ok"; }
        std::optional<std::string> details() const override { return std::nullopt; }
    };

    std::shared_ptr<ToolResult> execute(
        std::string_view,
        std::string_view,
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

    std::shared_ptr<AssistantMessage> stream(
        const Model& model,
        const AgentContext&,
        const StreamOptions&,
        AssistantEventCallback,
        std::stop_token stop_tok) override {
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

class ImmediateClient : public LLMClient {
public:
    std::shared_ptr<AssistantMessage> stream(
        const Model& model,
        const AgentContext&,
        const StreamOptions&,
        AssistantEventCallback,
        std::stop_token) override {
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

// ─── Simple test harness ──────────────────────────────────────────────────

namespace tests {

int passed{0};
int failed{0};
int total{0};
int current_failed{0};

bool CHECK_impl(bool cond, bool expected,
                std::string_view expr,
                std::source_location loc = std::source_location::current()) {
    if (cond != expected) {
        current_failed++;
        std::cerr << "  FAIL " << loc.file_name() << ":" << loc.line()
                  << " - " << expr << "\n";
        return false;
    }
    return true;
}

#define CHECK(cond) \
    (::tests::CHECK_impl(static_cast<bool>(cond), true, #cond, \
                         std::source_location::current()))

#define CHECK_EQ(a, b) \
    (::tests::CHECK_impl((a) == (b), true, #a " == " #b, \
                         std::source_location::current()))

void register_test(std::string name, std::function<void()> fn) {
    total++;
    current_failed = 0;
    fn();
    if (current_failed == 0) {
        passed++;
    } else {
        failed++;
    }
}

void print_summary() {
    std::cout << "\n========================================\n";
    std::cout << "  Tests: " << total << " total, "
              << passed << " passed, "
              << failed << " failed\n";
    std::cout << "========================================\n";
}

} // namespace tests

// ─── Agent tests ──────────────────────────────────────────────────────────

void test_agent_default_construction() {
    tests::register_test("Agent: default construction", []() {
        Agent agent;

        CHECK(agent.state().system_prompt().empty());
        CHECK(!agent.state().is_streaming());
        CHECK(agent.state().messages().empty());
    });
}

void test_agent_with_options() {
    tests::register_test("Agent: construction with options", []() {
        Agent::Options opts;
        opts.system_prompt = "You are a helpful assistant";
        opts.model.id = "test-model";
        opts.model.name = "Test";
        opts.model.api = "test";
        opts.model.provider = "test";
        opts.thinking_level = ThinkingLevel::medium;
        opts.tool_execution = ToolExecutionMode::sequential;

        Agent agent(opts);

        CHECK(agent.state().system_prompt() == "You are a helpful assistant");
        CHECK(agent.state().model().id == "test-model");
        CHECK(agent.state().thinking_level() == ThinkingLevel::off);
    });
}

void test_agent_add_tool() {
    tests::register_test("Agent: add tool", []() {
        Agent agent;
        auto tool = std::make_shared<NamedTool>("echo");
        agent.add_tool(tool);

        CHECK_EQ(agent.state().tools().size(), std::size_t(1));
        CHECK_EQ(agent.state().tools()[0]->name(), "echo");
    });
}

void test_agent_set_tools() {
    tests::register_test("Agent: set tools", []() {
        Agent agent;
        auto tool1 = std::make_shared<NamedTool>("echo");
        auto tool2 = std::make_shared<NamedTool>("counter");

        agent.set_tools({tool1, tool2});
        CHECK_EQ(agent.state().tools().size(), std::size_t(2));
    });
}

void test_agent_steer() {
    tests::register_test("Agent: steer queue", []() {
        Agent agent;

        CHECK(agent.state().messages().empty());
        agent.steer({});
        CHECK(agent.state().messages().empty());
    });
}

void test_agent_session_switch_drops_envelope_callbacks() {
    tests::register_test("Agent: session switch drops envelope callbacks", []() {
        Agent agent;
        int accepted = 0;
        UserMessage message;
        message.content.push_back(TextContent{.text = "queued"});
        agent.steer_envelopes({AgentMessageEnvelope{
            .message = Message{std::move(message)},
            .on_accepted = [&accepted] { ++accepted; },
        }});

        agent.set_session_identity("new-session");

        CHECK_EQ(accepted, 0);
    });
}

void test_agent_follow_up() {
    tests::register_test("Agent: follow_up queue", []() {
        Agent agent;
        agent.follow_up({});
        CHECK(agent.state().messages().empty());
    });
}

void test_agent_reset() {
    tests::register_test("Agent: reset", []() {
        Agent agent;
        agent.set_tools({std::make_shared<NamedTool>("echo")});
        auto msg = UserMessage{};
        msg.timestamp = 1;
        TextContent tc;
        tc.text = "test";
        msg.content.push_back(std::move(tc));
        agent.state().set_messages({std::move(msg)});

        agent.reset();

        CHECK(agent.state().messages().empty());
        CHECK(agent.state().tools().empty());
        CHECK(!agent.state().is_streaming());
    });
}

void test_agent_prompt_stream() {
    tests::register_test("Agent: prompt creates stream", []() {
        Agent::Options opts;
        opts.model.id = "test-model";
        opts.model.api = "test";
        opts.model.provider = "test";

        Agent agent(opts);

        auto stream = agent.prompt("Hello");

        // Drain the stream until the async prompt turn completes.
        for (auto& ev : stream) {
            if (std::holds_alternative<AgentEndEvent>(ev)) {
                break;
            }
        }

        // AgentEndEvent is published before the worker thread flips
        // is_streaming() back to false, so wait for that explicitly instead
        // of racing it.
        agent.wait_for_idle();
        CHECK(!agent.state().is_streaming());
    });
}

void test_agent_run_lifecycle() {
    tests::register_test("Agent: abort and reuse run lifecycle", []() {
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
        } catch (const std::exception&) {
            rejected_concurrent_prompt = true;
        }
        CHECK(rejected_concurrent_prompt);

        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(1);
        while (calls->load() == 0 &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK_EQ(calls->load(), 1);

        agent.interrupt(TurnAbortReason::timeout);
        bool first_ended = false;
        int aborted_events = 0;
        std::optional<TurnAbortReason> abort_reason;
        for (auto& event : first) {
            if (std::holds_alternative<AgentEndEvent>(event)) {
                first_ended = true;
            }
            if (const auto *aborted = std::get_if<TurnAbortedEvent>(&event)) {
                ++aborted_events;
                abort_reason = aborted->reason;
            }
        }
        agent.wait_for_idle();
        CHECK(first_ended);
        CHECK_EQ(aborted_events, 1);
        CHECK(abort_reason == TurnAbortReason::timeout);
        CHECK(!agent.is_streaming());

        auto second = agent.prompt("second");
        bool second_ended = false;
        for (auto& event : second) {
            if (std::holds_alternative<AgentEndEvent>(event)) {
                second_ended = true;
            }
        }
        agent.wait_for_idle();

        CHECK(second_ended);
        CHECK(!agent.is_streaming());
        CHECK_EQ(calls->load(), 2);
        CHECK(!agent.state().error_message().has_value());
    });
}

void test_agent_session_runtime() {
    tests::register_test("AgentSession: runs and persists through shared runtime", []() {
        LLMClientRegistry::instance().register_client(
            "agent-session-test",
            [] { return std::make_shared<ImmediateClient>(); });

        const auto session_dir =
            std::filesystem::temp_directory_path() /
            ("pici-agent-session-" + std::to_string(
                                          std::chrono::steady_clock::now()
                                              .time_since_epoch()
                                              .count()));

        auto store = std::make_shared<SessionStore>(session_dir);
        Agent::Options opts;
        opts.model.id = "test-model";
        opts.model.api = "agent-session-test";
        opts.model.provider = "test";

        {
            AgentSession runtime({.agent_options = opts,
                                  .session_store = store});
            SessionHeader header;
            header.id = "runtime-session";
            header.created = std::chrono::system_clock::to_time_t(
                std::chrono::system_clock::now());
            header.model = opts.model.id;
            header.provider = opts.model.provider;
            CHECK_EQ(runtime.create_session(header), "runtime-session");

            int agent_end_count = 0;
            auto result = runtime.run_prompt(
                "hello", [&agent_end_count](const AgentEvent& event) {
                    if (std::holds_alternative<AgentEndEvent>(event))
                        ++agent_end_count;
                });

            CHECK(!result.error.has_value());
            CHECK_EQ(agent_end_count, 1);
            CHECK_EQ(runtime.agent().state().messages().size(), std::size_t(2));
        }

        auto saved = store->load("runtime-session");
        CHECK(saved.has_value());
        CHECK_EQ(saved->messages.size(), std::size_t(2));
        CHECK(std::holds_alternative<UserMessage>(saved->messages[0]));
        CHECK(std::holds_alternative<AssistantMessage>(saved->messages[1]));
        store.reset();
        std::filesystem::remove_all(session_dir);
    });
}

void test_agent_model_switch_and_resume() {
    tests::register_test("AgentSession: model switch persists and resumes", []() {
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

        auto registry = std::make_shared<const ModelRegistry>(
            std::map<std::string, ProviderConfig>{{"provider-a", provider_a},
                                                   {"provider-b", provider_b}});
        const auto a = registry->resolve(
            {.provider = "provider-a", .model = "model-a", .source = "test"});
        const auto b = registry->resolve(
            {.provider = "provider-b", .model = "model-b", .source = "test"});
        CHECK(a);
        CHECK(b);

        const auto session_dir =
            std::filesystem::temp_directory_path() /
            ("pici-agent-switch-" + std::to_string(
                                        std::chrono::steady_clock::now()
                                            .time_since_epoch()
                                            .count()));
        auto store = std::make_shared<SessionStore>(session_dir);
        Agent::Options opts;
        opts.model = *a.model;
        opts.model_registry = registry;
        opts.thinking_level = ThinkingLevel::high;

        AgentSession runtime({.agent_options = opts,
                              .model_registry = registry,
                              .session_store = store});
        SessionHeader header{.id = "switch-session", .model = "model-a",
                             .provider = "provider-a"};
        runtime.create_session(header);
        const auto switched = runtime.set_model(*b.model, ThinkingLevel::high);
        CHECK_EQ(switched.previous.provider, "provider-a");
        CHECK_EQ(switched.current.provider, "provider-b");
        CHECK_EQ(runtime.agent().state().model().id, "model-b");

        const auto saved = store->load("switch-session");
        CHECK(saved.has_value());
        CHECK_EQ(saved->header.provider, "provider-b");
        CHECK_EQ(saved->header.model, "model-b");

        AgentSession resumed({.agent_options = opts,
                              .model_registry = registry,
                              .session_store = store});
        resumed.activate_session(*saved);
        CHECK_EQ(resumed.agent().state().model().provider, "provider-b");
        CHECK_EQ(resumed.agent().state().model().id, "model-b");

        std::filesystem::remove_all(session_dir);
    });
}

void test_agent_session_create_session_clears() {
    tests::register_test("AgentSession: create_session clears existing messages", []() {
        LLMClientRegistry::instance().register_client(
            "agent-session-test",
            [] { return std::make_shared<ImmediateClient>(); });

        const auto session_dir =
            std::filesystem::temp_directory_path() /
            ("pici-agent-create-clear-" + std::to_string(
                                          std::chrono::steady_clock::now()
                                              .time_since_epoch()
                                              .count()));

        auto store = std::make_shared<SessionStore>(session_dir);
        Agent::Options opts;
        opts.model.id = "test-model";
        opts.model.api = "agent-session-test";
        opts.model.provider = "test";

        {
            AgentSession runtime({.agent_options = opts,
                                  .session_store = store});

            SessionHeader header;
            header.id = "first-session";
            header.created = std::chrono::system_clock::to_time_t(
                std::chrono::system_clock::now());
            header.model = opts.model.id;
            header.provider = opts.model.provider;
            CHECK_EQ(runtime.create_session(header), "first-session");

            // Run a prompt to add messages
            auto result = runtime.run_prompt("hello", [](const AgentEvent &) {});
            CHECK(!result.error.has_value());
            CHECK_EQ(runtime.agent().state().messages().size(), std::size_t(2));

            // Now create a fresh session — messages should be cleared
            SessionHeader fresh;
            fresh.id = "fresh-session";
            fresh.created = std::chrono::system_clock::to_time_t(
                std::chrono::system_clock::now());
            fresh.model = opts.model.id;
            fresh.provider = opts.model.provider;
            CHECK_EQ(runtime.create_session(fresh), "fresh-session");

            CHECK_EQ(runtime.agent().state().messages().size(), std::size_t(0));
            CHECK_EQ(runtime.active_session_id().has_value(), true);
            CHECK_EQ(*runtime.active_session_id(), "fresh-session");
        }

        store.reset();
        std::filesystem::remove_all(session_dir);
    });
}

// ─── Main ──────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== pi-cpp agent tests ===\n\n";

    test_agent_default_construction();
    test_agent_with_options();
    test_agent_add_tool();
    test_agent_set_tools();
    test_agent_steer();
    test_agent_session_switch_drops_envelope_callbacks();
    test_agent_follow_up();
    test_agent_reset();
    test_agent_prompt_stream();
    test_agent_run_lifecycle();
    test_agent_session_runtime();
    test_agent_model_switch_and_resume();
    test_agent_session_create_session_clears();

    tests::print_summary();

    return tests::failed > 0 ? 1 : 0;
}
