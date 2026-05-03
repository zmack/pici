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

    ToolSchema& schema() const override {
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
        std::stop_token,
        ToolUpdateCallback) const override {
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
    mutable std::unique_ptr<ToolSchema> schema_;
};

class TrackingTool : public ToolDefinition {
public:
    explicit TrackingTool(std::atomic<int>& executions)
        : executions_(executions) {}

    std::string_view name() const override { return "tracking"; }
    std::string_view description() const override { return "Tracks calls"; }
    ToolSchema& schema() const override {
        if (!schema_) {
            class TrackingSchema : public CounterTool::CounterSchema {
            public:
                std::optional<std::string> validate_arguments(
                    ToolArguments& arguments) const override {
                    if (!arguments.contains("start")) {
                        return "Missing required argument: start";
                    }
                    return std::nullopt;
                }
            };
            schema_ = std::make_unique<TrackingSchema>();
        }
        return *schema_;
    }

    ToolArguments prepare_arguments(
        const ToolArguments& arguments) const override {
        auto prepared = arguments;
        if (!prepared.contains("start") && prepared.contains("alias_start")) {
            prepared["start"] = prepared["alias_start"];
        }
        return prepared;
    }

    class Result : public ToolResult {
    public:
        bool is_error() const override { return false; }
        std::string content() const override { return "executed"; }
        std::optional<std::string> details() const override {
            return "tracking-details";
        }
    };

    std::shared_ptr<ToolResult> execute(
        std::string_view,
        std::string_view,
        std::stop_token,
        ToolUpdateCallback) const override {
        executions_++;
        return std::make_shared<Result>();
    }

private:
    std::atomic<int>& executions_;
    mutable std::unique_ptr<ToolSchema> schema_;
};

// ─── Test LLM client: returns a pre-programmed response ───────────────────

class TestLLMClient : public LLMClient {
public:
    TestLLMClient(
        std::function<std::shared_ptr<AssistantMessage>(
            const AgentContext&, const StreamOptions&, StreamCallback, std::stop_token)>
            factory)
        : factory_(std::move(factory)) {}

    std::shared_ptr<AssistantMessage> stream(
        const Model& model,
        const AgentContext& context,
        const StreamOptions& options,
        StreamCallback emit,
        std::stop_token stop_tok) override {
        (void)model;
        return factory_(context, options, emit, stop_tok);
    }

    std::string_view provider_name() const override { return "test"; }
    std::string_view api_id() const override { return "test"; }

private:
    std::function<std::shared_ptr<AssistantMessage>(
        const AgentContext&, const StreamOptions&, StreamCallback, std::stop_token)>
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
                const StreamOptions&,
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

        for (auto& ev : stream) {
            (void)ev;
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
                                const StreamOptions&,
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
                tc.arguments["start"] = "0";
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

        for (auto& ev : stream) {
            (void)ev;
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
                          const StreamOptions&,
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

        for (auto& ev : stream) {
            (void)ev;
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
               const StreamOptions&,
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
                tc1.arguments["start"] = "0";
                msg->content.push_back(std::move(tc1));

                ToolCall tc2;
                tc2.id = "call_2";
                tc2.name = "counter";
                tc2.arguments["start"] = "10";
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

        for (auto& ev : stream) {
            (void)ev;
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

        for (auto& ev : stream) {
            (void)ev;
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
                          const StreamOptions&,
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
        for (auto& ev : stream1) {
            (void)ev;
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
        for (auto& ev : stream2) {
            (void)ev;
        }
        CHECK_EQ(turn_count, 2);
    });
}

void test_agent_loop_before_tool_call_blocks_with_reason() {
    tests::register_test("Agent loop: before_tool_call blocks with reason", []() {
        Model model;
        model.id = "test-model";
        model.api = "test";
        model.provider = "test";

        auto llm_client = std::make_shared<TestLLMClient>(
            [](const AgentContext&,
               const StreamOptions&,
               StreamCallback,
               std::stop_token) -> std::shared_ptr<AssistantMessage> {
                auto msg = std::make_shared<AssistantMessage>();
                msg->api = "test";
                msg->provider = "test";
                msg->model = "test-model";
                msg->stop_reason = StopReason::tool_use;

                ToolCall tc;
                tc.id = "call_blocked";
                tc.name = "tracking";
                tc.arguments["start"] = "0";
                msg->content.push_back(std::move(tc));
                return msg;
            });

        std::atomic<int> executions{0};
        AgentContext ctx;
        ctx.tools.push_back(std::make_shared<TrackingTool>(executions));

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
        config.before_tool_call =
            [](const BeforeToolCallContext& ctx, std::stop_token) {
                CHECK_EQ(ctx.tool_call.name, "tracking");
                return BeforeToolCallResult{true, "blocked by policy"};
            };

        std::vector<ToolResultMessage> tool_results;
        auto stream = run_agent_loop({}, ctx, config,
                                     [&tool_results](const AgentEvent& ev) {
                                         if (auto* e = std::get_if<TurnEndEvent>(&ev)) {
                                             tool_results = e->tool_results;
                                         }
                                     });
        for (auto& ev : stream) {
            (void)ev;
        }

        CHECK_EQ(executions.load(), 0);
        CHECK_EQ(tool_results.size(), std::size_t(1));
        CHECK(tool_results[0].is_error);
        CHECK_EQ(std::get<TextContent>(tool_results[0].content[0]).text,
                 "blocked by policy");
    });
}

void test_agent_loop_after_tool_call_partial_override() {
    tests::register_test("Agent loop: after_tool_call applies partial override", []() {
        Model model;
        model.id = "test-model";
        model.api = "test";
        model.provider = "test";

        auto llm_client = std::make_shared<TestLLMClient>(
            [](const AgentContext&,
               const StreamOptions&,
               StreamCallback,
               std::stop_token) -> std::shared_ptr<AssistantMessage> {
                auto msg = std::make_shared<AssistantMessage>();
                msg->api = "test";
                msg->provider = "test";
                msg->model = "test-model";
                msg->stop_reason = StopReason::tool_use;

                ToolCall tc;
                tc.id = "call_override";
                tc.name = "tracking";
                tc.arguments["start"] = "0";
                msg->content.push_back(std::move(tc));
                return msg;
            });

        std::atomic<int> executions{0};
        AgentContext ctx;
        ctx.tools.push_back(std::make_shared<TrackingTool>(executions));

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
        config.after_tool_call =
            [](const AfterToolCallContext& ctx, std::stop_token) {
                CHECK_EQ(ctx.result->content(), "executed");
                AfterToolCallResult result;
                result.content = std::vector<ToolResultContentBlock>{
                    TextContent{.text = "overridden"}};
                result.is_error = true;
                return result;
            };

        std::vector<ToolResultMessage> tool_results;
        auto stream = run_agent_loop({}, ctx, config,
                                     [&tool_results](const AgentEvent& ev) {
                                         if (auto* e = std::get_if<TurnEndEvent>(&ev)) {
                                             tool_results = e->tool_results;
                                         }
                                     });
        for (auto& ev : stream) {
            (void)ev;
        }

        CHECK_EQ(executions.load(), 1);
        CHECK_EQ(tool_results.size(), std::size_t(1));
        CHECK(tool_results[0].is_error);
        CHECK_EQ(std::get<TextContent>(tool_results[0].content[0]).text,
                 "overridden");
        CHECK_EQ(*tool_results[0].details, "tracking-details");
    });
}

void test_agent_loop_steering_after_turn_continues() {
    tests::register_test("Agent loop: steering after turn continues", []() {
        Model model;
        model.id = "test-model";
        model.api = "test";
        model.provider = "test";

        int turn_count = 0;
        auto llm_client = std::make_shared<TestLLMClient>(
            [&turn_count](const AgentContext&,
                          const StreamOptions&,
                          StreamCallback,
                          std::stop_token) -> std::shared_ptr<AssistantMessage> {
                turn_count++;
                auto msg = std::make_shared<AssistantMessage>();
                msg->api = "test";
                msg->provider = "test";
                msg->model = "test-model";
                msg->stop_reason = StopReason::stop;
                TextContent text;
                text.text = "turn " + std::to_string(turn_count);
                msg->content.push_back(std::move(text));
                return msg;
            });

        int steering_calls = 0;
        AgentContext ctx;
        AgentLoopConfig config;
        config.model = model;
        config.llm_client = llm_client;
        config.convert_to_llm = [](const std::vector<Message>& msgs) { return msgs; };
        config.should_stop_after_turn = [](const Message&,
                                           const std::vector<ToolResultMessage>&,
                                           AgentContext&) {
            return false;
        };
        config.get_steering_messages = [&steering_calls] {
            steering_calls++;
            if (steering_calls == 2) {
                UserMessage msg;
                TextContent text;
                text.text = "steer";
                msg.content.push_back(std::move(text));
                return std::vector<Message>{std::move(msg)};
            }
            return std::vector<Message>{};
        };
        config.get_follow_up_messages = [] { return std::vector<Message>{}; };

        auto stream = run_agent_loop({}, ctx, config,
                                     [](const AgentEvent&) {});
        for (auto& ev : stream) {
            (void)ev;
        }

        CHECK_EQ(turn_count, 2);
        CHECK_EQ(steering_calls, 3);
    });
}

void test_agent_loop_argument_validation_blocks_execution() {
    tests::register_test("Agent loop: argument validation blocks execution", []() {
        Model model;
        model.id = "test-model";
        model.api = "test";
        model.provider = "test";

        auto llm_client = std::make_shared<TestLLMClient>(
            [](const AgentContext&,
               const StreamOptions&,
               StreamCallback,
               std::stop_token) -> std::shared_ptr<AssistantMessage> {
                auto msg = std::make_shared<AssistantMessage>();
                msg->api = "test";
                msg->provider = "test";
                msg->model = "test-model";
                msg->stop_reason = StopReason::tool_use;

                ToolCall tc;
                tc.id = "call_invalid";
                tc.name = "tracking";
                msg->content.push_back(std::move(tc));
                return msg;
            });

        std::atomic<int> executions{0};
        AgentContext ctx;
        ctx.tools.push_back(std::make_shared<TrackingTool>(executions));

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

        std::vector<ToolResultMessage> tool_results;
        auto stream = run_agent_loop({}, ctx, config,
                                     [&tool_results](const AgentEvent& ev) {
                                         if (auto* e = std::get_if<TurnEndEvent>(&ev)) {
                                             tool_results = e->tool_results;
                                         }
                                     });
        for (auto& ev : stream) {
            (void)ev;
        }

        CHECK_EQ(executions.load(), 0);
        CHECK_EQ(tool_results.size(), std::size_t(1));
        CHECK(tool_results[0].is_error);
        CHECK_EQ(std::get<TextContent>(tool_results[0].content[0]).text,
                 "Missing required argument: start");
    });
}

void test_agent_loop_prepare_arguments_before_validation() {
    tests::register_test("Agent loop: prepare arguments before validation", []() {
        Model model;
        model.id = "test-model";
        model.api = "test";
        model.provider = "test";

        auto llm_client = std::make_shared<TestLLMClient>(
            [](const AgentContext&,
               const StreamOptions&,
               StreamCallback,
               std::stop_token) -> std::shared_ptr<AssistantMessage> {
                auto msg = std::make_shared<AssistantMessage>();
                msg->api = "test";
                msg->provider = "test";
                msg->model = "test-model";
                msg->stop_reason = StopReason::tool_use;

                ToolCall tc;
                tc.id = "call_prepared";
                tc.name = "tracking";
                tc.arguments["alias_start"] = "7";
                msg->content.push_back(std::move(tc));
                return msg;
            });

        std::atomic<int> executions{0};
        AgentContext ctx;
        ctx.tools.push_back(std::make_shared<TrackingTool>(executions));

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
        config.before_tool_call =
            [](const BeforeToolCallContext& ctx, std::stop_token) {
                CHECK(ctx.tool_call.arguments.contains("start"));
                CHECK_EQ(ctx.args_json.find("\"start\"") != std::string::npos,
                         true);
                return std::optional<BeforeToolCallResult>{};
            };

        auto stream = run_agent_loop({}, ctx, config,
                                     [](const AgentEvent&) {});
        for (auto& ev : stream) {
            (void)ev;
        }

        CHECK_EQ(executions.load(), 1);
    });
}

// ─── Test A: parallel completion order vs source order ────────────────────

class BlockingTool : public ToolDefinition {
public:
    explicit BlockingTool(std::string name,
                          std::atomic<bool>& release_flag,
                          std::atomic<int>& end_order_counter,
                          std::atomic<int>& my_end_index)
        : name_(std::move(name)),
          release_flag_(release_flag),
          end_order_counter_(end_order_counter),
          my_end_index_(my_end_index) {}

    std::string_view name() const override { return name_; }
    std::string_view description() const override { return "Blocking tool"; }
    ToolSchema& schema() const override {
        if (!schema_) {
            class S : public ToolSchema {
            public:
                std::string serialize() const override { return "{}"; }
                std::map<std::string, std::string> to_definition() const override {
                    return {};
                }
            };
            schema_ = std::make_unique<S>();
        }
        return *schema_;
    }

    std::shared_ptr<ToolResult> execute(
        std::string_view,
        std::string_view,
        std::stop_token,
        ToolUpdateCallback) const override {
        while (!release_flag_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        my_end_index_.store(end_order_counter_.fetch_add(1));
        return std::make_shared<class R>(name_);
    }

    class R : public ToolResult {
    public:
        explicit R(std::string n) : name_(std::move(n)) {}
        bool is_error() const override { return false; }
        std::string content() const override { return "result:" + name_; }
        std::optional<std::string> details() const override { return std::nullopt; }
    private:
        std::string name_;
    };

private:
    std::string name_;
    std::atomic<bool>& release_flag_;
    std::atomic<int>& end_order_counter_;
    std::atomic<int>& my_end_index_;
    mutable std::unique_ptr<ToolSchema> schema_;
};

void test_parallel_completion_vs_source_order() {
    tests::register_test("Parallel: completion order for end events, source order for results", []() {
        Model model;
        model.id = "test-model";
        model.api = "test";
        model.provider = "test";

        std::atomic<bool> release_a{false};
        std::atomic<bool> release_b{false};
        std::atomic<int> end_order_counter{0};
        std::atomic<int> a_end_index{-1};
        std::atomic<int> b_end_index{-1};

        auto tool_a = std::make_shared<BlockingTool>("tool_a", release_a, end_order_counter, a_end_index);
        auto tool_b = std::make_shared<BlockingTool>("tool_b", release_b, end_order_counter, b_end_index);

        auto llm_client = std::make_shared<TestLLMClient>(
            [](const AgentContext&, const StreamOptions&, StreamCallback, std::stop_token)
                -> std::shared_ptr<AssistantMessage> {
                auto msg = std::make_shared<AssistantMessage>();
                msg->api = "test";
                msg->provider = "test";
                msg->model = "test-model";
                msg->stop_reason = StopReason::tool_use;

                ToolCall tc1;
                tc1.id = "call_a";
                tc1.name = "tool_a";
                msg->content.push_back(std::move(tc1));

                ToolCall tc2;
                tc2.id = "call_b";
                tc2.name = "tool_b";
                msg->content.push_back(std::move(tc2));

                return msg;
            });

        AgentContext ctx;
        ctx.tools.push_back(tool_a);
        ctx.tools.push_back(tool_b);

        AgentLoopConfig config;
        config.model = model;
        config.llm_client = llm_client;
        config.tool_execution = ToolExecutionMode::parallel;
        config.convert_to_llm = [](const std::vector<Message>& msgs) { return msgs; };
        config.should_stop_after_turn = [](const Message&,
                                           const std::vector<ToolResultMessage>&,
                                           AgentContext&) { return true; };
        config.get_steering_messages = [] { return std::vector<Message>{}; };
        config.get_follow_up_messages = [] { return std::vector<Message>{}; };

        std::mutex events_mu;
        std::vector<std::string> end_event_order;
        std::vector<std::string> message_start_order;
        std::vector<ToolResultMessage> turn_tool_results;

        auto stream = run_agent_loop({}, ctx, config,
            [&](const AgentEvent& ev) {
                if (auto* e = std::get_if<ToolExecutionEndEvent>(&ev)) {
                    std::lock_guard lock(events_mu);
                    end_event_order.push_back(e->tool_name);
                } else if (auto* e = std::get_if<MessageStartEvent>(&ev)) {
                    if (auto* trm = std::get_if<ToolResultMessage>(&e->message)) {
                        std::lock_guard lock(events_mu);
                        message_start_order.push_back(trm->tool_name);
                    }
                } else if (auto* e = std::get_if<TurnEndEvent>(&ev)) {
                    std::lock_guard lock(events_mu);
                    turn_tool_results = e->tool_results;
                }
            });

        // Release B first (completes before A), then A
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        release_b.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        release_a.store(true);

        for (auto& ev : stream) {
            (void)ev;
        }

        // B ends before A (completion order)
        CHECK_EQ(end_event_order.size(), std::size_t(2));
        CHECK_EQ(end_event_order[0], std::string("tool_b"));
        CHECK_EQ(end_event_order[1], std::string("tool_a"));

        // MessageStart events appear in source order A then B
        CHECK_EQ(message_start_order.size(), std::size_t(2));
        CHECK_EQ(message_start_order[0], std::string("tool_a"));
        CHECK_EQ(message_start_order[1], std::string("tool_b"));

        // TurnEndEvent.tool_results in source order
        CHECK_EQ(turn_tool_results.size(), std::size_t(2));
        CHECK_EQ(turn_tool_results[0].tool_name, std::string("tool_a"));
        CHECK_EQ(turn_tool_results[1].tool_name, std::string("tool_b"));
    });
}

// ─── Test B: mixed immediate + non-immediate preserves source order ──────

void test_parallel_mixed_immediate_source_order() {
    tests::register_test("Parallel: mixed immediate+non-immediate preserves source order", []() {
        Model model;
        model.id = "test-model";
        model.api = "test";
        model.provider = "test";

        auto llm_client = std::make_shared<TestLLMClient>(
            [](const AgentContext&, const StreamOptions&, StreamCallback, std::stop_token)
                -> std::shared_ptr<AssistantMessage> {
                auto msg = std::make_shared<AssistantMessage>();
                msg->api = "test";
                msg->provider = "test";
                msg->model = "test-model";
                msg->stop_reason = StopReason::tool_use;

                ToolCall tc1;
                tc1.id = "call_x1";
                tc1.name = "unknown_tool";
                msg->content.push_back(std::move(tc1));

                ToolCall tc2;
                tc2.id = "call_x2";
                tc2.name = "counter";
                tc2.arguments["start"] = "0";
                msg->content.push_back(std::move(tc2));

                ToolCall tc3;
                tc3.id = "call_x3";
                tc3.name = "unknown_tool2";
                msg->content.push_back(std::move(tc3));

                return msg;
            });

        AgentContext ctx;
        ctx.tools.push_back(std::make_shared<CounterTool>());

        AgentLoopConfig config;
        config.model = model;
        config.llm_client = llm_client;
        config.tool_execution = ToolExecutionMode::parallel;
        config.convert_to_llm = [](const std::vector<Message>& msgs) { return msgs; };
        config.should_stop_after_turn = [](const Message&,
                                           const std::vector<ToolResultMessage>&,
                                           AgentContext&) { return true; };
        config.get_steering_messages = [] { return std::vector<Message>{}; };
        config.get_follow_up_messages = [] { return std::vector<Message>{}; };

        std::vector<ToolResultMessage> turn_tool_results;
        auto stream = run_agent_loop({}, ctx, config,
            [&turn_tool_results](const AgentEvent& ev) {
                if (auto* e = std::get_if<TurnEndEvent>(&ev)) {
                    turn_tool_results = e->tool_results;
                }
            });
        for (auto& ev : stream) {
            (void)ev;
        }

        CHECK_EQ(turn_tool_results.size(), std::size_t(3));
        CHECK_EQ(turn_tool_results[0].tool_call_id, std::string("call_x1"));
        CHECK(turn_tool_results[0].is_error);
        CHECK_EQ(turn_tool_results[1].tool_call_id, std::string("call_x2"));
        CHECK(!turn_tool_results[1].is_error);
        CHECK_EQ(turn_tool_results[2].tool_call_id, std::string("call_x3"));
        CHECK(turn_tool_results[2].is_error);
    });
}

// ─── Test C: per-tool sequential override ────────────────────────────────

class SequentialTool : public ToolDefinition {
public:
    explicit SequentialTool(std::string name, std::atomic<int>& counter)
        : name_(std::move(name)), counter_(counter) {}

    std::string_view name() const override { return name_; }
    std::string_view description() const override { return "Sequential tool"; }
    ToolExecutionMode execution_mode() const override { return ToolExecutionMode::sequential; }

    ToolSchema& schema() const override {
        if (!schema_) {
            class S : public ToolSchema {
            public:
                std::string serialize() const override { return "{}"; }
                std::map<std::string, std::string> to_definition() const override {
                    return {};
                }
            };
            schema_ = std::make_unique<S>();
        }
        return *schema_;
    }

    std::shared_ptr<ToolResult> execute(
        std::string_view,
        std::string_view,
        std::stop_token,
        ToolUpdateCallback) const override {
        entry_counter_ = counter_.fetch_add(1);
        return std::make_shared<class R>(entry_counter_);
    }

    int entry_counter() const { return entry_counter_; }

    class R : public ToolResult {
    public:
        explicit R(int v) : v_(v) {}
        bool is_error() const override { return false; }
        std::string content() const override { return std::to_string(v_); }
        std::optional<std::string> details() const override { return std::nullopt; }
    private:
        int v_;
    };

private:
    std::string name_;
    std::atomic<int>& counter_;
    mutable int entry_counter_{-1};
    mutable std::unique_ptr<ToolSchema> schema_;
};

void test_per_tool_sequential_override() {
    tests::register_test("Per-tool sequential: forces sequential batch with parallel config", []() {
        Model model;
        model.id = "test-model";
        model.api = "test";
        model.provider = "test";

        std::atomic<int> global_counter{0};
        auto tool_first = std::make_shared<SequentialTool>("seq_first", global_counter);
        auto tool_second = std::make_shared<SequentialTool>("seq_second", global_counter);

        auto llm_client = std::make_shared<TestLLMClient>(
            [](const AgentContext&, const StreamOptions&, StreamCallback, std::stop_token)
                -> std::shared_ptr<AssistantMessage> {
                auto msg = std::make_shared<AssistantMessage>();
                msg->api = "test";
                msg->provider = "test";
                msg->model = "test-model";
                msg->stop_reason = StopReason::tool_use;

                ToolCall tc1;
                tc1.id = "seq_call_1";
                tc1.name = "seq_first";
                msg->content.push_back(std::move(tc1));

                ToolCall tc2;
                tc2.id = "seq_call_2";
                tc2.name = "seq_second";
                msg->content.push_back(std::move(tc2));

                return msg;
            });

        AgentContext ctx;
        ctx.tools.push_back(tool_first);
        ctx.tools.push_back(tool_second);

        AgentLoopConfig config;
        config.model = model;
        config.llm_client = llm_client;
        config.tool_execution = ToolExecutionMode::parallel;
        config.convert_to_llm = [](const std::vector<Message>& msgs) { return msgs; };
        config.should_stop_after_turn = [](const Message&,
                                           const std::vector<ToolResultMessage>&,
                                           AgentContext&) { return true; };
        config.get_steering_messages = [] { return std::vector<Message>{}; };
        config.get_follow_up_messages = [] { return std::vector<Message>{}; };

        std::vector<std::string> end_order;
        std::mutex end_mu;
        auto stream = run_agent_loop({}, ctx, config,
            [&](const AgentEvent& ev) {
                if (auto* e = std::get_if<ToolExecutionEndEvent>(&ev)) {
                    std::lock_guard lock(end_mu);
                    end_order.push_back(e->tool_name);
                }
            });
        for (auto& ev : stream) {
            (void)ev;
        }

        // Sequential: first completes before second starts
        CHECK_EQ(tool_first->entry_counter(), 0);
        CHECK_EQ(tool_second->entry_counter(), 1);
        CHECK_EQ(end_order.size(), std::size_t(2));
        CHECK_EQ(end_order[0], std::string("seq_first"));
        CHECK_EQ(end_order[1], std::string("seq_second"));
    });
}

void test_stream_options_propagation() {
    tests::register_test("StreamOptions: fields propagated from AgentLoopConfig", []() {
        Model model;
        model.id = "test-model";
        model.api = "test";
        model.provider = "test";

        StreamOptions captured_opts;

        auto llm_client = std::make_shared<TestLLMClient>(
            [&captured_opts](const AgentContext&,
                             const StreamOptions& opts,
                             StreamCallback,
                             std::stop_token) -> std::shared_ptr<AssistantMessage> {
                captured_opts = opts;
                auto msg = std::make_shared<AssistantMessage>();
                msg->api = "test";
                msg->provider = "test";
                msg->model = "test-model";
                msg->stop_reason = StopReason::stop;
                return msg;
            });

        AgentContext ctx;
        AgentLoopConfig config;
        config.model = model;
        config.llm_client = llm_client;
        config.temperature = 0.7;
        config.max_tokens = std::uint32_t(2048);
        config.session_id = "abc";
        config.headers = {{"X-Test", "1"}};
        config.cache_retention = "long";
        config.convert_to_llm = [](const std::vector<Message>& msgs) { return msgs; };
        config.should_stop_after_turn = [](const Message&,
                                           const std::vector<ToolResultMessage>&,
                                           AgentContext&) { return true; };
        config.get_steering_messages = [] { return std::vector<Message>{}; };
        config.get_follow_up_messages = [] { return std::vector<Message>{}; };

        auto stream = run_agent_loop({}, ctx, config, [](const AgentEvent&) {});
        for (auto& ev : stream) { (void)ev; }

        CHECK(captured_opts.temperature.has_value());
        CHECK_EQ(*captured_opts.temperature, 0.7);
        CHECK(captured_opts.max_tokens.has_value());
        CHECK_EQ(*captured_opts.max_tokens, std::uint32_t(2048));
        CHECK(captured_opts.session_id.has_value());
        CHECK_EQ(*captured_opts.session_id, std::string("abc"));
        CHECK_EQ(captured_opts.headers.size(), std::size_t(1));
        CHECK_EQ(captured_opts.headers.at("X-Test"), std::string("1"));
        CHECK(captured_opts.cache_retention.has_value());
        CHECK_EQ(*captured_opts.cache_retention, std::string("long"));
    });
}

void test_api_key_resolution() {
    tests::register_test("StreamOptions: api_key resolved from get_api_key callback", []() {
        Model model;
        model.id = "test-model";
        model.api = "test";
        model.provider = "test";

        std::optional<std::string> captured_api_key;

        auto llm_client = std::make_shared<TestLLMClient>(
            [&captured_api_key](const AgentContext&,
                                const StreamOptions& opts,
                                StreamCallback,
                                std::stop_token) -> std::shared_ptr<AssistantMessage> {
                captured_api_key = opts.api_key;
                auto msg = std::make_shared<AssistantMessage>();
                msg->api = "test";
                msg->provider = "test";
                msg->model = "test-model";
                msg->stop_reason = StopReason::stop;
                return msg;
            });

        AgentContext ctx;
        AgentLoopConfig config;
        config.model = model;
        config.llm_client = llm_client;
        config.get_api_key = [](std::string_view) -> std::optional<std::string> {
            return "key-from-callback";
        };
        config.convert_to_llm = [](const std::vector<Message>& msgs) { return msgs; };
        config.should_stop_after_turn = [](const Message&,
                                           const std::vector<ToolResultMessage>&,
                                           AgentContext&) { return true; };
        config.get_steering_messages = [] { return std::vector<Message>{}; };
        config.get_follow_up_messages = [] { return std::vector<Message>{}; };

        auto stream = run_agent_loop({}, ctx, config, [](const AgentEvent&) {});
        for (auto& ev : stream) { (void)ev; }

        CHECK(captured_api_key.has_value());
        CHECK_EQ(*captured_api_key, std::string("key-from-callback"));
    });
}

void test_reasoning_forwarded() {
    tests::register_test("StreamOptions: thinking_level forwarded as reasoning", []() {
        Model model;
        model.id = "test-model";
        model.api = "test";
        model.provider = "test";

        ThinkingLevel captured_reasoning{ThinkingLevel::off};

        auto llm_client = std::make_shared<TestLLMClient>(
            [&captured_reasoning](const AgentContext&,
                                  const StreamOptions& opts,
                                  StreamCallback,
                                  std::stop_token) -> std::shared_ptr<AssistantMessage> {
                captured_reasoning = opts.reasoning;
                auto msg = std::make_shared<AssistantMessage>();
                msg->api = "test";
                msg->provider = "test";
                msg->model = "test-model";
                msg->stop_reason = StopReason::stop;
                return msg;
            });

        AgentContext ctx;
        AgentLoopConfig config;
        config.model = model;
        config.llm_client = llm_client;
        config.thinking_level = ThinkingLevel::high;
        config.convert_to_llm = [](const std::vector<Message>& msgs) { return msgs; };
        config.should_stop_after_turn = [](const Message&,
                                           const std::vector<ToolResultMessage>&,
                                           AgentContext&) { return true; };
        config.get_steering_messages = [] { return std::vector<Message>{}; };
        config.get_follow_up_messages = [] { return std::vector<Message>{}; };

        auto stream = run_agent_loop({}, ctx, config, [](const AgentEvent&) {});
        for (auto& ev : stream) { (void)ev; }

        CHECK_EQ(captured_reasoning, ThinkingLevel::high);
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
    test_agent_loop_before_tool_call_blocks_with_reason();
    test_agent_loop_after_tool_call_partial_override();
    test_agent_loop_steering_after_turn_continues();
    test_agent_loop_argument_validation_blocks_execution();
    test_agent_loop_prepare_arguments_before_validation();
    test_parallel_completion_vs_source_order();
    test_parallel_mixed_immediate_source_order();
    test_per_tool_sequential_override();
    test_stream_options_propagation();
    test_api_key_resolution();
    test_reasoning_forwarded();

    tests::print_summary();

    return tests::failed > 0 ? 1 : 0;
}
