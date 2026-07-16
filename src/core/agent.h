#pragma once

#include <concepts>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/agent_loop.h"
#include "core/agent_state.h"
#include "core/event_types.h"
#include "core/message_types.h"
#include "core/stream.h"

namespace pi::core {

class Agent {
public:
  struct Options {
    std::string system_prompt;
    Model model;
    ThinkingLevel thinking_level{ThinkingLevel::off};
    ToolExecutionMode tool_execution{ToolExecutionMode::parallel};
    Options() = default;

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
    std::shared_ptr<StreamDiagnostics> diagnostics;
    bool verbose{false};

    // LLM
    std::function<std::optional<std::string>(std::string_view provider)>
        get_api_key;

    // Context transform
    std::function<std::vector<Message>(const std::vector<Message> &,
                                       std::stop_token)>
        transform_context;

    std::function<std::optional<std::vector<Message>>(
        const AgentContext &, std::size_t estimated_tokens, std::stop_token)>
        prepare_context;

    // Message conversion
    std::function<std::vector<Message>(const std::vector<Message> &)>
        convert_to_llm;

    // Called with an immutable copy of the request-ready context immediately
    // before the LLM client is invoked.
    std::function<void(const AgentContext &)> on_effective_context;

    // Observes every typed event emitted by the agent loop. Observers must
    // treat the event as immutable and should not perform blocking work.
    std::function<void(const AgentEvent &)> on_event;

    // Tool callbacks
    std::function<std::optional<BeforeToolCallResult>(
        const BeforeToolCallContext &, std::stop_token)>
        before_tool_call;

    std::function<std::optional<AfterToolCallResult>(
        const AfterToolCallContext &, std::stop_token)>
        after_tool_call;

    // Stop condition
    std::function<bool(const Message &, const std::vector<ToolResultMessage> &,
                       const AgentContext &)>
        should_stop_after_turn;

    // Steering / follow-up
    std::function<std::vector<Message>()> get_steering_messages;
    std::function<std::vector<Message>()> get_follow_up_messages;
  };

  Agent();
  explicit Agent(const Options &options);
  ~Agent();

  Agent(const Agent &) = delete;
  Agent &operator=(const Agent &) = delete;
  Agent(Agent &&) = delete;
  Agent &operator=(Agent &&) = delete;

  // ── State access ───────────────────────────────────────────────────

  AgentState &state() { return state_; }
  const AgentState &state() const { return state_; }

  // Return an immutable snapshot of the current raw agent context.
  AgentContext context_snapshot() const;

  // ── Tools ──────────────────────────────────────────────────────────

  void add_tool(std::shared_ptr<const ToolDefinition> tool);
  void set_tools(std::vector<std::shared_ptr<const ToolDefinition>> tools);

  // ── Prompt / Continue ──────────────────────────────────────────────

  // Start a new prompt from text
  EventStream<AgentEvent, std::vector<Message>>
  prompt(std::string text, std::vector<ImageContent> images = {});

  // Start a new prompt from messages
  EventStream<AgentEvent, std::vector<Message>>
  prompt(std::vector<Message> messages);

  // Continue from current transcript
  EventStream<AgentEvent, std::vector<Message>> continue_();

  // ── Steering ───────────────────────────────────────────────────────

  // Queue a message to be injected after the current turn
  void steer(std::vector<Message> messages);
  void clear_steering_queue();

  // Queue a message to run only after the agent would otherwise stop
  void follow_up(std::vector<Message> messages);
  void clear_follow_up_queue();

  // ── Control ────────────────────────────────────────────────────────

  void abort();
  void reset();

  // Wait for the agent to become idle
  void wait_for_idle();

  // Check if agent is currently processing
  bool is_streaming() const { return state_.is_streaming(); }

private:
  AgentState state_;
  Options options_;

  // Queue management
  std::mutex steering_mutex_;
  std::vector<Message> steering_queue_;

  std::mutex followup_mutex_;
  std::vector<Message> followup_queue_;

  std::mutex worker_mutex_;
  std::vector<std::jthread> workers_;

  // Internal helpers
  void join_workers();
  void begin_run();
  void launch_worker(std::function<void()> worker);
  void run_with_lifecycle(const std::function<void(std::stop_token)> &executor);
  AgentContext create_context_snapshot() const;
  AgentLoopConfig create_loop_config();
  void process_event(const AgentEvent &event);
};

} // namespace pi::core
