#include <atomic>
#include <chrono>
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
        CHECK(agent.state().thinking_level() == ThinkingLevel::medium);
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

        agent.abort();
        bool first_ended = false;
        for (auto& event : first) {
            if (std::holds_alternative<AgentEndEvent>(event)) {
                first_ended = true;
            }
        }
        agent.wait_for_idle();
        CHECK(first_ended);
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

// ─── Main ──────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== pi-cpp agent tests ===\n\n";

    test_agent_default_construction();
    test_agent_with_options();
    test_agent_add_tool();
    test_agent_set_tools();
    test_agent_steer();
    test_agent_follow_up();
    test_agent_reset();
    test_agent_prompt_stream();
    test_agent_run_lifecycle();

    tests::print_summary();

    return tests::failed > 0 ? 1 : 0;
}
