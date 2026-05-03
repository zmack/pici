#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <source_location>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "core/agent_loop.h"
#include "core/agent_state.h"
#include "core/event_types.h"
#include "core/llm_client.h"
#include "core/message_types.h"
#include "core/stream.h"

using namespace pi::core;

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
                  << " - " << expr << " (expected " << expected << ", got " << cond << ")\n";
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

#define CHECK_NEQ(a, b) \
    (::tests::CHECK_impl((a) != (b), true, #a " != " #b, \
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

// ─── Test tool: a simple counter tool ─────────────────────────────────────

class CounterTool : public ToolDefinition {
public:
    CounterTool() = default;

    std::string_view name() const override { return "counter"; }
    std::string_view description() const override {
        return "Returns a counter value";
    }

    class CounterSchema : public ToolSchema {
    public:
        std::string serialize() const override {
            return R"({
                "type": "object",
                "properties": {
                    "start": {"type": "integer"},
                    "count": {"type": "integer"}
                },
                "required": ["start"]
            })";
        }
        std::map<std::string, std::string> to_definition() const override {
            return {{"type", "object"},
                    {"properties",
                     R"({"start": {"type": "integer"}})"},
                    {"required", R"(["start"])"}};
        }
    };

    ToolSchema& schema() override {
        if (!schema_) {
            schema_ = std::make_unique<CounterSchema>();
        }
        return *schema_;
    }

    class CounterResult : public ToolResult {
    public:
        CounterResult(int value, bool error = false)
            : value_(value), error_(error) {}

        bool is_error() const override { return error_; }
        std::string content() const override {
            return "Counter: " + std::to_string(value_);
        }
        std::optional<std::string> details() const override {
            return std::to_string(value_);
        }

    private:
        int value_;
        bool error_;
    };

    std::shared_ptr<ToolResult> execute(
        std::string_view call_id,
        std::string_view args_json,
        std::stop_token) override {
        (void)call_id;
        (void)args_json;
        int start = 0;
        auto pos = args_json.find("\"start\"");
        if (pos != std::string_view::npos) {
            auto val_pos = args_json.find(':', pos);
            if (val_pos != std::string_view::npos) {
                try {
                    start = std::stoi(std::string(args_json.substr(
                        val_pos + 1,
                        std::min((size_t)10,
                                 args_json.find_first_of(",}", val_pos + 1) -
                                     val_pos - 1))));
                } catch (...) {
                    start = 0;
                }
            }
        }
        return std::make_shared<CounterResult>(start + 1);
    }

private:
    std::unique_ptr<ToolSchema> schema_;
};

// ─── Test LLM client: returns a pre-programmed response ───────────────────

class TestLLMClient : public LLMClient {
public:
    TestLLMClient(
        std::function<std::shared_ptr<AssistantMessage>(
            const AgentContext&, ThinkingLevel, StreamCallback, std::stop_token)>
            factory)
        : factory_(std::move(factory)) {}

    std::shared_ptr<AssistantMessage> stream(
        const Model& model,
        const AgentContext& context,
        ThinkingLevel thinking_level,
        StreamCallback emit,
        const std::optional<std::string>& api_key,
        std::stop_token stop_tok) override {
        (void)model;
        (void)api_key;
        return factory_(context, thinking_level, emit, stop_tok);
    }

    std::string_view provider_name() const override { return "test"; }
    std::string_view api_id() const override { return "test"; }

private:
    std::function<std::shared_ptr<AssistantMessage>(
        const AgentContext&, ThinkingLevel, StreamCallback, std::stop_token)>
        factory_;
};

// ─── Test agent loop: single turn, no tool calls ──────────────────────────

void test_agent_loop_single_turn() {
    tests::register_test("Agent loop: single turn, no tools", []() {
        Model model;
        model.id = "test-model";
        model.name = "Test";
        model.api = "test";
        model.provider = "test";

        int message_events = 0;
        bool has_start = false;
        bool has_end = false;

        auto llm_client = std::make_shared<TestLLMClient>(
            [&message_events, &has_start, &has_end](
                const AgentContext& context,
                ThinkingLevel,
                StreamCallback emit,
                std::stop_token stop_tok) -> std::shared_ptr<AssistantMessage> {
            (void)context;
            (void)stop_tok;

            has_start = true;
            emit(TurnStartEvent());

            auto msg = std::make_shared<AssistantMessage>();
            msg->api = "test";
            msg->provider = "test";
            msg->model = "test-model";
            msg->stop_reason = StopReason::stop;
            msg->timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();

            TextContent tc;
            tc.text = "Hello!";
            msg->content.push_back(std::move(tc));

            emit(MessageStartEvent(*msg, std::source_location::current()));
            emit(MessageEndEvent(*msg, std::source_location::current()));
            message_events += 2;

            return msg;
        });

        AgentContext ctx;
        ctx.system_prompt = "You are helpful";
        UserMessage user_msg;
        user_msg.timestamp = 1;
        TextContent uc;
        uc.text = "Hi there";
        user_msg.content.push_back(std::move(uc));
        ctx.messages.push_back(std::move(user_msg));

        AgentLoopConfig config;
        config.model = model;
        config.llm_client = llm_client;
        config.convert_to_llm = [](const std::vector<Message>& msgs) {
            return msgs;
        };
        config.should_stop_after_turn = [](const Message&,
                                           const std::vector<ToolResultMessage>&,
                                           AgentContext&) {
            return true;
        };
        config.get_steering_messages = [] { return std::vector<Message>{}; };
        config.get_follow_up_messages = [] { return std::vector<Message>{}; };

        auto stream = run_agent_loop({}, ctx, config,
                                     [](const AgentEvent&) {});

        for (auto& : stream) {
        }

        CHECK(message_events >= 2);
        CHECK(has_start);
    });
}

// ─── Test agent loop: with tool calls ─────────────────────────────────────

void test_agent_loop_with_tools() {
    tests::register_test("Agent loop: assistant returns tool call", []() {
        Model model;
        model.id = "test-model";
        model.api = "test";
        model.provider = "test";

        int turn_start_count = 0;
        int tool_start_count = 0;

        auto llm_client = std::make_shared<TestLLMClient>(
            [&tool_start_count](const AgentContext& context,
                                ThinkingLevel,
                                StreamCallback emit,
                                std::stop_token stop_tok)
                -> std::shared_ptr<AssistantMessage> {
                (void)context;
                (void)emit;
                (void)stop_tok;

                auto msg = std::make_shared<AssistantMessage>();
                msg->api = "test";
                msg->provider = "test";
                msg->model = "test-model";
                msg->stop_reason = StopReason::tool_use;
                msg->timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count();

                ToolCall tc;
                tc.id = "call_001";
                tc.name = "counter";
                tc.arguments[{"start"}] = "0";
                msg->content.push_back(std::move(tc));

                return msg;
            });

        AgentContext ctx;
        ctx.system_prompt = "test";
        UserMessage user_msg;
        user_msg.timestamp = 1;
        TextContent uc;
        uc.text = "Count to 5";
        user_msg.content.push_back(std::move(uc));
        ctx.messages.push_back(std::move(user_msg));
        ctx.tools.push_back(std::make_shared<CounterTool>());

        AgentLoopConfig config;
        config.model = model;
        config.llm_client = llm_client;
        config.convert_to_llm = [](const std::vector<Message>& msgs) { return msgs; };
        config.should_stop_after_turn = [](const Message&,
                                           const std::vector<ToolResultMessage>&,
                                           AgentContext&) {
            return true; // Stop after one turn
        };
        config.get_steering_messages = [] { return std::vector<Message>{}; };
        config.get_follow_up_messages = [] { return std::vector<Message>{}; };

        auto stream = run_agent_loop({}, ctx, config,
                                     [&turn_start_count, &tool_start_count](const AgentEvent& ev) {
                                         if (std::holds_alternative<TurnStartEvent>(ev)) {
                                             turn_start_count++;
                                         }
                                         if (auto* e = std::get_if<ToolExecutionStartEvent>(&ev)) {
                                             tool_start_count++;
                                         }
                                     });

        for (auto& : stream) {
        }

        CHECK(turn_start_count >= 1);
        CHECK(tool_start_count >= 1);
    });
}

// ─── Test agent loop: stop after turn ─────────────────────────────────────

void test_agent_loop_stop_after_turn() {
    tests::register_test("Agent loop: stop after one turn", []() {
        Model model;
        model.id = "test-model";
        model.api = "test";
        model.provider = "test";

        int turn_count = 0;

        auto llm_client = std::make_shared<TestLLMClient>(
            [&turn_count](const AgentContext& context,
                          ThinkingLevel,
                          StreamCallback emit,
                          std::stop_token stop_tok)
                -> std::shared_ptr<AssistantMessage> {
                turn_count++;
                (void)context;
                (void)emit;
                (void)stop_tok;

                auto msg = std::make_shared<AssistantMessage>();
                msg->api = "test";
                msg->provider = "test";
                msg->model = "test-model";
                msg->stop_reason = StopReason::stop;
                msg->timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count();
                TextContent tc;
                tc.text = "Response " + std::to_string(turn_count);
                msg->content.push_back(std::move(tc));
                return msg;
            });

        AgentContext ctx;
        ctx.system_prompt = "test";
        UserMessage user_msg;
        user_msg.timestamp = 1;
        TextContent uc;
        uc.text = "Hello";
        user_msg.content.push_back(std::move(uc));
        ctx.messages.push_back(std::move(user_msg));

        AgentLoopConfig config;
        config.model = model;
        config.llm_client = llm_client;
        config.convert_to_llm = [](const std::vector<Message>& msgs) { return msgs; };
        config.should_stop_after_turn = [](const Message&,
                                           const std::vector<ToolResultMessage>&,
                                           AgentContext&) {
            return true;
        };
        config.get_steering_messages = [] { return std::vector<Message>{}; };
        config.get_follow_up_messages = [] { return std::vector<Message>{}; };

        auto stream = run_agent_loop({}, ctx, config,
                                     [](const AgentEvent&) {});

        for (auto& : stream) {
        }

        CHECK_EQ(turn_count, 1);
    });
}

// ─── Test agent loop: multiple tool calls (sequential) ────────────────────

void test_agent_loop_sequential_tools() {
    tests::register_test("Agent loop: tool execution sequential mode", []() {
        Model model;
        model.id = "test-model";
        model.api = "test";
        model.provider = "test";

        auto llm_client = std::make_shared<TestLLMClient>(
            [](const AgentContext& context,
               ThinkingLevel,
               StreamCallback emit,
               std::stop_token stop_tok)
                -> std::shared_ptr<AssistantMessage> {
                (void)context;
                (void)emit;
                (void)stop_tok;

                auto msg = std::make_shared<AssistantMessage>();
                msg->api = "test";
                msg->provider = "test";
                msg->model = "test-model";
                msg->stop_reason = StopReason::tool_use;
                msg->timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count();

                ToolCall tc1;
                tc1.id = "call_1";
                tc1.name = "counter";
                tc1.arguments[{"start"}] = "0";
                msg->content.push_back(std::move(tc1));

                ToolCall tc2;
                tc2.id = "call_2";
                tc2.name = "counter";
                tc2.arguments[{"start"}] = "10";
                msg->content.push_back(std::move(tc2));

                return msg;
            });

        AgentContext ctx;
        ctx.system_prompt = "test";
        UserMessage user_msg;
        user_msg.timestamp = 1;
        TextContent uc;
        uc.text = "Call tools";
        user_msg.content.push_back(std::move(uc));
        ctx.messages.push_back(std::move(user_msg));
        ctx.tools.push_back(std::make_shared<CounterTool>());

        AgentLoopConfig config;
        config.model = model;
        config.llm_client = llm_client;
        config.tool_execution = ToolExecutionMode::sequential;
        config.convert_to_llm = [](const std::vector<Message>& msgs) { return msgs; };
        config.should_stop_after_turn = [](const Message&,
                                           const std::vector<ToolResultMessage>&,
                                           AgentContext&) {
            return true;
        };
        config.get_steering_messages = [] { return std::vector<Message>{}; };
        config.get_follow_up_messages = [] { return std::vector<Message>{}; };

        int tool_start_count = 0;
        auto stream = run_agent_loop({}, ctx, config,
                                     [&tool_start_count](const AgentEvent& ev) {
                                         if (auto* e = std::get_if<ToolExecutionStartEvent>(&ev)) {
                                             tool_start_count++;
                                         }
                                     });

        for (auto& : stream) {
        }

        CHECK_EQ(tool_start_count, 2);
    });
}

// ─── Test agent loop: no LLM client ──────────────────────────────────────

void test_agent_loop_no_llm_client() {
    tests::register_test("Agent loop: no LLM client produces error", []() {
        Model model;
        model.id = "test-model";
        model.api = "test";
        model.provider = "test";

        AgentContext ctx;
        ctx.system_prompt = "test";
        UserMessage user_msg;
        user_msg.timestamp = 1;
        TextContent uc;
        uc.text = "Hello";
        user_msg.content.push_back(std::move(uc));
        ctx.messages.push_back(std::move(user_msg));

        AgentLoopConfig config;
        config.model = model;
        config.llm_client = nullptr;
        config.convert_to_llm = [](const std::vector<Message>& msgs) { return msgs; };
        config.should_stop_after_turn = [](const Message&,
                                           const std::vector<ToolResultMessage>&,
                                           AgentContext&) {
            return true;
        };
        config.get_steering_messages = [] { return std::vector<Message>{}; };
        config.get_follow_up_messages = [] { return std::vector<Message>{}; };

        bool error_handled = false;

        auto stream = run_agent_loop({}, ctx, config,
                                     [&error_handled](const AgentEvent& ev) {
                                         if (auto* e = std::get_if<AgentEndEvent>(&ev)) {
                                             for (const auto& m : e->messages) {
                                                 if (auto* asm_ = std::get_if<AssistantMessage>(&m)) {
                                                     if (asm_->stop_reason == StopReason::error) {
                                                         error_handled = true;
                                                     }
                                                 }
                                             }
                                         }
                                     });

        for (auto& : stream) {
        }

        CHECK(error_handled);
    });
}

// ─── Test agent loop: continue ────────────────────────────────────────────

void test_agent_loop_continue() {
    tests::register_test("Agent loop: continue from context", []() {
        Model model;
        model.id = "test-model";
        model.api = "test";
        model.provider = "test";

        int turn_count = 0;

        auto llm_client = std::make_shared<TestLLMClient>(
            [&turn_count](const AgentContext& context,
                          ThinkingLevel,
                          StreamCallback emit,
                          std::stop_token stop_tok)
                -> std::shared_ptr<AssistantMessage> {
                (void)context;
                (void)emit;
                (void)stop_tok;
                turn_count++;

                auto msg = std::make_shared<AssistantMessage>();
                msg->api = "test";
                msg->provider = "test";
                msg->model = "test-model";
                msg->stop_reason = StopReason::stop;
                msg->timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count();
                TextContent tc;
                tc.text = "Turn " + std::to_string(turn_count);
                msg->content.push_back(std::move(tc));
                return msg;
            });

        AgentContext ctx;
        ctx.system_prompt = "test";
        UserMessage user_msg;
        user_msg.timestamp = 1;
        TextContent uc;
        uc.text = "Initial prompt";
        user_msg.content.push_back(std::move(uc));
        ctx.messages.push_back(std::move(user_msg));

        AgentLoopConfig config;
        config.model = model;
        config.llm_client = llm_client;
        config.convert_to_llm = [](const std::vector<Message>& msgs) { return msgs; };
        config.should_stop_after_turn = [](const Message&,
                                           const std::vector<ToolResultMessage>&,
                                           AgentContext&) {
            return false; // Continue
        };
        config.get_steering_messages = [] { return std::vector<Message>{}; };
        config.get_follow_up_messages = [] { return std::vector<Message>{}; };

        // First continuation
        auto stream1 = run_agent_loop_continue(ctx, config,
                                               [](const AgentEvent&) {});
        for (auto& : stream1) {
        }
        CHECK_EQ(turn_count, 1);

        // Add a message and continue again
        UserMessage msg2;
        msg2.timestamp = 2;
        TextContent uc2;
        uc2.text = "Follow up";
        msg2.content.push_back(std::move(uc2));
        ctx.messages.push_back(std::move(msg2));

        auto stream2 = run_agent_loop_continue(ctx, config,
                                               [](const AgentEvent&) {});
        for (auto& : stream2) {
        }
        CHECK_EQ(turn_count, 2);
    });
}

// ─── Main ──────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== pi-cpp agent loop tests ===\n\n";

    test_agent_loop_single_turn();
    test_agent_loop_with_tools();
    test_agent_loop_stop_after_turn();
    test_agent_loop_sequential_tools();
    test_agent_loop_no_llm_client();
    test_agent_loop_continue();

    tests::print_summary();

    return failed > 0 ? 1 : 0;
}
