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

    auto content_str = result->content();
    if (!content_str.empty()) {
        TextContent tc_text;
        tc_text.text = std::move(content_str);
        msg.content.push_back(std::move(tc_text));
    }

    auto details = result->details();
    if (details) {
        msg.details = std::move(*details);
    }

    return msg;
}

// Helper: emit a tool result message pair (start + end)
void emit_tool_result(
    const ToolCall& tc,
    std::shared_ptr<ToolResult> result,
    bool is_err,
    StreamCallback emit) {
    ToolResultMessage msg = make_tool_result_message(tc, result);

    // Emit start
    emit(MessageStartEvent(std::move(msg),
                           std::source_location::current()));

    // Emit end
    emit(MessageEndEvent(
        std::move(msg), std::source_location::current()));
}

// Helper: serialize tool call arguments as JSON-like string
std::string tool_call_args_json(const ToolCall& tc) {
    std::string json_str = "{";
    for (const auto& [k, v] : tc.arguments) {
        json_str += "\"" + k + "\":\"" + v + "\",";
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

    for (const auto& tc : tool_calls) {
        // Find tool
        auto tool_it =
            std::find_if(context.tools.begin(), context.tools.end(),
                         [&tc](const auto& t) {
                             return t->name() == tc.name;
                         });

        // Emit tool_execution_start
        emit(ToolExecutionStartEvent(
            tc.id, tc.name, tool_call_args_json(tc),
            std::source_location::current()));

        if (tool_it != context.tools.end()) {
            // Call before_tool_call
            bool tool_blocked = false;
            if (config.before_tool_call) {
                auto block = config.before_tool_call(
                    assistant_message, tc, tool_call_args_json(tc));
                if (block.has_value() && *block) {
                    tool_blocked = true;
                }
            }

            if (tool_blocked) {
                // Tool was blocked — emit error result
                // Create a minimal error result inline
                struct SimpleErrorResult : public ToolResult {
                    bool is_error() const override { return true; }
                    std::string content() const override { return "Tool execution blocked"; }
                    std::optional<std::string> details() const override { return std::nullopt; }
                };
                auto error_result = std::make_shared<SimpleErrorResult>();
                emit_tool_result(tc, error_result, true, emit);
                result.messages.push_back(
                    make_tool_result_message(tc, error_result));
                result.terminate = true;
                continue;
            }

            // Execute tool
            auto tool_result = (*tool_it)->execute(
                tc.id, tool_call_args_json(tc), stop_tok);

            // Call after_tool_call
            if (config.after_tool_call && tool_result) {
                auto override = config.after_tool_call(
                    assistant_message, tc, tool_result);
                if (override) {
                    auto [content, is_err, terminate] = *override;
                    // Note: In a full impl, we'd modify the result here
                }
            }

            emit_tool_result(tc, tool_result, tool_result->is_error(),
                             emit);
            result.messages.push_back(
                make_tool_result_message(tc, tool_result));

            if (tool_result && tool_result->terminate()) {
                result.terminate = true;
            }
        } else {
            // Tool not found — error
            struct SimpleErrorResult : public ToolResult {
                bool is_error() const override { return true; }
                std::string content() const override { return "Tool not found"; }
                std::optional<std::string> details() const override { return std::nullopt; }
            };
            auto error_result = std::make_shared<SimpleErrorResult>();
            emit_tool_result(tc, error_result, true, emit);
            result.messages.push_back(
                make_tool_result_message(tc, error_result));
            result.terminate = true;
        }

        if (result.terminate) break;
    }

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

    for (const auto& tc : tool_calls) {
        auto tool_it =
            std::find_if(context.tools.begin(), context.tools.end(),
                         [&tc](const auto& t) {
                             return t->name() == tc.name;
                         });

        // Emit tool_execution_start
        emit(ToolExecutionStartEvent(
            tc.id, tc.name, tool_call_args_json(tc),
            std::source_location::current()));

        if (tool_it != context.tools.end()) {
            if (config.before_tool_call) {
                auto block_result = config.before_tool_call(
                    assistant_message, tc, tool_call_args_json(tc));
                if (block_result.has_value() && *block_result) {
                    struct SimpleErrorResult : public ToolResult {
                        bool is_error() const override { return true; }
                        std::string content() const override {
                            return "Tool execution blocked";
                        }
                        std::optional<std::string> details() const override {
                            return std::nullopt;
                        }
                    };
                    auto error_result = std::make_shared<SimpleErrorResult>();
                    emit_tool_result(tc, error_result, true, emit);
                    result.messages.push_back(
                        make_tool_result_message(tc, error_result));
                    result.terminate = true;
                    continue;
                }
            }

            auto tool_result =
                (*tool_it)->execute(tc.id, tool_call_args_json(tc), stop_tok);

            if (config.after_tool_call && tool_result) {
                auto override = config.after_tool_call(
                    assistant_message, tc, tool_result);
                if (override) {
                    auto [content, is_err, terminate] = *override;
                }
            }

            emit_tool_result(tc, tool_result, tool_result->is_error(),
                             emit);
            result.messages.push_back(
                make_tool_result_message(tc, tool_result));

            if (tool_result && tool_result->terminate()) {
                result.terminate = true;
            }
        } else {
            struct SimpleErrorResult : public ToolResult {
                bool is_error() const override { return true; }
                std::string content() const override {
                    return "Tool not found";
                }
                std::optional<std::string> details() const override {
                    return std::nullopt;
                }
            };
            auto error_result = std::make_shared<SimpleErrorResult>();
            emit_tool_result(tc, error_result, true, emit);
            result.messages.push_back(
                make_tool_result_message(tc, error_result));
            result.terminate = true;
        }

        if (result.terminate) break;
    }

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

    // Run the loop asynchronously
    std::ignore = std::async(std::launch::async, [&]() mutable {
        emit(AgentStartEvent());
        emit(TurnStartEvent());

        // Emit prompt messages
        for (const auto& prompt : prompts) {
            emit(MessageStartEvent(prompt));
            emit(MessageEndEvent(prompt));
        }

        auto new_messages = prompts;

        // Outer loop: continues when follow-up messages arrive
        while (!stop_tok.stop_requested()) {
            bool has_more_tool_calls = true;

            // Inner loop: process tool calls and steering messages
            while (has_more_tool_calls || !context.messages.empty()) {
                // Check steering messages
                auto steering =
                    config.get_steering_messages ? config.get_steering_messages()
                                                 : std::vector<Message>{};
                if (!steering.empty()) {
                    for (const auto& msg : steering) {
                        emit(MessageStartEvent(msg));
                        emit(MessageEndEvent(msg));
                        context.messages.push_back(msg);
                        new_messages.push_back(msg);
                    }
                }

                // Stream assistant response
                auto assistant_msg = stream_assistant_response(
                    context, config, emit, stop_tok);
                new_messages.push_back(*assistant_msg);

                // Check for error/abort
                if (assistant_msg->stop_reason == StopReason::error ||
                    assistant_msg->stop_reason == StopReason::aborted) {
                    emit(TurnEndEvent(*assistant_msg, {}));
                    emit(AgentEndEvent(new_messages));
                    stream.finish(new_messages);
                    return;
                }

                // Check for tool calls
                auto tool_calls = extract_tool_calls(assistant_msg->content);

                std::vector<ToolResultMessage> tool_results;
                has_more_tool_calls = false;

                if (!tool_calls.empty()) {
                    auto batch_result = execute_tool_calls(
                        context, *assistant_msg, config, emit, stop_tok);
                    tool_results = std::move(batch_result.messages);
                    has_more_tool_calls = !batch_result.terminate;

                    for (const auto& tr : tool_results) {
                        context.messages.push_back(tr);
                        new_messages.push_back(tr);
                    }
                }

                emit(TurnEndEvent(*assistant_msg, tool_results));

                // Should we stop after this turn?
                if (config.should_stop_after_turn) {
                    bool stop = config.should_stop_after_turn(
                        *assistant_msg, tool_results, context);
                    if (stop) {
                        emit(AgentEndEvent(new_messages));
                        stream.finish(new_messages);
                        return;
                    }
                }
            }

            // Check for follow-up messages
            auto follow_ups =
                config.get_follow_up_messages ? config.get_follow_up_messages()
                                              : std::vector<Message>{};
            if (!follow_ups.empty()) {
                continue;
            }

            // No more messages, exit
            break;
        }

        emit(AgentEndEvent(new_messages));
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

    std::ignore = std::async(std::launch::async, [&]() mutable {
        emit(AgentStartEvent());
        emit(TurnStartEvent());

        auto new_messages = std::vector<Message>{};

        while (!stop_tok.stop_requested()) {
            bool has_more_tool_calls = true;

            while (has_more_tool_calls) {
                emit(TurnStartEvent());

                // Check steering messages
                auto steering =
                    config.get_steering_messages ? config.get_steering_messages()
                                                 : std::vector<Message>{};
                if (!steering.empty()) {
                    for (const auto& msg : steering) {
                        emit(MessageStartEvent(msg));
                        emit(MessageEndEvent(msg));
                        context.messages.push_back(msg);
                        new_messages.push_back(msg);
                    }
                }

                // Stream assistant response
                auto assistant_msg = stream_assistant_response(
                    context, config, emit, stop_tok);
                new_messages.push_back(*assistant_msg);

                if (assistant_msg->stop_reason == StopReason::error ||
                    assistant_msg->stop_reason == StopReason::aborted) {
                    emit(TurnEndEvent(*assistant_msg, {}));
                    emit(AgentEndEvent(new_messages));
                    stream.finish(new_messages);
                    return;
                }

                // Check for tool calls
                auto tool_calls = extract_tool_calls(assistant_msg->content);

                std::vector<ToolResultMessage> tool_results;
                has_more_tool_calls = false;

                if (!tool_calls.empty()) {
                    auto batch_result = execute_tool_calls(
                        context, *assistant_msg, config, emit, stop_tok);
                    tool_results = std::move(batch_result.messages);
                    has_more_tool_calls = !batch_result.terminate;

                    for (const auto& tr : tool_results) {
                        context.messages.push_back(tr);
                        new_messages.push_back(tr);
                    }
                }

                emit(TurnEndEvent(*assistant_msg, tool_results));

                if (config.should_stop_after_turn) {
                    bool stop = config.should_stop_after_turn(
                        *assistant_msg, tool_results, context);
                    if (stop) {
                        emit(AgentEndEvent(new_messages));
                        stream.finish(new_messages);
                        return;
                    }
                }
            }

            auto follow_ups =
                config.get_follow_up_messages ? config.get_follow_up_messages()
                                              : std::vector<Message>{};
            if (!follow_ups.empty()) {
                continue;
            }
            break;
        }

        emit(AgentEndEvent(new_messages));
        stream.finish(new_messages);
    });

    return stream;
}

} // namespace pi::core
