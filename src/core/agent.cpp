#include "core/agent.h"

#include <chrono>
#include <concepts>
#include <exception>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "core/agent_loop.h"
#include "core/agent_state.h"
#include "core/event_types.h"
#include "core/message_types.h"
#include "core/stream.h"

namespace pi::core {

namespace {

// Default convert_to_llm: pass through user/assistant/toolResult messages
std::vector<Message> default_convert_to_llm(const std::vector<Message> &msgs) {
  std::vector<Message> result;
  for (const auto &msg : msgs) {
    // Pass through: user, assistant, and toolResult messages
    if (std::holds_alternative<UserMessage>(msg) ||
        std::holds_alternative<AssistantMessage>(msg) ||
        std::holds_alternative<ToolResultMessage>(msg)) {
      result.push_back(msg);
    }
  }
  return result;
}

// Helper: create a user message from text + optional images
Message make_user_message(std::string text, std::vector<ImageContent> images) {
  UserMessage msg;
  msg.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count();
  TextContent tc;
  tc.text = std::move(text);
  msg.content.emplace_back(std::move(tc));
  for (auto &img : images) {
    msg.content.emplace_back(std::move(img));
  }
  return msg;
}

} // namespace

Agent::Agent() : Agent(Options{}) {}

Agent::Agent(const Options &options)
    : state_(options.system_prompt, options.model, options.thinking_level),
      options_(options) {}

Agent::~Agent() {
  abort();
  join_workers();
}

void Agent::add_tool(std::shared_ptr<const ToolDefinition> tool) {
  state_.add_tool(std::move(tool));
}

void Agent::set_tools(
    std::vector<std::shared_ptr<const ToolDefinition>> tools) {
  state_.set_tools(std::move(tools));
}

// ── Prompt ───────────────────────────────────────────────────────────────

EventStream<AgentEvent, std::vector<Message>>
Agent::prompt(std::string text, std::vector<ImageContent> images) {
  return prompt(std::vector<Message>{
      make_user_message(std::move(text), std::move(images))});
}

EventStream<AgentEvent, std::vector<Message>>
Agent::prompt(std::vector<Message> messages) {
  if (state_.is_streaming()) {
    throw std::runtime_error(
        "Agent is already processing a prompt. "
        "Use steer() or follow_up() to queue messages, or wait for "
        "completion.");
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

  join_workers();

  auto ctx = create_context_snapshot();
  auto config = create_loop_config();
  begin_run();

  launch_worker([this, messages, ctx, config, stream]() mutable {
    run_with_lifecycle([this, messages = std::move(messages),
                        ctx = std::move(ctx), config = std::move(config),
                        stream](const std::stop_token &stop_tok) mutable {
      auto event_stream = run_agent_loop(
          messages, ctx, config,
          [this](const AgentEvent &event) {
            process_event(event);
            // Also push to stream for the caller
          },
          stop_tok);

      // Forward events to the stream
      for (auto &event : event_stream) {
        stream.push(std::move(event));
      }

      stream.wait();
    });
  });

  return stream;
}

// ── Continue ──────────────────────────────────────────────────────────────

EventStream<AgentEvent, std::vector<Message>> Agent::continue_() {
  if (state_.is_streaming()) {
    throw std::runtime_error(
        "Agent is already processing. Wait for completion before "
        "continuing.");
  }

  auto ctx = state_.messages();
  if (ctx.empty()) {
    throw std::runtime_error("No messages to continue from");
  }

  auto &last = ctx.back();
  if (std::holds_alternative<AssistantMessage>(last)) {
    // Check for queued steering messages
    {
      std::scoped_lock lock(steering_mutex_);
      if (!steering_queue_.empty()) {
        auto messages = std::move(steering_queue_);
        return prompt(std::move(messages));
      }
    }

    // Check follow-up queue
    {
      std::scoped_lock lock(followup_mutex_);
      if (!followup_queue_.empty()) {
        auto messages = std::move(followup_queue_);
        return prompt(std::move(messages));
      }
    }

    throw std::runtime_error("Cannot continue from assistant message");
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

  join_workers();

  auto context = create_context_snapshot();
  auto config = create_loop_config();
  begin_run();

  launch_worker([this, context = std::move(context), config = std::move(config),
                 stream]() mutable {
    run_with_lifecycle([this, context = std::move(context),
                        config = std::move(config),
                        stream](const std::stop_token &stop_tok) mutable {
      auto event_stream = run_agent_loop_continue(
          context, config,
          [this](const AgentEvent &event) { process_event(event); }, stop_tok);

      for (auto &event : event_stream) {
        stream.push(std::move(event));
      }

      stream.wait();
    });
  });

  return stream;
}

// ── Steering ──────────────────────────────────────────────────────────────

void Agent::steer(std::vector<Message> messages) {
  std::scoped_lock lock(steering_mutex_);
  steering_queue_.insert(steering_queue_.end(),
                         std::make_move_iterator(messages.begin()),
                         std::make_move_iterator(messages.end()));
}

void Agent::clear_steering_queue() {
  std::scoped_lock lock(steering_mutex_);
  steering_queue_.clear();
}

void Agent::follow_up(std::vector<Message> messages) {
  std::scoped_lock lock(followup_mutex_);
  followup_queue_.insert(followup_queue_.end(),
                         std::make_move_iterator(messages.begin()),
                         std::make_move_iterator(messages.end()));
}

void Agent::clear_follow_up_queue() {
  std::scoped_lock lock(followup_mutex_);
  followup_queue_.clear();
}

// ── Control ───────────────────────────────────────────────────────────────

void Agent::abort() {
  auto &src = state_.stop_source();
  src.request_stop();
}

void Agent::reset() {
  state_.reset();
  clear_steering_queue();
  clear_follow_up_queue();
}

void Agent::wait_for_idle() { state_.wait_until_idle(); }

// ── Internal helpers ──────────────────────────────────────────────────────

void Agent::join_workers() {
  std::vector<std::jthread> workers;
  {
    std::scoped_lock lock(worker_mutex_);
    workers.swap(workers_);
  }
}

void Agent::begin_run() {
  std::scoped_lock lock(worker_mutex_);
  if (state_.is_streaming()) {
    throw std::runtime_error(
        "Agent is already processing a prompt. "
        "Use steer() or follow_up() to queue messages, or wait for "
        "completion.");
  }
  state_.reset_stop_source();
  state_.clear_error_message();
  state_.set_streaming(true);
  state_.set_complete(false);
}

void Agent::launch_worker(std::function<void()> worker) {
  try {
    std::scoped_lock lock(worker_mutex_);
    workers_.emplace_back(std::move(worker));
  } catch (...) {
    state_.set_streaming(false);
    state_.set_complete(true);
    throw;
  }
}

void Agent::run_with_lifecycle(
    const std::function<void(std::stop_token)> &executor) {
  try {
    executor(state_.stop_token());
  } catch (const std::exception &e) {
    state_.set_error_message(e.what());
  } catch (...) {
    state_.set_error_message("Unknown error");
  }

  state_.set_streaming(false);
  state_.set_complete(true);
}

AgentContext Agent::create_context_snapshot() const {
  return context_snapshot();
}

AgentContext Agent::context_snapshot() const {
  AgentContext ctx;
  ctx.system_prompt = state_.system_prompt();
  ctx.messages = state_.messages();
  ctx.model = state_.model();
  ctx.tools = state_.tools();
  return ctx;
}

AgentLoopConfig Agent::create_loop_config() {
  AgentLoopConfig config;
  config.model = state_.model();
  config.thinking_level = state_.thinking_level();
  config.temperature = options_.temperature;
  config.max_tokens = options_.max_tokens;
  config.cache_retention = options_.cache_retention;
  config.session_id = options_.session_id;
  config.transport = options_.transport;
  config.headers = options_.headers;
  config.timeout_ms = options_.timeout_ms;
  config.max_retries = options_.max_retries;
  config.max_retry_delay_ms = options_.max_retry_delay_ms;
  config.metadata = options_.metadata;
  config.on_payload = options_.on_payload;
  config.on_response = options_.on_response;
  config.diagnostics = options_.diagnostics;
  config.verbose = options_.verbose;
  config.tool_execution = options_.tool_execution;
  config.convert_to_llm = options_.convert_to_llm ? options_.convert_to_llm
                                                  : default_convert_to_llm;
  config.on_effective_context = options_.on_effective_context;
  config.transform_context = options_.transform_context;
  config.get_api_key = options_.get_api_key;
  config.should_stop_after_turn = options_.should_stop_after_turn;
  config.get_steering_messages = [this]() {
    std::scoped_lock lock(steering_mutex_);
    return std::move(steering_queue_);
  };
  config.get_follow_up_messages = [this]() {
    std::scoped_lock lock(followup_mutex_);
    return std::move(followup_queue_);
  };
  config.before_tool_call = options_.before_tool_call;
  config.after_tool_call = options_.after_tool_call;

  // Default LLM client
  config.llm_client = LLMClient::create(config.model);

  return config;
}

void Agent::process_event(const AgentEvent &event) {
  std::visit(
      [&](const auto &ev) {
        using T = std::remove_cvref_t<decltype(ev)>;
        if constexpr (std::same_as<T, MessageEndEvent>) {
          state_.append_message(ev.message);
        } else if constexpr (std::same_as<T, ToolExecutionStartEvent>) {
          state_.add_pending_tool_call(ev.tool_call_id);
        } else if constexpr (std::same_as<T, ToolExecutionEndEvent>) {
          state_.remove_pending_tool_call(ev.tool_call_id);
        } else if constexpr (std::same_as<T, TurnEndEvent>) {
          if (auto *asm_ = std::get_if<AssistantMessage>(&ev.message)) {
            if (asm_->error_message) {
              state_.set_error_message(*asm_->error_message);
            }
          }
        }
      },
      event);
}

} // namespace pi::core
