#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <source_location>
#include <stdexcept>
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

class ThrowingTool : public ToolDefinition {
public:
    std::string_view name() const override { return "throwing"; }
    std::string_view description() const override { return "Throws"; }
    ToolSchema& schema() const override {
        if (!schema_) {
            schema_ = std::make_unique<CounterTool::CounterSchema>();
        }
        return *schema_;
    }

    std::shared_ptr<ToolResult> execute(
        std::string_view,
        std::string_view,
        std::stop_token,
        ToolUpdateCallback) const override {
        throw std::runtime_error("tool exploded");
    }

private:
    mutable std::unique_ptr<ToolSchema> schema_;
};

// ─── Test LLM client: returns a pre-programmed response ───────────────────

class TestLLMClient : public LLMClient {
public:
    TestLLMClient(
        std::function<std::shared_ptr<AssistantMessage>(
            const AgentContext&, const StreamOptions&, AssistantEventCallback, std::stop_token)>
            factory)
        : factory_(std::move(factory)) {}

    std::shared_ptr<AssistantMessage> stream(
        const Model& model,
        const AgentContext& context,
        const StreamOptions& options,
        AssistantEventCallback on_event,
        std::stop_token stop_tok) override {
        (void)model;
        return factory_(context, options, on_event, stop_tok);
    }

    std::string_view provider_name() const override { return "test"; }
    std::string_view api_id() const override { return "test"; }

private:
    std::function<std::shared_ptr<AssistantMessage>(
        const AgentContext&, const StreamOptions&, AssistantEventCallback, std::stop_token)>
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

        auto llm_client = std::make_shared<TestLLMClient>(
            [&has_start](
                const AgentContext& context,
                const StreamOptions&,
                AssistantEventCallback,
                std::stop_token stop_tok) -> std::shared_ptr<AssistantMessage> {
            (void)context;
            (void)stop_tok;

            has_start = true;

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
            [&message_events](const AgentEvent& ev) {
                if (std::holds_alternative<MessageStartEvent>(ev) ||
                    std::holds_alternative<MessageEndEvent>(ev)) {
                    message_events++;
                }
            });

        for (auto& ev : stream) {
            (void)ev;
        }

        CHECK(message_events >= 2);
        CHECK(has_start);
    });
}

void test_effective_context_callback() {
    tests::register_test("Agent loop: publishes effective context before request", []() {
        Model model;
        model.id = "request-model";
        model.api = "request-api";
        model.provider = "request-provider";

        std::optional<AgentContext> observed;
        AgentContext sent_to_client;
        auto llm_client = std::make_shared<TestLLMClient>(
            [&sent_to_client](const AgentContext& context,
                              const StreamOptions&,
                              AssistantEventCallback,
                              std::stop_token) {
                sent_to_client = context;
                auto message = std::make_shared<AssistantMessage>();
                message->api = "request-api";
                message->provider = "request-provider";
                message->model = "request-model";
                message->stop_reason = StopReason::stop;
                message->content.emplace_back(TextContent{.text = "done"});
                return message;
            });

        AgentContext context;
        context.system_prompt = "raw system";
        context.model.id = "raw-model";
        context.tools.emplace_back(std::make_shared<CounterTool>());
        UserMessage user;
        user.content.emplace_back(TextContent{.text = "original"});
        context.messages.emplace_back(std::move(user));

        AgentLoopConfig config;
        config.model = model;
        config.llm_client = llm_client;
        config.transform_context = [](const std::vector<Message>& messages,
                                      std::stop_token) {
            auto result = messages;
            UserMessage transformed;
            transformed.content.emplace_back(TextContent{.text = "transformed"});
            result.emplace_back(std::move(transformed));
            return result;
        };
        config.convert_to_llm = [](const std::vector<Message>& messages) {
            auto result = messages;
            UserMessage converted;
            converted.content.emplace_back(TextContent{.text = "converted"});
            result.emplace_back(std::move(converted));
            return result;
        };
        config.on_effective_context =
            [&observed](const AgentContext& snapshot) { observed = snapshot; };
        config.should_stop_after_turn = [](const Message&,
                                           const std::vector<ToolResultMessage>&,
                                           AgentContext&) { return true; };
        config.get_steering_messages = [] { return std::vector<Message>{}; };
        config.get_follow_up_messages = [] { return std::vector<Message>{}; };

        auto stream = run_agent_loop({}, context, config, [](const AgentEvent&) {});
        for (auto& event : stream)
            (void)event;

        CHECK(observed.has_value());
        CHECK_EQ(observed->system_prompt, std::string("raw system"));
        CHECK_EQ(observed->model.id, std::string("request-model"));
        CHECK_EQ(observed->model.provider, std::string("request-provider"));
        CHECK_EQ(observed->messages.size(), std::size_t(3));
        CHECK_EQ(sent_to_client.messages.size(), std::size_t(3));
        CHECK_EQ(std::get<TextContent>(
                      std::get<UserMessage>(observed->messages[0]).content[0])
                      .text,
                  std::string("original"));
        CHECK_EQ(observed->tools.size(), std::size_t(1));
        CHECK_EQ(std::get<TextContent>(
                      std::get<UserMessage>(observed->messages[2]).content[0])
                      .text,
                  std::string("converted"));
    });
}

void test_request_local_runtime_identity() {
  tests::register_test("Agent loop: request-local runtime identity", []() {
    Model model;
    model.id = "identity-model";
    model.api = "test";
    model.provider = "test";

    AgentContext context;
    context.system_prompt = "stable system prompt";
    context.runtime_identity =
        AgentRuntimeIdentity{.agent_id = "agt_child",
                             .session_id = "sess_root",
                             .kind = "subagent",
                             .task_id = "task_7",
                             .task_path = "/root/research"};
    UserMessage persisted;
    persisted.content.emplace_back(TextContent{.text = "persisted"});
    context.messages.emplace_back(std::move(persisted));

    int prepare_calls = 0;
    int provider_calls = 0;
    std::vector<AgentContext> requests;
    std::vector<Message> final_messages;
    auto llm_client = std::make_shared<TestLLMClient>(
        [&provider_calls, &requests](const AgentContext &request,
                                     const StreamOptions &,
                                     AssistantEventCallback, std::stop_token) {
          requests.push_back(request);
          auto response = std::make_shared<AssistantMessage>();
          response->api = "test";
          response->provider = "test";
          response->model = "identity-model";
          if (provider_calls++ == 0) {
            response->stop_reason = StopReason::tool_use;
            response->content.emplace_back(
                ToolCall{.id = "call_identity",
                         .name = "counter",
                         .arguments = nlohmann::json{{"start", 1}}});
          } else {
            response->stop_reason = StopReason::stop;
            response->content.emplace_back(TextContent{.text = "complete"});
          }
          return response;
        });

    AgentLoopConfig config;
    config.model = model;
    config.llm_client = llm_client;
    config.prepare_context = [&prepare_calls](const AgentContext &raw,
                                              std::size_t, std::stop_token) {
      ++prepare_calls;
      for (const auto &message : raw.messages) {
        if (const auto *user = std::get_if<UserMessage>(&message)) {
          if (!user->content.empty()) {
            const auto *text = std::get_if<TextContent>(&user->content.front());
            CHECK(text == nullptr ||
                  !text->text.starts_with("[pici runtime context"));
          }
        }
      }
      auto prepared = raw.messages;
      auto *first_user = std::get_if<UserMessage>(&prepared.front());
      std::get<TextContent>(first_user->content.front()).text =
          "prepared transcript";
      return std::optional<std::vector<Message>>(std::move(prepared));
    };
    config.convert_to_llm = [](const std::vector<Message> &messages) {
      return messages;
    };
    config.should_stop_after_turn = [](const Message &assistant,
                                       const std::vector<ToolResultMessage> &,
                                       AgentContext &) {
      return !std::get<AssistantMessage>(assistant).content.empty() &&
             std::holds_alternative<TextContent>(
                 std::get<AssistantMessage>(assistant).content.front());
    };
    config.get_steering_messages = [] { return std::vector<Message>{}; };
    config.get_follow_up_messages = [] { return std::vector<Message>{}; };

    auto stream =
        run_agent_loop({}, context, config, [](const AgentEvent &) {});
    auto [result, error] = stream.wait();
    CHECK(!error.has_value());
    if (result)
      final_messages = *result;

    CHECK_EQ(prepare_calls, 2);
    CHECK_EQ(requests.size(), std::size_t(2));
    CHECK_EQ(requests[0].messages.size(), std::size_t(2));
    CHECK_EQ(requests[1].messages.size(), std::size_t(4));
    for (const auto &request : requests) {
      CHECK_EQ(request.system_prompt, std::string("stable system prompt"));
      const auto *identity = std::get_if<UserMessage>(&request.messages[0]);
      CHECK(identity != nullptr);
      CHECK_EQ(std::get<TextContent>(identity->content[0]).text,
               std::string("[pici runtime context; not user-authored]\n"
                           "mailbox agent_id=agt_child; session_id=sess_root; "
                           "kind=subagent;\n"
                           "task_id=task_7; task_path=/root/research\n"
                           "Use agents_self when you need the authoritative "
                           "structured identity."));
      CHECK_EQ(std::get<TextContent>(
                   std::get<UserMessage>(request.messages[1]).content[0])
                   .text,
               std::string("prepared transcript"));
    }

    CHECK_EQ(context.messages.size(), std::size_t(1));
    CHECK_EQ(std::get<TextContent>(
                 std::get<UserMessage>(context.messages[0]).content[0])
                 .text,
             std::string("persisted"));
    CHECK_EQ(final_messages.size(), std::size_t(3));
    for (const auto &message : final_messages) {
      if (const auto *user = std::get_if<UserMessage>(&message)) {
        if (!user->content.empty()) {
          const auto *text = std::get_if<TextContent>(&user->content.front());
          CHECK(text == nullptr ||
                !text->text.starts_with("[pici runtime context"));
        }
      }
    }
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
                                AssistantEventCallback,
                                std::stop_token stop_tok)
                -> std::shared_ptr<AssistantMessage> {
                (void)context;
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
                          AssistantEventCallback,
                          std::stop_token stop_tok)
                -> std::shared_ptr<AssistantMessage> {
                turn_count++;
                (void)context;
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
               AssistantEventCallback,
               std::stop_token stop_tok)
                -> std::shared_ptr<AssistantMessage> {
                (void)context;
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

void test_agent_loop_provider_exception() {
    tests::register_test("Agent loop: provider exception settles stream", []() {
        Model model;
        model.id = "test-model";
        model.api = "test";
        model.provider = "test";

        auto llm_client = std::make_shared<TestLLMClient>(
            [](const AgentContext&, const StreamOptions&,
               AssistantEventCallback, std::stop_token)
                -> std::shared_ptr<AssistantMessage> {
                throw std::runtime_error("provider exploded");
            });

        AgentLoopConfig config;
        config.model = model;
        config.llm_client = llm_client;
        config.convert_to_llm = [](const std::vector<Message>& messages) {
            return messages;
        };

        int agent_end_count = 0;
        std::optional<AssistantMessage> failure;
        auto stream = run_agent_loop(
            {}, AgentContext{}, config, [&agent_end_count, &failure](
                                           const AgentEvent& event) {
                if (const auto* end = std::get_if<AgentEndEvent>(&event)) {
                    agent_end_count++;
                    for (const auto& message : end->messages) {
                        if (const auto* assistant =
                                std::get_if<AssistantMessage>(&message)) {
                            failure = *assistant;
                        }
                    }
                }
            });

        for (auto& event : stream) {
            (void)event;
        }

        auto [result, error] = stream.wait();
        CHECK_EQ(agent_end_count, 1);
        CHECK(failure.has_value());
        CHECK_EQ(failure->stop_reason, StopReason::error);
        CHECK_EQ(failure->error_message,
                 std::optional<std::string>{"provider exploded"});
        CHECK(result.has_value());
        CHECK(!error.has_value());
    });
}

void test_agent_loop_tool_exception() {
    tests::register_test("Agent loop: tool exception becomes tool error", []() {
        Model model;
        model.id = "test-model";
        model.api = "test";
        model.provider = "test";

        auto llm_client = std::make_shared<TestLLMClient>(
            [](const AgentContext&, const StreamOptions&,
               AssistantEventCallback, std::stop_token)
                -> std::shared_ptr<AssistantMessage> {
                auto message = std::make_shared<AssistantMessage>();
                message->api = "test";
                message->provider = "test";
                message->model = "test-model";
                message->stop_reason = StopReason::tool_use;
                ToolCall call;
                call.id = "call_throwing";
                call.name = "throwing";
                call.arguments["start"] = 0;
                message->content.emplace_back(std::move(call));
                return message;
            });

        AgentContext context;
        context.tools.emplace_back(std::make_shared<ThrowingTool>());

        AgentLoopConfig config;
        config.model = model;
        config.llm_client = llm_client;
        config.tool_execution = ToolExecutionMode::sequential;
        config.convert_to_llm = [](const std::vector<Message>& messages) {
            return messages;
        };
        config.should_stop_after_turn = [](const Message&,
                                           const std::vector<ToolResultMessage>&,
                                           AgentContext&) { return true; };

        std::vector<ToolResultMessage> tool_results;
        auto stream = run_agent_loop(
            {}, context, config, [&tool_results](const AgentEvent& event) {
                if (const auto* turn_end = std::get_if<TurnEndEvent>(&event)) {
                    tool_results = turn_end->tool_results;
                }
            });

        for (auto& event : stream) {
            (void)event;
        }

        CHECK_EQ(tool_results.size(), std::size_t(1));
        CHECK(tool_results[0].is_error);
        CHECK_EQ(std::get<TextContent>(tool_results[0].content[0]).text,
                 "tool exploded");
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
                          AssistantEventCallback,
                          std::stop_token stop_tok)
                -> std::shared_ptr<AssistantMessage> {
                (void)context;
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
               AssistantEventCallback,
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
               AssistantEventCallback,
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
                          AssistantEventCallback,
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

void test_agent_loop_message_envelopes() {
    tests::register_test("Agent loop: message envelopes accept after append", []() {
        Model model;
        model.id = "test-model";
        model.api = "test";
        model.provider = "test";

        std::vector<std::string> order;
        int accepted = 0;
        auto llm_client = std::make_shared<TestLLMClient>(
            [&order](const AgentContext& context,
                     const StreamOptions&,
                     AssistantEventCallback,
                     std::stop_token) -> std::shared_ptr<AssistantMessage> {
                CHECK_EQ(context.messages.size(), std::size_t(2));
                CHECK(std::holds_alternative<UserMessage>(context.messages[0]));
                CHECK(std::holds_alternative<UserMessage>(context.messages[1]));
                auto msg = std::make_shared<AssistantMessage>();
                msg->api = "test";
                msg->provider = "test";
                msg->model = "test-model";
                msg->stop_reason = StopReason::stop;
                msg->content.emplace_back(TextContent{.text = "done"});
                order.push_back("llm");
                return msg;
            });

        auto make_message = [](std::string text) {
            UserMessage message;
            message.content.emplace_back(TextContent{.text = std::move(text)});
            return Message{std::move(message)};
        };
        std::vector<AgentMessageEnvelope> prompts;
        prompts.push_back({
            .message = make_message("one"),
            .on_accepted = [&order, &accepted] {
                CHECK_EQ(order.back(), std::string("end"));
                ++accepted;
                order.push_back("accepted");
                throw std::runtime_error("observer failure");
            }});
        prompts.push_back({
            .message = make_message("two"),
            .on_accepted = [&order, &accepted] {
                CHECK_EQ(order.back(), std::string("end"));
                ++accepted;
                order.push_back("accepted");
            }});

        AgentContext context;
        AgentLoopConfig config;
        config.model = model;
        config.llm_client = llm_client;
        config.convert_to_llm = [](const std::vector<Message>& messages) {
            return messages;
        };
        config.get_steering_envelopes = [] {
            return std::vector<AgentMessageEnvelope>{};
        };
        auto stream = run_agent_loop_envelopes(
            std::move(prompts), context, config, [&order](const AgentEvent& event) {
                if (const auto* end = std::get_if<MessageEndEvent>(&event)) {
                    if (std::holds_alternative<UserMessage>(end->message))
                        order.push_back("end");
                }
            });
        std::vector<Message> result;
        for (auto& event : stream) {
            if (const auto* end = std::get_if<AgentEndEvent>(&event))
                result = end->messages;
        }

        CHECK_EQ(accepted, 2);
        CHECK_EQ(result.size(), std::size_t(3));
        CHECK_EQ(order[0], std::string("end"));
        CHECK_EQ(order[1], std::string("accepted"));
        CHECK_EQ(order[2], std::string("end"));
        CHECK_EQ(order[3], std::string("accepted"));
        CHECK_EQ(order[4], std::string("llm"));
    });
}

void test_agent_loop_message_envelopes_cancelled() {
    tests::register_test("Agent loop: cancelled envelopes are not accepted", []() {
        int accepted = 0;
        AgentMessageEnvelope envelope;
        UserMessage message;
        message.content.emplace_back(TextContent{.text = "drop me"});
        envelope.message = Message{std::move(message)};
        envelope.on_accepted = [&accepted] { ++accepted; };

        std::stop_source stop_source;
        stop_source.request_stop();
        AgentLoopConfig config;
        config.model.id = "test-model";
        auto stream = run_agent_loop_envelopes(
            {std::move(envelope)}, {}, config, [](const AgentEvent&) {},
            stop_source.get_token());
        for (auto& event : stream)
            (void)event;
        CHECK_EQ(accepted, 0);
    });
}

void test_mailbox_envelope_waits_for_turn_boundary() {
    tests::register_test("Agent loop: mailbox envelope waits for turn boundary", []() {
        Model model{.id = "test-model", .api = "test", .provider = "test"};
        auto release = std::make_shared<std::promise<void>>();
        auto released = std::make_shared<std::shared_future<void>>(
            release->get_future().share());
        auto active = std::make_shared<std::promise<void>>();
        auto injected = std::make_shared<std::atomic<bool>>(false);
        auto calls = std::make_shared<std::atomic<int>>(0);
        std::vector<std::string> order;
        int accepted = 0;
        auto client = std::make_shared<TestLLMClient>(
            [=, &order](const AgentContext& context, const StreamOptions&,
                        AssistantEventCallback, std::stop_token)
                -> std::shared_ptr<AssistantMessage> {
                const auto call = calls->fetch_add(1) + 1;
                if (call == 1) {
                    active->set_value();
                    released->wait();
                } else {
                    bool found = false;
                    for (const auto& message : context.messages) {
                        if (const auto* user = std::get_if<UserMessage>(&message)) {
                            for (const auto& content : user->content) {
                                if (const auto* text = std::get_if<TextContent>(&content))
                                    found = found || text->text.find("mailbox-id") !=
                                                       std::string::npos;
                            }
                        }
                    }
                    CHECK(found);
                }
                auto result = std::make_shared<AssistantMessage>();
                result->api = "test";
                result->provider = "test";
                result->model = "test-model";
                result->stop_reason = StopReason::stop;
                result->content.emplace_back(TextContent{.text = "done"});
                return result;
            });
        AgentContext context;
        UserMessage prompt;
        prompt.content.emplace_back(TextContent{.text = "prompt"});
        AgentLoopConfig config;
        config.model = model;
        config.llm_client = client;
        config.convert_to_llm = [](const std::vector<Message>& messages) {
            return messages;
        };
        config.get_steering_envelopes = [injected, &accepted] {
            if (!injected->exchange(false))
                return std::vector<AgentMessageEnvelope>{};
            UserMessage message;
            message.content.emplace_back(TextContent{.text = "mailbox-id"});
            return std::vector<AgentMessageEnvelope>{AgentMessageEnvelope{
                .message = Message{std::move(message)},
                .on_accepted = [&accepted] { ++accepted; }}};
        };
        auto stream = run_agent_loop_envelopes(
            {AgentMessageEnvelope{.message = Message{std::move(prompt)}}},
            context, config, [&order](const AgentEvent& event) {
                if (const auto* end = std::get_if<MessageEndEvent>(&event)) {
                    if (std::holds_alternative<UserMessage>(end->message))
                        order.push_back(order.empty() ? "prompt_end"
                                                       : "mailbox_end");
                } else if (std::holds_alternative<TurnEndEvent>(event)) {
                    order.push_back("turn_end");
                }
            });
        active->get_future().wait();
        injected->store(true);
        release->set_value();
        for (auto& event : stream)
            (void)event;
        CHECK_EQ(accepted, 1);
        const auto first_turn_end =
            std::ranges::find(order, "turn_end");
        const auto mailbox_end =
            std::ranges::find(order, "mailbox_end");
        CHECK(first_turn_end != order.end());
        CHECK(mailbox_end != order.end());
        CHECK(mailbox_end > first_turn_end);
    });
}

void test_agent_loop_steering_envelopes() {
    tests::register_test("Agent loop: steering envelopes preserve order", []() {
        Model model;
        model.id = "test-model";
        model.api = "test";
        model.provider = "test";
        int getter_calls = 0;
        int accepted = 0;
        auto llm_client = std::make_shared<TestLLMClient>(
            [](const AgentContext& context,
               const StreamOptions&,
               AssistantEventCallback,
               std::stop_token) -> std::shared_ptr<AssistantMessage> {
                CHECK_EQ(context.messages.size(), std::size_t(1));
                const auto& user = std::get<UserMessage>(context.messages[0]);
                CHECK_EQ(std::get<TextContent>(user.content[0]).text,
                         std::string("steer"));
                auto result = std::make_shared<AssistantMessage>();
                result->api = "test";
                result->provider = "test";
                result->model = "test-model";
                result->stop_reason = StopReason::stop;
                return result;
            });
        AgentLoopConfig config;
        config.model = model;
        config.llm_client = llm_client;
        config.convert_to_llm = [](const std::vector<Message>& messages) {
            return messages;
        };
        config.get_steering_envelopes = [&getter_calls, &accepted] {
            ++getter_calls;
            if (getter_calls != 1)
                return std::vector<AgentMessageEnvelope>{};
            UserMessage message;
            message.content.emplace_back(TextContent{.text = "steer"});
            return std::vector<AgentMessageEnvelope>{
                {.message = Message{std::move(message)},
                 .on_accepted = [&accepted] { ++accepted; }}};
        };
        auto stream = run_agent_loop({}, {}, config, [](const AgentEvent&) {});
        for (auto& event : stream)
            (void)event;
        CHECK_EQ(accepted, 1);
        CHECK_EQ(getter_calls, 2);
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
               AssistantEventCallback,
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
               AssistantEventCallback,
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
    tests::register_test("Parallel: all events in source order (end events no longer completion-ordered)", []() {
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
            [](const AgentContext&, const StreamOptions&, AssistantEventCallback, std::stop_token)
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

        // Both end events and MessageStart events appear in source order
        // (A then B), NOT completion order.  The parallel path emits
        // ToolExecutionEndEvent after all futures complete, grouped with
        // MessageStart/MessageEnd, to keep the renderer's event stream
        // consistent with the sequential path.
        CHECK_EQ(end_event_order.size(), std::size_t(2));
        CHECK_EQ(end_event_order[0], std::string("tool_a"));
        CHECK_EQ(end_event_order[1], std::string("tool_b"));

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
            [](const AgentContext&, const StreamOptions&, AssistantEventCallback, std::stop_token)
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
            [](const AgentContext&, const StreamOptions&, AssistantEventCallback, std::stop_token)
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
                             AssistantEventCallback,
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
                                AssistantEventCallback,
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
                                  AssistantEventCallback,
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

void test_streaming_text_delta() {
    tests::register_test("Streaming: text delta events produce ordered MessageUpdateEvents", []() {
        Model model;
        model.id = "test-model";
        model.api = "test";
        model.provider = "test";

        auto llm_client = std::make_shared<TestLLMClient>(
            [](const AgentContext&,
               const StreamOptions&,
               AssistantEventCallback on_event,
               std::stop_token) -> std::shared_ptr<AssistantMessage> {
                AssistantMessage partial;
                partial.api = "test";
                partial.provider = "test";
                partial.model = "test-model";
                partial.stop_reason = StopReason::stop;

                on_event(AssistantMessageStartEvent{partial});
                on_event(AssistantMessageTextStartEvent{0, partial});

                partial.content.push_back(TextContent{.text = "a"});
                on_event(AssistantMessageTextDeltaEvent{0, "a", partial});
                partial.content[0] = TextContent{.text = "ab"};
                on_event(AssistantMessageTextDeltaEvent{0, "b", partial});
                partial.content[0] = TextContent{.text = "abc"};
                on_event(AssistantMessageTextDeltaEvent{0, "c", partial});

                on_event(AssistantMessageTextEndEvent{0, "abc", partial});

                auto final_msg = std::make_shared<AssistantMessage>(partial);
                final_msg->stop_reason = StopReason::stop;
                on_event(AssistantMessageDoneEvent{StopReason::stop, *final_msg});
                return final_msg;
            });

        AgentContext ctx;
        AgentLoopConfig config;
        config.model = model;
        config.llm_client = llm_client;
        config.convert_to_llm = [](const std::vector<Message>& msgs) { return msgs; };
        config.should_stop_after_turn = [](const Message&,
                                           const std::vector<ToolResultMessage>&,
                                           AgentContext&) { return true; };
        config.get_steering_messages = [] { return std::vector<Message>{}; };
        config.get_follow_up_messages = [] { return std::vector<Message>{}; };

        std::vector<std::string> deltas;
        bool has_message_end = false;
        auto stream = run_agent_loop({}, ctx, config,
            [&](const AgentEvent& ev) {
                if (auto* e = std::get_if<MessageUpdateEvent>(&ev)) {
                    if (auto* td = std::get_if<AssistantMessageTextDeltaEvent>(
                            &e->assistant_message_event)) {
                        deltas.push_back(td->delta);
                    }
                } else if (std::holds_alternative<MessageEndEvent>(ev)) {
                    has_message_end = true;
                }
            });
        for (auto& ev : stream) { (void)ev; }

        CHECK_EQ(deltas.size(), std::size_t(3));
        CHECK_EQ(deltas[0], std::string("a"));
        CHECK_EQ(deltas[1], std::string("b"));
        CHECK_EQ(deltas[2], std::string("c"));
        CHECK(has_message_end);
    });
}

void test_streaming_tool_call() {
    tests::register_test("Streaming: tool call stream produces correct ToolCall", []() {
        Model model;
        model.id = "test-model";
        model.api = "test";
        model.provider = "test";

        auto llm_client = std::make_shared<TestLLMClient>(
            [](const AgentContext&,
               const StreamOptions&,
               AssistantEventCallback on_event,
               std::stop_token) -> std::shared_ptr<AssistantMessage> {
                AssistantMessage partial;
                partial.api = "test";
                partial.provider = "test";
                partial.model = "test-model";
                partial.stop_reason = StopReason::tool_use;

                on_event(AssistantMessageStartEvent{partial});
                on_event(AssistantMessageToolCallStartEvent{0, partial});

                partial.content.push_back(ToolCall{.id = "tc1", .name = "counter", .partial_json = "{"});
                on_event(AssistantMessageToolCallDeltaEvent{0, "{", partial});

                ToolCall final_tc;
                final_tc.id = "tc1";
                final_tc.name = "counter";
                final_tc.arguments = nlohmann::json::object();
                final_tc.arguments["start"] = 5;
                partial.content[0] = final_tc;
                on_event(AssistantMessageToolCallEndEvent{0, final_tc, partial});

                auto final_msg = std::make_shared<AssistantMessage>(partial);
                final_msg->stop_reason = StopReason::tool_use;
                on_event(AssistantMessageDoneEvent{StopReason::tool_use, *final_msg});
                return final_msg;
            });

        AgentContext ctx;
        AgentLoopConfig config;
        config.model = model;
        config.llm_client = llm_client;
        config.convert_to_llm = [](const std::vector<Message>& msgs) { return msgs; };
        config.should_stop_after_turn = [](const Message&,
                                           const std::vector<ToolResultMessage>&,
                                           AgentContext&) { return true; };
        config.get_steering_messages = [] { return std::vector<Message>{}; };
        config.get_follow_up_messages = [] { return std::vector<Message>{}; };

        std::string captured_tool_name;
        std::string captured_tool_id;
        auto stream = run_agent_loop({}, ctx, config,
            [&](const AgentEvent& ev) {
                if (auto* e = std::get_if<MessageEndEvent>(&ev)) {
                    if (auto* am = std::get_if<AssistantMessage>(&e->message)) {
                        for (const auto& cb : am->content) {
                            if (auto* tc = std::get_if<ToolCall>(&cb)) {
                                captured_tool_name = tc->name;
                                captured_tool_id = tc->id;
                            }
                        }
                    }
                }
            });
        for (auto& ev : stream) { (void)ev; }

        CHECK_EQ(captured_tool_name, std::string("counter"));
        CHECK_EQ(captured_tool_id, std::string("tc1"));
    });
}

void test_streaming_error_path() {
    tests::register_test("Streaming: error event sets stop_reason and error_message", []() {
        Model model;
        model.id = "test-model";
        model.api = "test";
        model.provider = "test";

        auto llm_client = std::make_shared<TestLLMClient>(
            [](const AgentContext&,
               const StreamOptions&,
               AssistantEventCallback on_event,
               std::stop_token) -> std::shared_ptr<AssistantMessage> {
                AssistantMessage partial;
                partial.api = "test";
                partial.provider = "test";
                partial.model = "test-model";
                partial.stop_reason = StopReason::error;
                partial.error_message = "provider error";

                on_event(AssistantMessageStartEvent{partial});
                partial.content.push_back(TextContent{.text = "partial"});
                on_event(AssistantMessageTextDeltaEvent{0, "partial", partial});

                auto err_msg = std::make_shared<AssistantMessage>(partial);
                err_msg->stop_reason = StopReason::error;
                err_msg->error_message = "provider error";
                on_event(AssistantMessageErrorEvent{StopReason::error, *err_msg});
                return err_msg;
            });

        AgentContext ctx;
        AgentLoopConfig config;
        config.model = model;
        config.llm_client = llm_client;
        config.convert_to_llm = [](const std::vector<Message>& msgs) { return msgs; };
        config.should_stop_after_turn = [](const Message&,
                                           const std::vector<ToolResultMessage>&,
                                           AgentContext&) { return true; };
        config.get_steering_messages = [] { return std::vector<Message>{}; };
        config.get_follow_up_messages = [] { return std::vector<Message>{}; };

        StopReason captured_reason = StopReason::stop;
        bool has_error_message = false;
        auto stream = run_agent_loop({}, ctx, config,
            [&](const AgentEvent& ev) {
                if (auto* e = std::get_if<MessageEndEvent>(&ev)) {
                    if (auto* am = std::get_if<AssistantMessage>(&e->message)) {
                        captured_reason = am->stop_reason;
                        has_error_message = am->error_message.has_value();
                    }
                }
            });
        for (auto& ev : stream) { (void)ev; }

        CHECK_EQ(captured_reason, StopReason::error);
        CHECK(has_error_message);
    });
}

void test_streaming_done_stop_reason() {
    tests::register_test("Streaming: done event with tool_use stop reason", []() {
        Model model;
        model.id = "test-model";
        model.api = "test";
        model.provider = "test";

        auto llm_client = std::make_shared<TestLLMClient>(
            [](const AgentContext&,
               const StreamOptions&,
               AssistantEventCallback on_event,
               std::stop_token) -> std::shared_ptr<AssistantMessage> {
                AssistantMessage partial;
                partial.api = "test";
                partial.provider = "test";
                partial.model = "test-model";
                partial.stop_reason = StopReason::tool_use;

                on_event(AssistantMessageStartEvent{partial});

                auto final_msg = std::make_shared<AssistantMessage>(partial);
                final_msg->stop_reason = StopReason::tool_use;
                on_event(AssistantMessageDoneEvent{StopReason::tool_use, *final_msg});
                return final_msg;
            });

        AgentContext ctx;
        AgentLoopConfig config;
        config.model = model;
        config.llm_client = llm_client;
        config.convert_to_llm = [](const std::vector<Message>& msgs) { return msgs; };
        config.should_stop_after_turn = [](const Message&,
                                           const std::vector<ToolResultMessage>&,
                                           AgentContext&) { return true; };
        config.get_steering_messages = [] { return std::vector<Message>{}; };
        config.get_follow_up_messages = [] { return std::vector<Message>{}; };

        StopReason captured_reason = StopReason::stop;
        auto stream = run_agent_loop({}, ctx, config,
            [&](const AgentEvent& ev) {
                if (auto* e = std::get_if<MessageEndEvent>(&ev)) {
                    if (auto* am = std::get_if<AssistantMessage>(&e->message)) {
                        captured_reason = am->stop_reason;
                    }
                }
            });
        for (auto& ev : stream) { (void)ev; }

        CHECK_EQ(captured_reason, StopReason::tool_use);
    });
}

// ─── Tool result fed back to LLM on second call ───────────────────────────

void test_tool_result_in_context_on_second_llm_call() {
    tests::register_test("Tool loop: tool result is in context on second LLM call", []() {
        Model model;
        model.id = "test-model";
        model.api = "test";
        model.provider = "test";

        int call_count = 0;
        std::string captured_tool_result_content;
        bool captured_has_tool_result = false;
        std::vector<AgentContext> captured_contexts;

        auto llm_client = std::make_shared<TestLLMClient>(
            [&](const AgentContext& context,
                const StreamOptions&,
                AssistantEventCallback,
                std::stop_token) -> std::shared_ptr<AssistantMessage> {
                ++call_count;
                auto msg = std::make_shared<AssistantMessage>();
                msg->api = "test";
                msg->provider = "test";
                msg->model = "test-model";
                msg->timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();

                if (call_count == 1) {
                    msg->stop_reason = StopReason::tool_use;
                    ToolCall tc;
                    tc.id = "call_001";
                    tc.name = "counter";
                    tc.arguments["start"] = 5;
                    msg->content.push_back(std::move(tc));
                } else {
                    // On second call, inspect context for the tool result
                    for (const auto& m : context.messages) {
                        if (const auto* trm = std::get_if<ToolResultMessage>(&m)) {
                            captured_has_tool_result = true;
                            for (const auto& cb : trm->content) {
                                if (const auto* tc = std::get_if<TextContent>(&cb)) {
                                    captured_tool_result_content = tc->text;
                                }
                            }
                        }
                    }
                    msg->stop_reason = StopReason::stop;
                    TextContent text;
                    text.text = "done";
                    msg->content.push_back(std::move(text));
                }
                return msg;
            });

        AgentContext ctx;
        ctx.system_prompt = "captured system";
        ctx.tools.push_back(std::make_shared<CounterTool>());

        AgentLoopConfig config;
        config.model = model;
        config.llm_client = llm_client;
        config.convert_to_llm = [](const std::vector<Message>& msgs) { return msgs; };
        config.on_effective_context =
            [&captured_contexts](const AgentContext& context) {
                captured_contexts.push_back(context);
            };
        config.should_stop_after_turn = nullptr;
        config.get_steering_messages = [] { return std::vector<Message>{}; };
        config.get_follow_up_messages = [] { return std::vector<Message>{}; };

        UserMessage user;
        user.content.emplace_back(TextContent{.text = "inspect the workspace"});

        auto stream = run_agent_loop({user}, ctx, config, [](const AgentEvent&) {});
        for (auto& ev : stream) { (void)ev; }

        CHECK_EQ(call_count, 2);
        CHECK_EQ(captured_contexts.size(), std::size_t(2));
        CHECK_EQ(captured_contexts[0].system_prompt,
                 std::string("captured system"));
        CHECK_EQ(captured_contexts[1].system_prompt,
                 std::string("captured system"));
        CHECK_EQ(captured_contexts[0].messages.size(), std::size_t(1));
        CHECK_EQ(captured_contexts[1].messages.size(), std::size_t(3));
        CHECK_EQ(captured_contexts[1].tools.size(), std::size_t(1));
        CHECK(std::holds_alternative<UserMessage>(captured_contexts[1].messages[0]));
        CHECK(std::holds_alternative<AssistantMessage>(captured_contexts[1].messages[1]));
        CHECK(std::holds_alternative<ToolResultMessage>(captured_contexts[1].messages[2]));
        CHECK(captured_has_tool_result);
        CHECK_EQ(captured_tool_result_content, std::string("Counter: 6"));
    });
}

// ─── Full tool round trip: correct final message sequence ─────────────────

void test_tool_full_round_trip_message_sequence() {
    tests::register_test("Tool loop: full round trip produces correct message sequence", []() {
        Model model;
        model.id = "test-model";
        model.api = "test";
        model.provider = "test";

        int call_count = 0;

        auto llm_client = std::make_shared<TestLLMClient>(
            [&](const AgentContext&,
                const StreamOptions&,
                AssistantEventCallback,
                std::stop_token) -> std::shared_ptr<AssistantMessage> {
                ++call_count;
                auto msg = std::make_shared<AssistantMessage>();
                msg->api = "test";
                msg->provider = "test";
                msg->model = "test-model";
                msg->timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();

                if (call_count == 1) {
                    msg->stop_reason = StopReason::tool_use;
                    ToolCall tc;
                    tc.id = "call_abc";
                    tc.name = "counter";
                    tc.arguments["start"] = 0;
                    msg->content.push_back(std::move(tc));
                } else {
                    msg->stop_reason = StopReason::stop;
                    TextContent text;
                    text.text = "final answer";
                    msg->content.push_back(std::move(text));
                }
                return msg;
            });

        UserMessage user_msg;
        TextContent uc;
        uc.text = "use the counter";
        user_msg.content.push_back(std::move(uc));

        AgentContext ctx;
        ctx.tools.push_back(std::make_shared<CounterTool>());

        AgentLoopConfig config;
        config.model = model;
        config.llm_client = llm_client;
        config.convert_to_llm = [](const std::vector<Message>& msgs) { return msgs; };
        config.should_stop_after_turn = nullptr;
        config.get_steering_messages = [] { return std::vector<Message>{}; };
        config.get_follow_up_messages = [] { return std::vector<Message>{}; };

        std::vector<Message> final_messages;
        auto stream = run_agent_loop({user_msg}, ctx, config,
            [&final_messages](const AgentEvent& ev) {
                if (const auto* e = std::get_if<AgentEndEvent>(&ev)) {
                    final_messages = e->messages;
                }
            });
        for (auto& ev : stream) { (void)ev; }

        // Expected sequence: user, assistant(tool_use), tool_result, assistant(stop)
        CHECK_EQ(final_messages.size(), std::size_t(4));

        CHECK(std::holds_alternative<UserMessage>(final_messages[0]));

        const auto* first_asst = std::get_if<AssistantMessage>(&final_messages[1]);
        CHECK(first_asst != nullptr);
        CHECK_EQ(first_asst->stop_reason, StopReason::tool_use);
        CHECK(!first_asst->content.empty());
        CHECK(std::holds_alternative<ToolCall>(first_asst->content[0]));

        const auto* tool_result = std::get_if<ToolResultMessage>(&final_messages[2]);
        CHECK(tool_result != nullptr);
        CHECK(!tool_result->is_error);
        CHECK_EQ(tool_result->tool_call_id, std::string("call_abc"));
        CHECK(!tool_result->content.empty());
        const auto* result_text = std::get_if<TextContent>(&tool_result->content[0]);
        CHECK(result_text != nullptr);
        CHECK_EQ(result_text->text, std::string("Counter: 1"));

        const auto* second_asst = std::get_if<AssistantMessage>(&final_messages[3]);
        CHECK(second_asst != nullptr);
        CHECK_EQ(second_asst->stop_reason, StopReason::stop);
    });
}

// ─── Main ──────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== pi-cpp agent loop tests ===\n\n";

    test_agent_loop_single_turn();
    test_effective_context_callback();
    test_request_local_runtime_identity();
    test_agent_loop_with_tools();
    test_agent_loop_stop_after_turn();
    test_agent_loop_sequential_tools();
    test_agent_loop_no_llm_client();
    test_agent_loop_provider_exception();
    test_agent_loop_tool_exception();
    test_agent_loop_continue();
    test_agent_loop_before_tool_call_blocks_with_reason();
    test_agent_loop_after_tool_call_partial_override();
    test_agent_loop_steering_after_turn_continues();
    test_agent_loop_message_envelopes();
    test_agent_loop_message_envelopes_cancelled();
    test_mailbox_envelope_waits_for_turn_boundary();
    test_agent_loop_steering_envelopes();
    test_agent_loop_argument_validation_blocks_execution();
    test_agent_loop_prepare_arguments_before_validation();
    test_parallel_completion_vs_source_order();
    test_parallel_mixed_immediate_source_order();
    test_per_tool_sequential_override();
    test_stream_options_propagation();
    test_api_key_resolution();
    test_reasoning_forwarded();
    test_streaming_text_delta();
    test_streaming_tool_call();
    test_streaming_error_path();
    test_streaming_done_stop_reason();
    test_tool_result_in_context_on_second_llm_call();
    test_tool_full_round_trip_message_sequence();

    tests::print_summary();

    return tests::failed > 0 ? 1 : 0;
}
