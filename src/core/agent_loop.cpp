#include "core/agent_loop.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <exception>
#include <future>
#include <memory>
#include <optional>
#include <source_location>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "core/agent_state.h"
#include "core/event_types.h"
#include "core/llm_client.h"
#include "core/message_types.h"
#include "core/stream.h"

#ifdef PI_CPP_OTEL_ENABLED
#include <opentelemetry/context/context.h>
#include <opentelemetry/context/runtime_context.h>
#include <opentelemetry/trace/context.h>
#include <opentelemetry/trace/scope.h>
#include <opentelemetry/trace/span_startoptions.h>
#include <opentelemetry/trace/tracer.h>
#endif

namespace pi::core {

namespace {

// Helper: extract ToolCalls from a content vector
std::vector<ToolCall>
extract_tool_calls(const std::vector<ContentBlock> &content) {
  std::vector<ToolCall> tool_calls;
  for (const auto &cb : content) {
    if (const auto *tc = std::get_if<ToolCall>(&cb)) {
      tool_calls.push_back(*tc);
    }
  }
  return tool_calls;
}

// Helper: create a ToolResultMessage from a ToolResult
ToolResultMessage
make_tool_result_message(const ToolCall &tc,
                         const std::shared_ptr<ToolResult> &result) {
  ToolResultMessage msg;
  (void)pi::core::ToolResultMessage::role; // role is set by type system, not
                                           // assignment
  msg.tool_call_id = tc.id;
  msg.tool_name = tc.name;
  msg.is_error = result->is_error();
  msg.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count();

  for (auto &block : result->content_blocks()) {
    std::visit(
        [&msg](auto &&value) {
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
  StaticToolResult(std::string content, bool is_error, bool terminate = false,
                   std::optional<std::string> details = std::nullopt)
      : content_blocks_({TextContent{.text = std::move(content)}}),
        is_error_(is_error), terminate_(terminate),
        details_(std::move(details)) {}
  StaticToolResult(std::vector<ToolResultContentBlock> content, bool is_error,
                   bool terminate = false,
                   std::optional<std::string> details = std::nullopt)
      : content_blocks_(std::move(content)), is_error_(is_error),
        terminate_(terminate), details_(std::move(details)) {}

  bool is_error() const override { return is_error_; }
  std::string content() const override {
    std::string text;
    for (const auto &block : content_blocks_) {
      if (const auto *tc = std::get_if<TextContent>(&block)) {
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
void emit_tool_result(const ToolCall &tc, std::shared_ptr<ToolResult> result,
                      bool is_err, const StreamCallback &emit) {
  ToolResultMessage msg = make_tool_result_message(tc, result);

  emit(MessageStartEvent(msg, std::source_location::current()));
  emit(MessageEndEvent(msg, std::source_location::current()));
  emit(ToolExecutionEndEvent(tc.id, tc.name, std::move(result), is_err,
                             std::source_location::current()));
}

// Helper: serialize tool call arguments as compact JSON string
std::string tool_call_args_json(const ToolCall &tc) {
  if (tc.arguments.is_object()) {
    return tc.arguments.dump();
  }
  return "{}";
}

// Helper: check if a tool is marked sequential
bool is_sequential_tool(
    const std::vector<std::shared_ptr<const ToolDefinition>> &tools,
    std::string_view name) {
  for (const auto &t : tools) {
    if (t->name() == name) {
      return t->execution_mode() == ToolExecutionMode::sequential;
    }
  }
  return false;
}

std::shared_ptr<ToolResult> make_error_tool_result(std::string message) {
  return std::make_shared<StaticToolResult>(std::move(message), true);
}

std::shared_ptr<const ToolDefinition> find_tool(const AgentContext &context,
                                                const ToolCall &tc) {
  auto tool_it = std::ranges::find_if(
      context.tools, [&tc](const auto &t) { return t->name() == tc.name; });
  if (tool_it == context.tools.end()) {
    return nullptr;
  }
  return *tool_it;
}

std::variant<PreparedToolCall, FinalizedToolCall>
prepare_tool_call(AgentContext &context,
                  const AssistantMessage &assistant_message, const ToolCall &tc,
                  const AgentLoopConfig &config, std::stop_token stop_tok) {
  auto tool = find_tool(context, tc);
  if (!tool) {
    return FinalizedToolCall{
        .tool_call = tc,
        .result = make_error_tool_result("Tool " + tc.name + " not found"),
        .is_error = true};
  }

  ToolCall prepared_tc = tc;
  try {
    prepared_tc.arguments = tool->prepare_arguments(tc.arguments);
  } catch (const std::exception &e) {
    return FinalizedToolCall{.tool_call = tc,
                             .result = make_error_tool_result(e.what()),
                             .is_error = true};
  } catch (...) {
    return FinalizedToolCall{.tool_call = tc,
                             .result = make_error_tool_result(
                                 "Unknown tool argument preparation error"),
                             .is_error = true};
  }

  if (auto validation_error =
          tool->schema().validate_arguments(prepared_tc.arguments)) {
    return FinalizedToolCall{.tool_call = prepared_tc,
                             .result =
                                 make_error_tool_result(*validation_error),
                             .is_error = true};
  }

  auto args_json = tool_call_args_json(prepared_tc);
  if (config.before_tool_call) {
    auto before = config.before_tool_call(
        BeforeToolCallContext{
            .assistant_message = assistant_message,
            .tool_call = prepared_tc,
            .args_json = args_json,
            .context = context,
        },
        std::move(stop_tok));
    if (before && before->block) {
      auto reason = before->reason.empty()
                        ? std::string{"Tool execution was blocked"}
                        : before->reason;
      return FinalizedToolCall{.tool_call = prepared_tc,
                               .result =
                                   make_error_tool_result(std::move(reason)),
                               .is_error = true};
    }
  }

  return PreparedToolCall{.tool_call = std::move(prepared_tc),
                          .tool = std::move(tool),
                          .args_json = std::move(args_json)};
}

FinalizedToolCall
finalize_tool_call(AgentContext &context,
                   const AssistantMessage &assistant_message,
                   const ToolCall &tc, std::shared_ptr<ToolResult> tool_result,
                   bool is_error, const AgentLoopConfig &config,
                   std::string_view args_json, std::stop_token stop_tok) {
  if (!tool_result) {
    tool_result = make_error_tool_result("Tool returned no result");
    is_error = true;
  }

  if (config.after_tool_call) {
    try {
      auto after = config.after_tool_call(
          AfterToolCallContext{
              .assistant_message = assistant_message,
              .tool_call = tc,
              .args_json = std::string(args_json),
              .result = tool_result,
              .is_error = is_error,
              .context = context,
          },
          std::move(stop_tok));
      if (after) {
        auto content = after->content.value_or(tool_result->content_blocks());
        auto details = after->details.has_value() ? after->details
                                                  : tool_result->details();
        auto final_is_error = after->is_error.value_or(is_error);
        auto terminate = after->terminate.value_or(tool_result->terminate());
        tool_result = std::make_shared<StaticToolResult>(
            std::move(content), final_is_error, terminate, std::move(details));
        is_error = final_is_error;
      }
    } catch (const std::exception &e) {
      tool_result = make_error_tool_result(e.what());
      is_error = true;
    } catch (...) {
      tool_result = make_error_tool_result("Unknown after_tool_call error");
      is_error = true;
    }
  }

  return FinalizedToolCall{
      .tool_call = tc, .result = std::move(tool_result), .is_error = is_error};
}

bool should_terminate_tool_batch(
    const std::vector<FinalizedToolCall> &finalized_calls) {
  return !finalized_calls.empty() &&
         std::ranges::all_of(finalized_calls, [](const auto &finalized) {
           return finalized.result && finalized.result->terminate();
         });
}

ToolResultMessage emit_finalized_tool_call(const FinalizedToolCall &finalized,
                                           StreamCallback emit) {
  emit_tool_result(finalized.tool_call, finalized.result, finalized.is_error,
                   std::move(emit));
  return make_tool_result_message(finalized.tool_call, finalized.result);
}

AssistantMessage get_partial(const AssistantMessageEvent &ev) {
  return std::visit(
      [](const auto &e) -> AssistantMessage {
        using E = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<E, AssistantMessageDoneEvent>) {
          return e.message;
        } else if constexpr (std::is_same_v<E, AssistantMessageErrorEvent>) {
          return e.error;
        } else {
          return e.partial;
        }
      },
      ev);
}

#ifdef PI_CPP_OTEL_ENABLED

namespace otel = opentelemetry;
using OtelSpan   = otel::nostd::shared_ptr<otel::trace::Span>;
using OtelTracer = otel::nostd::shared_ptr<otel::trace::Tracer>;

// Create a span whose parent is parent_ctx.
static OtelSpan otel_child_span(const OtelTracer &tracer,
                                 const otel::context::Context &parent_ctx,
                                 otel::nostd::string_view name) {
  otel::trace::StartSpanOptions opts;
  opts.parent = parent_ctx;
  return tracer->StartSpan(name, opts);
}

// Build a Context that has span as its active span (parent_ctx is the base).
static otel::context::Context otel_ctx_with(otel::context::Context parent,
                                             const OtelSpan &span) {
  return otel::trace::SetSpan(parent, span);
}

// True if any content block is a thinking block.
static bool otel_has_thinking(const std::vector<ContentBlock> &content) {
  return std::ranges::any_of(content, [](const ContentBlock &b) {
    return std::holds_alternative<ThinkingContent>(b);
  });
}

#endif // PI_CPP_OTEL_ENABLED

} // namespace

// ─── stream_assistant_response ─────────────────────────────────────────────

std::shared_ptr<AssistantMessage>
stream_assistant_response(AgentContext &context, const AgentLoopConfig &config,
                          StreamCallback emit, const std::stop_token &stop_tok,
                          OtelCtx otel_ctx) {
  auto messages = context.messages;

#ifdef PI_CPP_OTEL_ENABLED
  OtelSpan xform_span;
#endif

  if (config.transform_context) {
#ifdef PI_CPP_OTEL_ENABLED
    xform_span = otel_child_span(config.tracer, otel_ctx, "llm.transform_context");
    xform_span->SetAttribute("pi.transform.input_message_count",
                              static_cast<int64_t>(messages.size()));
#endif
    messages = config.transform_context(messages, stop_tok);
#ifdef PI_CPP_OTEL_ENABLED
    xform_span->SetAttribute("pi.transform.output_message_count",
                              static_cast<int64_t>(messages.size()));
    xform_span->End();
#endif
  }

  auto llm_messages = config.convert_to_llm(messages);
  (void)llm_messages;

  auto client = config.llm_client;
  if (!client) {
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

  StreamOptions opts;
  opts.temperature = config.temperature;
  opts.max_tokens = config.max_tokens;
  opts.reasoning = config.thinking_level;
  opts.cache_retention = config.cache_retention;
  opts.session_id = config.session_id;
  opts.transport = config.transport;
  opts.headers = config.headers;
  opts.timeout_ms = config.timeout_ms;
  opts.max_retries = config.max_retries;
  opts.max_retry_delay_ms = config.max_retry_delay_ms;
  opts.metadata = config.metadata;
  opts.on_payload = config.on_payload;
  opts.on_response = config.on_response;
  opts.api_key = config.get_api_key ? config.get_api_key(config.model.provider)
                                    : std::nullopt;

#ifdef PI_CPP_OTEL_ENABLED
  // Capture HTTP response metadata via on_response hook (compose with user's).
  int         llm_http_status = 0;
  std::string llm_response_id;
  auto prev_on_response = std::move(opts.on_response);
  opts.on_response = [&, prev = std::move(prev_on_response)](
                         int status,
                         const std::map<std::string, std::string> &hdrs,
                         const Model &model) {
    llm_http_status = status;
    if (auto it = hdrs.find("x-request-id"); it != hdrs.end())
      llm_response_id = it->second;
    if (prev) prev(status, hdrs, model);
  };

  auto llm_span = otel_child_span(config.tracer, otel_ctx, "llm.request");
  llm_span->SetAttribute("gen_ai.system",         config.model.provider);
  llm_span->SetAttribute("gen_ai.request.model",  config.model.id);
  llm_span->SetAttribute("gen_ai.operation.name", "chat");

  auto   llm_start        = std::chrono::steady_clock::now();
  bool   llm_first_token  = false;
#endif

  std::shared_ptr<AssistantMessage> partial;
  bool added_partial = false;

  auto on_event = [&](const AssistantMessageEvent &ev) {
#ifdef PI_CPP_OTEL_ENABLED
    if (!llm_first_token &&
        std::holds_alternative<AssistantMessageStartEvent>(ev)) {
      llm_first_token = true;
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - llm_start)
                         .count();
      llm_span->AddEvent("pi.llm.first_token");
      llm_span->SetAttribute("pi.llm.time_to_first_token_ms", elapsed);
    }
#endif
    std::visit(
        [&](const auto &e) {
          using E = std::decay_t<decltype(e)>;
          if constexpr (std::is_same_v<E, AssistantMessageStartEvent>) {
            partial = std::make_shared<AssistantMessage>(e.partial);
            context.messages.emplace_back(*partial);
            added_partial = true;
            emit(MessageStartEvent(*partial));
          } else if constexpr (std::is_same_v<E, AssistantMessageDoneEvent> ||
                               std::is_same_v<E, AssistantMessageErrorEvent>) {
            // handled after stream() returns
          } else {
            if (partial) {
              partial = std::make_shared<AssistantMessage>(get_partial(ev));
              context.messages.back() = *partial;
              emit(MessageUpdateEvent(*partial, ev));
            }
          }
        },
        ev);
  };

  auto final_msg =
      client->stream(config.model, context, opts, on_event, stop_tok);

  if (added_partial) {
    context.messages.back() = *final_msg;
  } else {
    context.messages.emplace_back(*final_msg);
    emit(MessageStartEvent(*final_msg));
  }
  emit(MessageEndEvent(*final_msg));

#ifdef PI_CPP_OTEL_ENABLED
  if (llm_http_status)
    llm_span->SetAttribute("http.response.status_code",
                            static_cast<int64_t>(llm_http_status));
  if (!llm_response_id.empty())
    llm_span->SetAttribute("gen_ai.response.id", llm_response_id);
  llm_span->SetAttribute("gen_ai.response.model",       final_msg->model);
  llm_span->SetAttribute("gen_ai.usage.input_tokens",
                          static_cast<int64_t>(final_msg->usage.input));
  llm_span->SetAttribute("gen_ai.usage.output_tokens",
                          static_cast<int64_t>(final_msg->usage.output));
  llm_span->SetAttribute("anthropic.usage.cache_read_input_tokens",
                          static_cast<int64_t>(final_msg->usage.cache_read));
  llm_span->SetAttribute("anthropic.usage.cache_creation_input_tokens",
                          static_cast<int64_t>(final_msg->usage.cache_write));
  llm_span->SetAttribute("llm.has_thinking",
                          otel_has_thinking(final_msg->content));
  {
    std::string_view finish_reason =
        stop_reason_to_string(final_msg->stop_reason);
    llm_span->SetAttribute(
        "gen_ai.response.finish_reasons",
        opentelemetry::nostd::span<const opentelemetry::nostd::string_view>{
            &finish_reason, 1});
  }
  if (final_msg->stop_reason == StopReason::error)
    llm_span->SetStatus(opentelemetry::trace::StatusCode::kError,
                        final_msg->error_message.value_or(""));
  llm_span->End();
#endif

  return final_msg;
}

// ─── execute_tool_calls (sequential) ───────────────────────────────────────

static ToolCallResult execute_tool_calls_sequential(
    AgentContext &context, const AssistantMessage &assistant_message,
    const std::vector<ToolCall> &tool_calls, const AgentLoopConfig &config,
    StreamCallback emit, const std::stop_token &stop_tok,
    OtelCtx otel_ctx) {
  ToolCallResult result;
  std::vector<FinalizedToolCall> finalized_calls;

  for (const auto &tc : tool_calls) {
    emit(ToolExecutionStartEvent(tc.id, tc.name, tool_call_args_json(tc),
                                 std::source_location::current()));

#ifdef PI_CPP_OTEL_ENABLED
    auto tool_span = otel_child_span(config.tracer, otel_ctx, "tool.call");
    tool_span->SetAttribute("gen_ai.tool.name",    tc.name);
    tool_span->SetAttribute("gen_ai.tool.call.id", tc.id);
    tool_span->SetAttribute("gen_ai.tool.type",    "function");
    tool_span->AddEvent("tool.prepare.start");
#endif

    auto prepared =
        prepare_tool_call(context, assistant_message, tc, config, stop_tok);

#ifdef PI_CPP_OTEL_ENABLED
    tool_span->AddEvent("tool.prepare.end");
#endif

    if (auto *immediate = std::get_if<FinalizedToolCall>(&prepared)) {
#ifdef PI_CPP_OTEL_ENABLED
      tool_span->SetAttribute("tool.blocked",  immediate->is_error);
      tool_span->SetAttribute("tool.is_error", immediate->is_error);
      if (immediate->is_error)
        tool_span->SetStatus(opentelemetry::trace::StatusCode::kError, "");
      tool_span->End();
#endif
      result.messages.push_back(emit_finalized_tool_call(*immediate, emit));
      finalized_calls.push_back(std::move(*immediate));
      continue;
    }

    auto call = std::get<PreparedToolCall>(std::move(prepared));

    auto tool_result = call.tool->execute(
        call.tool_call.id, call.args_json, stop_tok,
        [&emit, &call](const std::shared_ptr<ToolResult> &partial) {
          emit(ToolExecutionUpdateEvent(
              call.tool_call.id, call.tool_call.name, call.args_json,
              partial ? partial->content() : std::string{},
              std::source_location::current()));
        });

#ifdef PI_CPP_OTEL_ENABLED
    tool_span->AddEvent("tool.finalize.start");
#endif

    auto finalized = finalize_tool_call(
        context, assistant_message, call.tool_call, std::move(tool_result),
        false, config, call.args_json, stop_tok);

#ifdef PI_CPP_OTEL_ENABLED
    tool_span->AddEvent("tool.finalize.end");
    tool_span->SetAttribute("tool.is_error", finalized.is_error);
    if (finalized.result)
      tool_span->SetAttribute("pi.tool.result_bytes",
          static_cast<int64_t>(finalized.result->content().size()));
    if (finalized.is_error)
      tool_span->SetStatus(opentelemetry::trace::StatusCode::kError, "");
    tool_span->End();
#endif

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
    AgentContext &context, const AssistantMessage &assistant_message,
    const std::vector<ToolCall> &tool_calls, const AgentLoopConfig &config,
    const StreamCallback &emit, const std::stop_token &stop_tok,
    OtelCtx otel_ctx) {
  ToolCallResult result;
  const std::size_t n = tool_calls.size();
  std::vector<std::optional<FinalizedToolCall>> slots(n);
  std::vector<std::future<std::pair<std::size_t, FinalizedToolCall>>> pending;

  for (std::size_t i = 0; i < n; ++i) {
    const auto &tc = tool_calls[i];
    emit(ToolExecutionStartEvent(tc.id, tc.name, tool_call_args_json(tc),
                                 std::source_location::current()));

#ifdef PI_CPP_OTEL_ENABLED
    // Create the tool.call span on the worker thread; it will be ended on the
    // async thread. tool_span (shared_ptr) is captured by value so it stays
    // alive. Span::End() is thread-safe per the OTel spec and SDK.
    auto tool_span = otel_child_span(config.tracer, otel_ctx, "tool.call");
    tool_span->SetAttribute("gen_ai.tool.name",    tc.name);
    tool_span->SetAttribute("gen_ai.tool.call.id", tc.id);
    tool_span->SetAttribute("gen_ai.tool.type",    "function");
    tool_span->AddEvent("tool.prepare.start");
    auto tool_ctx = otel_ctx_with(otel_ctx, tool_span);
#endif

    auto prepared =
        prepare_tool_call(context, assistant_message, tc, config, stop_tok);

#ifdef PI_CPP_OTEL_ENABLED
    tool_span->AddEvent("tool.prepare.end");
#endif

    if (auto *immediate = std::get_if<FinalizedToolCall>(&prepared)) {
#ifdef PI_CPP_OTEL_ENABLED
      tool_span->SetAttribute("tool.blocked",  immediate->is_error);
      tool_span->SetAttribute("tool.is_error", immediate->is_error);
      if (immediate->is_error)
        tool_span->SetStatus(opentelemetry::trace::StatusCode::kError, "");
      tool_span->End();
#endif
      emit(ToolExecutionEndEvent(
          immediate->tool_call.id, immediate->tool_call.name, immediate->result,
          immediate->is_error, std::source_location::current()));
      slots[i] = std::move(*immediate);
      continue;
    }

    auto call = std::get<PreparedToolCall>(std::move(prepared));
    pending.push_back(std::async(
        std::launch::async,
        [&context, &assistant_message, &config, emit, stop_tok,
         call = std::move(call), idx = i
#ifdef PI_CPP_OTEL_ENABLED
         , tool_span, tool_ctx
#endif
        ]() mutable {
#ifdef PI_CPP_OTEL_ENABLED
          // Attach tool_ctx to this thread so inner instrumentation (e.g.
          // inside tool->execute) can find the parent via GetCurrent().
          otel::trace::Scope tool_scope(tool_span);
          (void)tool_ctx;
#endif
          auto tool_result = call.tool->execute(
              call.tool_call.id, call.args_json, stop_tok,
              [emit, call](const std::shared_ptr<ToolResult> &partial) {
                emit(ToolExecutionUpdateEvent(
                    call.tool_call.id, call.tool_call.name, call.args_json,
                    partial ? partial->content() : std::string{},
                    std::source_location::current()));
              });

#ifdef PI_CPP_OTEL_ENABLED
          tool_span->AddEvent("tool.finalize.start");
#endif
          auto finalized = finalize_tool_call(
              context, assistant_message, call.tool_call,
              std::move(tool_result), false, config, call.args_json, stop_tok);
#ifdef PI_CPP_OTEL_ENABLED
          tool_span->AddEvent("tool.finalize.end");
          tool_span->SetAttribute("tool.is_error", finalized.is_error);
          if (finalized.result)
            tool_span->SetAttribute("pi.tool.result_bytes",
                static_cast<int64_t>(finalized.result->content().size()));
          if (finalized.is_error)
            tool_span->SetStatus(opentelemetry::trace::StatusCode::kError, "");
          tool_span->End();
#endif
          emit(ToolExecutionEndEvent(call.tool_call.id, call.tool_call.name,
                                     finalized.result, finalized.is_error,
                                     std::source_location::current()));
          return std::make_pair(idx, std::move(finalized));
        }));
  }

  for (auto &future : pending) {
    auto [idx, finalized] = future.get();
    slots[idx] = std::move(finalized);
  }

  std::vector<FinalizedToolCall> finalized_calls;
  finalized_calls.reserve(n);
  for (auto &slot : slots) {
    finalized_calls.push_back(std::move(*slot));
  }

  for (const auto &finalized : finalized_calls) {
    auto msg = make_tool_result_message(finalized.tool_call, finalized.result);
    emit(MessageStartEvent(msg, std::source_location::current()));
    emit(MessageEndEvent(msg, std::source_location::current()));
    result.messages.push_back(std::move(msg));
  }

  result.terminate = should_terminate_tool_batch(finalized_calls);
  return result;
}

// ─── execute_tool_calls (public entry) ─────────────────────────────────────

ToolCallResult execute_tool_calls(AgentContext &context,
                                  const AssistantMessage &assistant_message,
                                  const AgentLoopConfig &config,
                                  const StreamCallback &emit,
                                  const std::stop_token &stop_tok,
                                  OtelCtx otel_ctx) {
  auto tool_calls = extract_tool_calls(assistant_message.content);

  if (tool_calls.empty()) {
    return {};
  }

  auto has_sequential =
      std::ranges::any_of(tool_calls, [&context](const auto &tc) {
        return is_sequential_tool(context.tools, tc.name);
      });

  if (config.tool_execution == ToolExecutionMode::sequential ||
      has_sequential) {
    return execute_tool_calls_sequential(context, assistant_message, tool_calls,
                                         config, emit, stop_tok, otel_ctx);
  }
  return execute_tool_calls_parallel(context, assistant_message, tool_calls,
                                     config, emit, stop_tok, otel_ctx);
}

// ─── run_agent_loop (main loop) ────────────────────────────────────────────

EventStream<AgentEvent, std::vector<Message>>
run_agent_loop(const std::vector<Message> &prompts, AgentContext context,
               const AgentLoopConfig &config, StreamCallback emit,
               const std::stop_token &stop_tok) {
  EventStream<AgentEvent, std::vector<Message>> stream(
      // Done predicate: agent_end
      [](const AgentEvent &ev) {
        return std::holds_alternative<AgentEndEvent>(ev);
      },
      // Result extractor
      [](const AgentEvent &ev) -> std::vector<Message> {
        if (const auto *e = std::get_if<AgentEndEvent>(&ev)) {
          return e->messages;
        }
        return {};
      });

  std::thread([prompts, context = std::move(context), config,
               emit = std::move(emit), stream, stop_tok]() mutable {
    auto publish = [&](AgentEvent event) {
      emit(event);
      stream.push(std::move(event));
    };

#ifdef PI_CPP_OTEL_ENABLED
    auto session_span = otel_child_span(
        config.tracer, otel::context::RuntimeContext::GetCurrent(),
        "agent.session");
    session_span->SetAttribute("agent.model",    config.model.id);
    session_span->SetAttribute("agent.provider", config.model.provider);
    if (config.session_id)
      session_span->SetAttribute("session.id", *config.session_id);
    // Attach session context for this thread's lifetime.
    otel::trace::Scope session_scope(session_span);

    OtelSpan turn_span;
    otel::context::Context turn_ctx;

    auto begin_turn = [&] {
      turn_span = otel_child_span(
          config.tracer, otel::context::RuntimeContext::GetCurrent(),
          "agent.turn");
      turn_ctx = otel_ctx_with(otel::context::RuntimeContext::GetCurrent(),
                               turn_span);
    };

    auto end_turn = [&](const AssistantMessage &msg, std::size_t tool_count) {
      if (!turn_span) return;
      turn_span->SetAttribute("agent.tool_calls_count",
                               static_cast<int64_t>(tool_count));
      if (stop_tok.stop_requested()) {
        turn_span->AddEvent("cancelled");
        turn_span->SetStatus(otel::trace::StatusCode::kError, "cancelled");
      } else if (msg.stop_reason == StopReason::error) {
        turn_span->SetStatus(otel::trace::StatusCode::kError,
                             msg.error_message.value_or(""));
      }
      turn_span->End();
      turn_span = {};
    };
#endif

    publish(AgentStartEvent());
    publish(TurnStartEvent());
#ifdef PI_CPP_OTEL_ENABLED
    begin_turn();
#endif

    // Emit prompt messages
    for (const auto &prompt : prompts) {
      publish(MessageStartEvent(prompt));
      publish(MessageEndEvent(prompt));
      context.messages.push_back(prompt);
    }

    auto new_messages = prompts;
    auto pending_messages = config.get_steering_messages
                                ? config.get_steering_messages()
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
#ifdef PI_CPP_OTEL_ENABLED
          begin_turn();
#endif
        }
        has_more_tool_calls = false;

        if (!pending_messages.empty()) {
          for (const auto &msg : pending_messages) {
            publish(MessageStartEvent(msg));
            publish(MessageEndEvent(msg));
            context.messages.push_back(msg);
            new_messages.push_back(msg);
          }
          pending_messages.clear();
        }

        // Stream assistant response
        auto assistant_msg = stream_assistant_response(
            context, config, publish, stop_tok
#ifdef PI_CPP_OTEL_ENABLED
            , turn_ctx
#endif
        );
        if (!assistant_msg) {
          assistant_msg = std::make_shared<AssistantMessage>();
          assistant_msg->api = "none";
          assistant_msg->provider = "none";
          assistant_msg->model = config.model.id;
          assistant_msg->stop_reason = StopReason::error;
          assistant_msg->error_message = "LLM client returned no message";
        }
        new_messages.emplace_back(*assistant_msg);

        // Check for error/abort
        if (assistant_msg->stop_reason == StopReason::error ||
            assistant_msg->stop_reason == StopReason::aborted) {
#ifdef PI_CPP_OTEL_ENABLED
          end_turn(*assistant_msg, 0);
#endif
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
              context, *assistant_msg, config, publish, stop_tok
#ifdef PI_CPP_OTEL_ENABLED
              , turn_ctx
#endif
          );
          tool_results = std::move(batch_result.messages);
          has_more_tool_calls = !batch_result.terminate;

          for (const auto &tr : tool_results) {
            context.messages.emplace_back(tr);
            new_messages.emplace_back(tr);
          }
        }

#ifdef PI_CPP_OTEL_ENABLED
        end_turn(*assistant_msg, tool_calls.size());
#endif
        publish(TurnEndEvent(*assistant_msg, tool_results));

        // Should we stop after this turn?
        if (config.should_stop_after_turn) {
          bool stop = config.should_stop_after_turn(*assistant_msg,
                                                    tool_results, context);
          if (stop) {
            publish(AgentEndEvent(new_messages));
            stream.finish(new_messages);
            return;
          }
        }

        pending_messages = config.get_steering_messages
                               ? config.get_steering_messages()
                               : std::vector<Message>{};
      }

      // Check for follow-up messages
      auto follow_ups = config.get_follow_up_messages
                            ? config.get_follow_up_messages()
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
#ifdef PI_CPP_OTEL_ENABLED
    session_span->End();
#endif
  }).detach();

  return stream;
}

// ─── run_agent_loop_continue ───────────────────────────────────────────────

EventStream<AgentEvent, std::vector<Message>>
run_agent_loop_continue(AgentContext &context, const AgentLoopConfig &config,
                        StreamCallback emit, const std::stop_token &stop_tok) {
  if (context.messages.empty()) {
    // Can't continue with no messages
    EventStream<AgentEvent, std::vector<Message>> error_stream(
        [](const AgentEvent &) { return false; },
        [](const AgentEvent &) { return std::vector<Message>{}; });
    error_stream.finish_error("Cannot continue: no messages in context");
    return error_stream;
  }

  EventStream<AgentEvent, std::vector<Message>> stream(
      [](const AgentEvent &ev) {
        return std::holds_alternative<AgentEndEvent>(ev);
      },
      [](const AgentEvent &ev) -> std::vector<Message> {
        if (const auto *e = std::get_if<AgentEndEvent>(&ev)) {
          return e->messages;
        }
        return {};
      });

  auto context_snapshot = context;
  std::thread([context = std::move(context_snapshot), config,
               emit = std::move(emit), stream, stop_tok]() mutable {
    auto publish = [&](AgentEvent event) {
      emit(event);
      stream.push(std::move(event));
    };

#ifdef PI_CPP_OTEL_ENABLED
    auto session_span = otel_child_span(
        config.tracer, otel::context::RuntimeContext::GetCurrent(),
        "agent.session");
    session_span->SetAttribute("agent.model",    config.model.id);
    session_span->SetAttribute("agent.provider", config.model.provider);
    if (config.session_id)
      session_span->SetAttribute("session.id", *config.session_id);
    otel::trace::Scope session_scope(session_span);

    OtelSpan turn_span;
    otel::context::Context turn_ctx;

    auto begin_turn = [&] {
      turn_span = otel_child_span(
          config.tracer, otel::context::RuntimeContext::GetCurrent(),
          "agent.turn");
      turn_ctx = otel_ctx_with(otel::context::RuntimeContext::GetCurrent(),
                               turn_span);
    };

    auto end_turn = [&](const AssistantMessage &msg, std::size_t tool_count) {
      if (!turn_span) return;
      turn_span->SetAttribute("agent.tool_calls_count",
                               static_cast<int64_t>(tool_count));
      if (stop_tok.stop_requested()) {
        turn_span->AddEvent("cancelled");
        turn_span->SetStatus(otel::trace::StatusCode::kError, "cancelled");
      } else if (msg.stop_reason == StopReason::error) {
        turn_span->SetStatus(otel::trace::StatusCode::kError,
                             msg.error_message.value_or(""));
      }
      turn_span->End();
      turn_span = {};
    };
#endif

    publish(AgentStartEvent());
    publish(TurnStartEvent());
#ifdef PI_CPP_OTEL_ENABLED
    begin_turn();
#endif

    auto new_messages = std::vector<Message>{};
    auto pending_messages = config.get_steering_messages
                                ? config.get_steering_messages()
                                : std::vector<Message>{};
    bool first_turn = true;

    while (!stop_tok.stop_requested()) {
      bool has_more_tool_calls = true;

      while (has_more_tool_calls || !pending_messages.empty()) {
        if (first_turn) {
          first_turn = false;
        } else {
          publish(TurnStartEvent());
#ifdef PI_CPP_OTEL_ENABLED
          begin_turn();
#endif
        }

        if (!pending_messages.empty()) {
          for (const auto &msg : pending_messages) {
            publish(MessageStartEvent(msg));
            publish(MessageEndEvent(msg));
            context.messages.push_back(msg);
            new_messages.push_back(msg);
          }
          pending_messages.clear();
        }

        // Stream assistant response
        auto assistant_msg = stream_assistant_response(
            context, config, publish, stop_tok
#ifdef PI_CPP_OTEL_ENABLED
            , turn_ctx
#endif
        );
        if (!assistant_msg) {
          assistant_msg = std::make_shared<AssistantMessage>();
          assistant_msg->api = "none";
          assistant_msg->provider = "none";
          assistant_msg->model = config.model.id;
          assistant_msg->stop_reason = StopReason::error;
          assistant_msg->error_message = "LLM client returned no message";
        }
        new_messages.emplace_back(*assistant_msg);

        if (assistant_msg->stop_reason == StopReason::error ||
            assistant_msg->stop_reason == StopReason::aborted) {
#ifdef PI_CPP_OTEL_ENABLED
          end_turn(*assistant_msg, 0);
#endif
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
              context, *assistant_msg, config, publish, stop_tok
#ifdef PI_CPP_OTEL_ENABLED
              , turn_ctx
#endif
          );
          tool_results = std::move(batch_result.messages);
          has_more_tool_calls = !batch_result.terminate;

          for (const auto &tr : tool_results) {
            context.messages.emplace_back(tr);
            new_messages.emplace_back(tr);
          }
        }

#ifdef PI_CPP_OTEL_ENABLED
        end_turn(*assistant_msg, tool_calls.size());
#endif
        publish(TurnEndEvent(*assistant_msg, tool_results));

        if (config.should_stop_after_turn) {
          bool stop = config.should_stop_after_turn(*assistant_msg,
                                                    tool_results, context);
          if (stop) {
            publish(AgentEndEvent(new_messages));
            stream.finish(new_messages);
            return;
          }
        }

        pending_messages = config.get_steering_messages
                               ? config.get_steering_messages()
                               : std::vector<Message>{};
      }

      auto follow_ups = config.get_follow_up_messages
                            ? config.get_follow_up_messages()
                            : std::vector<Message>{};
      if (!follow_ups.empty()) {
        pending_messages = std::move(follow_ups);
        continue;
      }
      break;
    }

    publish(AgentEndEvent(new_messages));
    stream.finish(new_messages);
#ifdef PI_CPP_OTEL_ENABLED
    session_span->End();
#endif
  }).detach();

  return stream;
}

} // namespace pi::core
