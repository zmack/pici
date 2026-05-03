#pragma once

#include <concepts>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <source_location>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "core/agent_state.h"
#include "core/event_types.h"
#include "core/llm_client.h"
#include "core/message_types.h"
#include "core/stream.h"

namespace pi::core {

// Forward declaration
class LLMClient;

// ─── Streaming callback type ────────────────────────────────────────────────

// Called for each streaming event from the LLM.
// Returns true to continue, false to abort.
using StreamCallback = std::function<void(const AgentEvent &)>;

struct BeforeToolCallContext {
  const Message &assistant_message;
  const ToolCall &tool_call;
  std::string args_json;
  AgentContext &context;
};

struct BeforeToolCallResult {
  bool block{false};
  std::string reason;
};

struct AfterToolCallContext {
  const Message &assistant_message;
  const ToolCall &tool_call;
  std::string args_json;
  std::shared_ptr<ToolResult> result;
  bool is_error{false};
  AgentContext &context;
};

struct AfterToolCallResult {
  std::optional<std::vector<ToolResultContentBlock>> content;
  std::optional<std::string> details;
  std::optional<bool> is_error;
  std::optional<bool> terminate;
};

// ─── Config for the agent loop ─────────────────────────────────────────────

struct AgentLoopConfig {
  Model model;
  ThinkingLevel thinking_level{ThinkingLevel::off};
  ToolExecutionMode tool_execution{ToolExecutionMode::parallel};

  std::optional<double> temperature;
  std::optional<std::uint32_t> max_tokens;
  std::optional<std::string> cache_retention;
  std::optional<std::string> session_id;
  Transport transport{Transport::auto_transport};
  std::map<std::string, std::string> headers;
  std::optional<std::uint32_t> timeout_ms;
  std::optional<std::uint32_t> max_retries;
  std::optional<std::uint32_t> max_retry_delay_ms;
  nlohmann::json metadata;
  decltype(StreamOptions::on_payload) on_payload;
  decltype(StreamOptions::on_response) on_response;

  // Converts AgentMessage[] to LLM-compatible messages
  std::function<std::vector<Message>(const std::vector<Message> &)>
      convert_to_llm;

  // Optional context transform (e.g., pruning)
  std::function<std::vector<Message>(const std::vector<Message> &,
                                     std::stop_token)>
      transform_context;

  // Resolves API key dynamically
  std::function<std::optional<std::string>(std::string_view provider)>
      get_api_key;

  // Called after each turn — return true to stop the loop
  std::function<bool(const Message &, const std::vector<ToolResultMessage> &,
                     AgentContext &)>
      should_stop_after_turn;

  // Returns steering messages (injected mid-run)
  std::function<std::vector<Message>()> get_steering_messages;

  // Returns follow-up messages (injected after agent would stop)
  std::function<std::vector<Message>()> get_follow_up_messages;

  // Called before tool execution
  std::function<std::optional<BeforeToolCallResult>(
      const BeforeToolCallContext &, std::stop_token)>
      before_tool_call;

  // Called after tool execution to possibly override result
  std::function<std::optional<AfterToolCallResult>(const AfterToolCallContext &,
                                                   std::stop_token)>
      after_tool_call;

  // The LLM client to use
  std::shared_ptr<LLMClient> llm_client;
};

// ─── Agent Loop Entry Points ───────────────────────────────────────────────

// Start a new agent loop with prompts
EventStream<AgentEvent, std::vector<Message>>
run_agent_loop(const std::vector<Message> &prompts, AgentContext context,
               const AgentLoopConfig &config, StreamCallback emit,
               const std::stop_token &stop_tok = std::stop_token{});

// Continue from existing context (no new prompt)
EventStream<AgentEvent, std::vector<Message>>
run_agent_loop_continue(AgentContext &context, const AgentLoopConfig &config,
                        StreamCallback emit,
                        const std::stop_token &stop_tok = std::stop_token{});

// ─── Streaming assistant response ──────────────────────────────────────────

// Stream an assistant response from the LLM.
// This is called by the loop on each turn.
std::shared_ptr<AssistantMessage>
stream_assistant_response(AgentContext &context, const AgentLoopConfig &config,
                          StreamCallback emit,
                          const std::stop_token &stop_tok = std::stop_token{});

// ─── Tool execution ─────────────────────────────────────────────────────────

// Execute all tool calls from an assistant message
struct ToolCallResult {
  std::vector<ToolResultMessage> messages;
  bool terminate{false}; // Early termination hint
};

ToolCallResult
execute_tool_calls(AgentContext &context,
                   const AssistantMessage &assistant_message,
                   const AgentLoopConfig &config, const StreamCallback &emit,
                   const std::stop_token &stop_tok = std::stop_token{});

} // namespace pi::core
