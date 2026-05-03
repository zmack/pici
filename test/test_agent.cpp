#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
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

        // Drain the stream
        for (auto& ev : stream) {
            if (auto* e = std::get_if<MessageEndEvent>(&ev)) {
                break;
            }
        }

        CHECK(!agent.state().is_streaming());
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

    tests::print_summary();

    return tests::failed > 0 ? 1 : 0;
}
