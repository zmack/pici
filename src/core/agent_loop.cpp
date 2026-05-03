#include "core/agent_loop.h"

#include <algorithm>
#include <concepts>
#include <future>
#include <mutex>
#include <numeric>
#include <optional>
#include <shared_mutex>
#include <source_location>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/event_types.h"
#include "core/llm_client.h"
#include "core/message_types.h"
#include "core/stream.h"

namespace pi::core {

namespace {

// Helper: extract ToolCalls from a content vector
std::vector<ToolCall> extract_tool_calls(const std::vector<ContentBlock>& content) {
    std::vector<ToolCall> tool_calls;
    for (const auto& cb : content) {
        if (const auto* tc = std::get_if<ToolCall>(&cb)) {
            tool_calls.push_back(*tc);
        }
    }
    return tool_calls;
}

// Helper: create a ToolResultMessage from a ToolResult
ToolResultMessage make_tool_result_message(
    const ToolCall& tc,
    std::shared_ptr<ToolResult> result) {
    ToolResultMessage msg;
    (void)msg.role; // role is set by type system, not assignment
    msg.tool_call_id = tc.id;
    msg.tool_name = tc.name;
    msg.is_error = result->is_error();
    msg.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();

    for (auto& block : result->content_blocks()) {
        std::visit(
            [&msg](auto&& value) {
                msg.content.push_back(std::forward<decltype(value)>(value));
            },
            std::move(block));
    }

    auto details = result->details();
    if (details) {
        msg.details = std::move(*details);
    }

    return msg;
}

class StaticToolResult : public ToolResult {
public:
    StaticToolResult(std::string content,
                     bool is_error,
                     bool terminate = false,
                     std::optional<std::string> details = std::nullopt)
        : content_blocks_({TextContent{.text = std::move(content)}}),
          is_error_(is_error),
          terminate_(terminate), details_(std::move(details)) {}
    StaticToolResult(std::vector<ToolResultContentBlock> content,
                     bool is_error,
                     bool terminate = false,
                     std::optional<std::string> details = std::nullopt)
        : content_blocks_(std::move(content)), is_error_(is_error),
          terminate_(terminate), details_(std::move(details)) {}

    bool is_error() const override { return is_error_; }
    std::string content() const override {
        std::string text;
        for (const auto& block : content_blocks_) {
            if (const auto* tc = std::get_if<TextContent>(&block)) {
                text += tc->text;
            }
        }
        return text;
    }
    std::vector<ToolResultContentBlock> content_blocks() const override {
        return content_blocks_;
    }
    std::optional<std::string> details() const override { return details_; }
    bool terminate() const override { return terminate_; }

private:
    std::vector<ToolResultContentBlock> content_blocks_;
    bool is_error_{false};
    bool terminate_{false};
    std::optional<std::string> details_;
};

struct FinalizedToolCall {
    ToolCall tool_call;
    std::shared_ptr<ToolResult> result;
    bool is_error{false};
};

struct PreparedToolCall {
    ToolCall tool_call;
    std::shared_ptr<const ToolDefinition> tool;
    std::string args_json;
};

// Helper: emit a tool result message pair (start + end)
void emit_tool_result(
    const ToolCall& tc,
    std::shared_ptr<ToolResult> result,
    bool is_err,
    StreamCallback emit) {
    ToolResultMessage msg = make_tool_result_message(tc, result);

    emit(MessageStartEvent(msg,
                           std::source_location::current()));
    emit(MessageEndEvent(
        msg, std::source_location::current()));
    emit(ToolExecutionEndEvent(
        tc.id, tc.name, std::move(result), is_err,
        std::source_location::current()));
}

std::string json_escape(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char ch : value) {
        switch (ch) {
            case '"': escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default: escaped += ch; break;
        }
    }
    return escaped;
}

// Helper: serialize tool call arguments as JSON string
std::string tool_call_args_json(const ToolCall& tc) {
    std::string json_str = "{";
    for (const auto& [k, v] : tc.arguments) {
        json_str += "\"" + json_escape(k) + "\":\"" + json_escape(v) + "\",";
    }
    if (json_str.size() > 1 && json_str.back() == ',') {
        json_str.pop_back();
    }
    json_str += "}";
    return json_str;
}

// Helper: check if a tool is marked sequential
bool is_sequential_tool(const std::vector<std::shared_ptr<const ToolDefinition>>& tools,
                   std::string_view name) {
    for (const auto& t : tools) {
        if (t->name() == name) {
            return t->execution_mode() == ToolExecutionMode::sequential;
        }
    }
    return false;
}

std::shared_ptr<ToolResult> make_error_tool_result(std::string message) {
    return std::make_shared<StaticToolResult>(std::move(message), true);
}

std::shared_ptr<const ToolDefinition> find_tool(
    const AgentContext& context,
    const ToolCall& tc) {
    auto tool_it =
        std::find_if(context.tools.begin(), context.tools.end(),
                     [&tc](const auto& t) {
                         return t->name() == tc.name;
                     });
    if (tool_it == context.tools.end()) {
        return nullptr;
    }
    return *tool_it;
}

std::variant<PreparedToolCall, FinalizedToolCall> prepare_tool_call(
    AgentContext& context,
    const AssistantMessage& assistant_message,
    const ToolCall& tc,
    const AgentLoopConfig& config,
    std::stop_token stop_tok) {
    auto tool = find_tool(context, tc);
    if (!tool) {
        return FinalizedToolCall{
            tc, make_error_tool_result("Tool " + tc.name + " not found"), true};
    }

    ToolCall prepared_tc = tc;
    try {
        prepared_tc.arguments = tool->prepare_arguments(tc.arguments);
    } catch (const std::exception& e) {
        return FinalizedToolCall{tc, make_error_tool_result(e.what()), true};
    } catch (...) {
        return FinalizedToolCall{
            tc, make_error_tool_result("Unknown tool argument preparation error"),
            true};
    }

    if (auto validation_error =
            tool->schema().validate_arguments(prepared_tc.arguments)) {
        return FinalizedToolCall{
            prepared_tc, make_error_tool_result(*validation_error), true};
    }

    auto args_json = tool_call_args_json(prepared_tc);
    if (config.before_tool_call) {
        auto before = config.before_tool_call(
            BeforeToolCallContext{
                assistant_message,
                prepared_tc,
                args_json,
                context,
            },
            stop_tok);
        if (before && before->block) {
            auto reason = before->reason.empty()
                              ? std::string{"Tool execution was blocked"}
                              : before->reason;
            return FinalizedToolCall{
                prepared_tc, make_error_tool_result(std::move(reason)), true};
        }
    }

    return PreparedToolCall{std::move(prepared_tc), std::move(tool),
                            std::move(args_json)};
}

FinalizedToolCall finalize_tool_call(
    AgentContext& context,
    const AssistantMessage& assistant_message,
    const ToolCall& tc,
    std::shared_ptr<ToolResult> tool_result,
    bool is_error,
    const AgentLoopConfig& config,
    std::string_view args_json,
    std::stop_token stop_tok) {
    if (!tool_result) {
        tool_result = make_error_tool_result("Tool returned no result");
        is_error = true;
    }

    if (config.after_tool_call) {
        try {
            auto after = config.after_tool_call(
                AfterToolCallContext{
                    assistant_message,
                    tc,
                    std::string(args_json),
                    tool_result,
                    is_error,
                    context,
                },
                stop_tok);
            if (after) {
                auto content = after->content.value_or(
                    tool_result->content_blocks());
                auto details = after->details.has_value()
                                   ? after->details
                                   : tool_result->details();
                auto final_is_error = after->is_error.value_or(is_error);
                auto terminate =
                    after->terminate.value_or(tool_result->terminate());
                tool_result = std::make_shared<StaticToolResult>(
                    std::move(content), final_is_error, terminate,
                    std::move(details));
                is_error = final_is_error;
            }
        } catch (const std::exception& e) {
            tool_result = make_error_tool_result(e.what());
            is_error = true;
        } catch (...) {
            tool_result = make_error_tool_result("Unknown after_tool_call error");
            is_error = true;
        }
    }

    return FinalizedToolCall{tc, std::move(tool_result), is_error};
}

bool should_terminate_tool_batch(
    const std::vector<FinalizedToolCall>& finalized_calls) {
    return !finalized_calls.empty() &&
           std::ranges::all_of(finalized_calls, [](const auto& finalized) {
               return finalized.result && finalized.result->terminate();
           });
}

ToolResultMessage emit_finalized_tool_call(
    const FinalizedToolCall& finalized,
    StreamCallback emit) {
    emit_tool_result(finalized.tool_call, finalized.result, finalized.is_error, emit);
    return make_tool_result_message(finalized.tool_call, finalized.result);
}

} // namespace

// ─── stream_assistant_response ─────────────────────────────────────────────

std::shared_ptr<AssistantMessage> stream_assistant_response(
    AgentContext& context,
    const AgentLoopConfig& config,
    StreamCallback emit,
    std::stop_token stop_tok) {
    // 1. Apply context transform if configured
    auto messages = context.messages;
    if (config.transform_context) {
        messages = config.transform_context(messages, stop_tok);
    }

    // 2. Convert to LLM messages
    auto llm_messages = config.convert_to_llm(messages);
    (void)llm_messages;

    // 3. Call the LLM client
    auto client = config.llm_client;
    if (!client) {
        // No LLM client configured — return a stub message
        auto stub = std::make_shared<AssistantMessage>();
        stub->api = "none";
        stub->provider = "none";
        stub->model = config.model.id;
        stub->stop_reason = StopReason::error;
        stub->error_message = "No LLM client configured";
        stub->timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
        return stub;
    }

    auto result = client->stream(
        config.model,
        context,
        config.thinking_level,
        emit,
        []<class C>(const C& cfg) -> std::optional<std::string> {
            if (cfg.get_api_key) {
                return cfg.get_api_key(cfg.model.provider);
            }
            return std::nullopt;
        }(config),
        stop_tok);

    // 4. Update context with the result
    if (result) {
        // Replace the last message in context (the partial assistant message)
        // with the final result
        if (!context.messages.empty() &&
            std::holds_alternative<AssistantMessage>(context.messages.back())) {
            context.messages.back() = *result;
        } else {
            context.messages.push_back(*result);
        }
    }

    return result;
}

// ─── execute_tool_calls (sequential) ───────────────────────────────────────

static ToolCallResult execute_tool_calls_sequential(
    AgentContext& context,
    const AssistantMessage& assistant_message,
    const std::vector<ToolCall>& tool_calls,
    const AgentLoopConfig& config,
    StreamCallback emit,
    std::stop_token stop_tok) {
    ToolCallResult result;
    std::vector<FinalizedToolCall> finalized_calls;

    for (const auto& tc : tool_calls) {
        emit(ToolExecutionStartEvent(
            tc.id, tc.name, tool_call_args_json(tc),
            std::source_location::current()));

        auto prepared = prepare_tool_call(
            context, assistant_message, tc, config, stop_tok);
        if (auto* immediate = std::get_if<FinalizedToolCall>(&prepared)) {
            result.messages.push_back(emit_finalized_tool_call(*immediate, emit));
            finalized_calls.push_back(std::move(*immediate));
            continue;
        }

        auto call = std::get<PreparedToolCall>(std::move(prepared));

        auto tool_result = call.tool->execute(
            call.tool_call.id, call.args_json, stop_tok,
            [&emit, &call](std::shared_ptr<ToolResult> partial) {
                emit(ToolExecutionUpdateEvent(
                    call.tool_call.id, call.tool_call.name, call.args_json,
                    partial ? partial->content() : std::string{},
                    std::source_location::current()));
            });
        auto finalized = finalize_tool_call(
            context, assistant_message, call.tool_call, std::move(tool_result),
            false, config, call.args_json, stop_tok);

        result.messages.push_back(emit_finalized_tool_call(finalized, emit));
        finalized_calls.push_back(std::move(finalized));
    }

    result.terminate = should_terminate_tool_batch(finalized_calls);
    return result;
}

// ─── execute_tool_calls (parallel) ─────────────────────────────────────────
// Note: In a full implementation, tools would execute concurrently.
// For now, we execute them sequentially (simpler, no async complexity).

static ToolCallResult execute_tool_calls_parallel(
    AgentContext& context,
    const AssistantMessage& assistant_message,
    const std::vector<ToolCall>& tool_calls,
    const AgentLoopConfig& config,
    StreamCallback emit,
    std::stop_token stop_tok) {
    ToolCallResult result;
    const std::size_t n = tool_calls.size();
    std::vector<std::optional<FinalizedToolCall>> slots(n);
    std::vector<std::future<std::pair<std::size_t, FinalizedToolCall>>> pending;

    for (std::size_t i = 0; i < n; ++i) {
        const auto& tc = tool_calls[i];
        emit(ToolExecutionStartEvent(
            tc.id, tc.name, tool_call_args_json(tc),
            std::source_location::current()));

        auto prepared = prepare_tool_call(
            context, assistant_message, tc, config, stop_tok);
        if (auto* immediate = std::get_if<FinalizedToolCall>(&prepared)) {
            emit(ToolExecutionEndEvent(
                immediate->tool_call.id, immediate->tool_call.name,
                immediate->result, immediate->is_error,
                std::source_location::current()));
            slots[i] = std::move(*immediate);
            continue;
        }

        auto call = std::get<PreparedToolCall>(std::move(prepared));
        pending.push_back(std::async(
            std::launch::async,
            [&context, &assistant_message, &config, emit, stop_tok,
             call = std::move(call), idx = i]() mutable {
                auto tool_result = call.tool->execute(
                    call.tool_call.id, call.args_json, stop_tok,
                    [emit, call](std::shared_ptr<ToolResult> partial) {
                        emit(ToolExecutionUpdateEvent(
                            call.tool_call.id, call.tool_call.name,
                            call.args_json,
                            partial ? partial->content() : std::string{},
                            std::source_location::current()));
                    });
                auto finalized = finalize_tool_call(
                    context, assistant_message, call.tool_call,
                    std::move(tool_result), false, config, call.args_json,
                    stop_tok);
                emit(ToolExecutionEndEvent(
                    call.tool_call.id, call.tool_call.name, finalized.result,
                    finalized.is_error,
                    std::source_location::current()));
                return std::make_pair(idx, std::move(finalized));
            }));
    }

    for (auto& future : pending) {
        auto [idx, finalized] = future.get();
        slots[idx] = std::move(finalized);
    }

    std::vector<FinalizedToolCall> finalized_calls;
    finalized_calls.reserve(n);
    for (auto& slot : slots) {
        finalized_calls.push_back(std::move(*slot));
    }

    for (const auto& finalized : finalized_calls) {
        auto msg = make_tool_result_message(finalized.tool_call, finalized.result);
        emit(MessageStartEvent(msg, std::source_location::current()));
        emit(MessageEndEvent(msg, std::source_location::current()));
        result.messages.push_back(std::move(msg));
    }

    result.terminate = should_terminate_tool_batch(finalized_calls);
    return result;
}

// ─── execute_tool_calls (public entry) ─────────────────────────────────────

ToolCallResult execute_tool_calls(
    AgentContext& context,
    const AssistantMessage& assistant_message,
    const AgentLoopConfig& config,
    StreamCallback emit,
    std::stop_token stop_tok) {
    auto tool_calls = extract_tool_calls(assistant_message.content);

    if (tool_calls.empty()) {
        return {};
    }

    auto has_sequential = std::ranges::any_of(
        tool_calls,
        [&context](const auto& tc) {
            return is_sequential_tool(context.tools, tc.name);
        });

    if (config.tool_execution == ToolExecutionMode::sequential ||
        has_sequential) {
        return execute_tool_calls_sequential(
            context, assistant_message, tool_calls, config, emit, stop_tok);
    }
    return execute_tool_calls_parallel(
        context, assistant_message, tool_calls, config, emit, stop_tok);
}

// ─── run_agent_loop (main loop) ────────────────────────────────────────────

EventStream<AgentEvent, std::vector<Message>>
run_agent_loop(const std::vector<Message>& prompts,
               AgentContext context,
               const AgentLoopConfig& config,
               StreamCallback emit,
               std::stop_token stop_tok) {
    EventStream<AgentEvent, std::vector<Message>> stream(
        // Done predicate: agent_end
        [](const AgentEvent& ev) {
            return std::holds_alternative<AgentEndEvent>(ev);
        },
        // Result extractor
        [](const AgentEvent& ev) -> std::vector<Message> {
            if (auto* e = std::get_if<AgentEndEvent>(&ev)) {
                return e->messages;
            }
            return {};
        });

    std::ignore = std::async(std::launch::async,
                             [prompts, context = std::move(context), config,
                              emit = std::move(emit), stream, stop_tok]() mutable {
        auto publish = [&](AgentEvent event) {
            emit(event);
            stream.push(std::move(event));
        };

        publish(AgentStartEvent());
        publish(TurnStartEvent());

        // Emit prompt messages
        for (const auto& prompt : prompts) {
            publish(MessageStartEvent(prompt));
            publish(MessageEndEvent(prompt));
            context.messages.push_back(prompt);
        }

        auto new_messages = prompts;
        auto pending_messages =
            config.get_steering_messages ? config.get_steering_messages()
                                         : std::vector<Message>{};
        bool first_turn = true;

        // Outer loop: continues when follow-up messages arrive
        while (!stop_tok.stop_requested()) {
            bool has_more_tool_calls = true;

            // Inner loop: process tool calls and steering messages
            while (has_more_tool_calls || !pending_messages.empty()) {
                if (first_turn) {
                    first_turn = false;
                } else {
                    publish(TurnStartEvent());
                }
                has_more_tool_calls = false;

                if (!pending_messages.empty()) {
                    for (const auto& msg : pending_messages) {
                        publish(MessageStartEvent(msg));
                        publish(MessageEndEvent(msg));
                        context.messages.push_back(msg);
                        new_messages.push_back(msg);
                    }
                    pending_messages.clear();
                }

                // Stream assistant response
                auto assistant_msg = stream_assistant_response(
                    context, config, publish, stop_tok);
                if (!assistant_msg) {
                    assistant_msg = std::make_shared<AssistantMessage>();
                    assistant_msg->api = "none";
                    assistant_msg->provider = "none";
                    assistant_msg->model = config.model.id;
                    assistant_msg->stop_reason = StopReason::error;
                    assistant_msg->error_message = "LLM client returned no message";
                }
                new_messages.push_back(*assistant_msg);

                // Check for error/abort
                if (assistant_msg->stop_reason == StopReason::error ||
                    assistant_msg->stop_reason == StopReason::aborted) {
                    publish(TurnEndEvent(*assistant_msg, {}));
                    publish(AgentEndEvent(new_messages));
                    stream.finish(new_messages);
                    return;
                }

                // Check for tool calls
                auto tool_calls = extract_tool_calls(assistant_msg->content);

                std::vector<ToolResultMessage> tool_results;
                if (!tool_calls.empty()) {
                    auto batch_result = execute_tool_calls(
                        context, *assistant_msg, config, publish, stop_tok);
                    tool_results = std::move(batch_result.messages);
                    has_more_tool_calls = !batch_result.terminate;

                    for (const auto& tr : tool_results) {
                        context.messages.push_back(tr);
                        new_messages.push_back(tr);
                    }
                }

                publish(TurnEndEvent(*assistant_msg, tool_results));

                // Should we stop after this turn?
                if (config.should_stop_after_turn) {
                    bool stop = config.should_stop_after_turn(
                        *assistant_msg, tool_results, context);
                    if (stop) {
                        publish(AgentEndEvent(new_messages));
                        stream.finish(new_messages);
                        return;
                    }
                }

                pending_messages =
                    config.get_steering_messages ? config.get_steering_messages()
                                                 : std::vector<Message>{};
            }

            // Check for follow-up messages
            auto follow_ups =
                config.get_follow_up_messages ? config.get_follow_up_messages()
                                              : std::vector<Message>{};
            if (!follow_ups.empty()) {
                pending_messages = std::move(follow_ups);
                continue;
            }

            // No more messages, exit
            break;
        }

        publish(AgentEndEvent(new_messages));
        stream.finish(new_messages);
    });

    return stream;
}

// ─── run_agent_loop_continue ───────────────────────────────────────────────

EventStream<AgentEvent, std::vector<Message>>
run_agent_loop_continue(AgentContext& context,
                        const AgentLoopConfig& config,
                        StreamCallback emit,
                        std::stop_token stop_tok) {
    if (context.messages.empty()) {
        // Can't continue with no messages
        EventStream<AgentEvent, std::vector<Message>> error_stream(
            [](const AgentEvent&) { return false; },
            [](const AgentEvent&) { return std::vector<Message>{}; });
        error_stream.finish_error("Cannot continue: no messages in context");
        return error_stream;
    }

    EventStream<AgentEvent, std::vector<Message>> stream(
        [](const AgentEvent& ev) {
            return std::holds_alternative<AgentEndEvent>(ev);
        },
        [](const AgentEvent& ev) -> std::vector<Message> {
            if (auto* e = std::get_if<AgentEndEvent>(&ev)) {
                return e->messages;
            }
            return {};
        });

    auto context_snapshot = context;
    std::ignore = std::async(std::launch::async,
                             [context = std::move(context_snapshot), config,
                              emit = std::move(emit), stream, stop_tok]() mutable {
        auto publish = [&](AgentEvent event) {
            emit(event);
            stream.push(std::move(event));
        };

        publish(AgentStartEvent());
        publish(TurnStartEvent());

        auto new_messages = std::vector<Message>{};
        auto pending_messages =
            config.get_steering_messages ? config.get_steering_messages()
                                         : std::vector<Message>{};
        bool first_turn = true;

        while (!stop_tok.stop_requested()) {
            bool has_more_tool_calls = true;

            while (has_more_tool_calls || !pending_messages.empty()) {
                if (first_turn) {
                    first_turn = false;
                } else {
                    publish(TurnStartEvent());
                }

                if (!pending_messages.empty()) {
                    for (const auto& msg : pending_messages) {
                        publish(MessageStartEvent(msg));
                        publish(MessageEndEvent(msg));
                        context.messages.push_back(msg);
                        new_messages.push_back(msg);
                    }
                    pending_messages.clear();
                }

                // Stream assistant response
                auto assistant_msg = stream_assistant_response(
                    context, config, publish, stop_tok);
                if (!assistant_msg) {
                    assistant_msg = std::make_shared<AssistantMessage>();
                    assistant_msg->api = "none";
                    assistant_msg->provider = "none";
                    assistant_msg->model = config.model.id;
                    assistant_msg->stop_reason = StopReason::error;
                    assistant_msg->error_message = "LLM client returned no message";
                }
                new_messages.push_back(*assistant_msg);

                if (assistant_msg->stop_reason == StopReason::error ||
                    assistant_msg->stop_reason == StopReason::aborted) {
                    publish(TurnEndEvent(*assistant_msg, {}));
                    publish(AgentEndEvent(new_messages));
                    stream.finish(new_messages);
                    return;
                }

                // Check for tool calls
                auto tool_calls = extract_tool_calls(assistant_msg->content);

                std::vector<ToolResultMessage> tool_results;
                has_more_tool_calls = false;

                if (!tool_calls.empty()) {
                    auto batch_result = execute_tool_calls(
                        context, *assistant_msg, config, publish, stop_tok);
                    tool_results = std::move(batch_result.messages);
                    has_more_tool_calls = !batch_result.terminate;

                    for (const auto& tr : tool_results) {
                        context.messages.push_back(tr);
                        new_messages.push_back(tr);
                    }
                }

                publish(TurnEndEvent(*assistant_msg, tool_results));

                if (config.should_stop_after_turn) {
                    bool stop = config.should_stop_after_turn(
                        *assistant_msg, tool_results, context);
                    if (stop) {
                        publish(AgentEndEvent(new_messages));
                        stream.finish(new_messages);
                        return;
                    }
                }

                pending_messages =
                    config.get_steering_messages ? config.get_steering_messages()
                                                 : std::vector<Message>{};
            }

            auto follow_ups =
                config.get_follow_up_messages ? config.get_follow_up_messages()
                                              : std::vector<Message>{};
            if (!follow_ups.empty()) {
                pending_messages = std::move(follow_ups);
                continue;
            }
            break;
        }

        publish(AgentEndEvent(new_messages));
        stream.finish(new_messages);
    });

    return stream;
}

} // namespace pi::core
