#include "core/agent.h"

#include <chrono>
#include <concepts>
#include <cstddef>
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
#include "core/models.h"
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

std::size_t estimate_context_tokens(const std::vector<Message> &messages) {
  std::size_t bytes = 0;
  for (const auto &message : messages)
    bytes += json::to_json(message).size();
  // This is deliberately a byte-based warning, not an exact tokenizer.
  return (bytes + 3) / 4;
}

std::vector<AgentMessageEnvelope>
make_message_envelopes(std::vector<Message> messages) {
  std::vector<AgentMessageEnvelope> envelopes;
  envelopes.reserve(messages.size());
  for (auto &message : messages)
    envelopes.push_back({.message = std::move(message)});
  return envelopes;
}

} // namespace

Agent::Agent() : Agent(Options{}) {}

Agent::Agent(const Options &options)
    : state_(
          options.system_prompt, options.model,
          resolve_thinking_level(options.model, options.thinking_level).level),
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

EventStream<AgentEvent, std::vector<Message>>
Agent::prompt(std::string text, std::vector<ImageContent> images) {
  return prompt(std::vector<Message>{
      make_user_message(std::move(text), std::move(images))});
}

EventStream<AgentEvent, std::vector<Message>>
Agent::prompt(std::vector<Message> messages) {
  return prompt(make_message_envelopes(std::move(messages)));
}

EventStream<AgentEvent, std::vector<Message>>
Agent::prompt(std::vector<AgentMessageEnvelope> messages) {
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
  std::unique_lock run_lock(worker_mutex_);
  if (state_.is_streaming()) {
    throw std::runtime_error(
        "Agent is already processing a prompt. "
        "Use steer() or follow_up() to queue messages, or wait for "
        "completion.");
  }
  auto ctx = create_context_snapshot();
  auto config = create_loop_config();
  begin_run_locked();

  launch_worker_locked(
      [this, messages = std::move(messages), ctx, config, stream]() mutable {
        run_with_lifecycle([this, messages = std::move(messages),
                            ctx = std::move(ctx), config = std::move(config),
                            stream](const std::stop_token &stop_tok) mutable {
          auto event_stream = run_agent_loop_envelopes(
              std::move(messages), std::move(ctx), config,
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
  std::unique_lock run_lock(worker_mutex_);
  if (state_.is_streaming())
    throw std::runtime_error("Agent is already processing");
  auto context = create_context_snapshot();
  auto config = create_loop_config();
  begin_run_locked();

  launch_worker_locked([this, context = std::move(context),
                        config = std::move(config), stream]() mutable {
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

ModelSwitchResult Agent::set_model(Model model, ThinkingLevel thinking,
                                   const std::function<void()> &persist) {
  std::scoped_lock run_lock(worker_mutex_);
  if (state_.is_streaming())
    throw std::runtime_error("model switching requires an idle agent");
  if (!state_.pending_tool_calls().empty())
    throw std::runtime_error(
        "model switching is unavailable while tool execution is pending");
  {
    std::scoped_lock queue_lock(steering_mutex_, followup_mutex_);
    if (!steering_queue_.empty() || !followup_queue_.empty())
      throw std::runtime_error(
          "model switching requires empty steering and follow-up queues");
  }

  const auto previous = state_.model();
  const auto normalized = resolve_thinking_level(model, thinking);
  std::optional<std::string> warning = normalized.warning;
  if (model.context_window != 0) {
    const auto estimated = estimate_context_tokens(state_.messages());
    if (estimated > model.context_window) {
      const auto context_warning =
          "raw context estimate " + std::to_string(estimated) +
          " tokens exceeds " + model.provider + "/" + model.id +
          " context window " + std::to_string(model.context_window) +
          " tokens; context preparation will be attempted";
      if (warning)
        *warning += "; " + context_warning;
      else
        warning = context_warning;
    }
  }
  if (persist)
    persist();
  state_.set_model_and_thinking(std::move(model), normalized.level);
  return {.previous = previous,
          .current = state_.model(),
          .thinking_level = normalized.level,
          .warning = std::move(warning)};
}

ModelSwitchResult
Agent::restore_session(Model model, ThinkingLevel thinking,
                       std::vector<Message> messages, std::string session_id,
                       std::optional<std::string> session_name) {
  std::scoped_lock run_lock(worker_mutex_);
  if (state_.is_streaming())
    throw std::runtime_error("session restoration requires an idle agent");
  if (!state_.pending_tool_calls().empty())
    throw std::runtime_error(
        "session restoration is unavailable while tool execution is pending");
  {
    std::scoped_lock steering_lock(steering_mutex_);
    std::erase_if(steering_queue_, [](const AgentMessageEnvelope &envelope) {
      return envelope.source == AgentMessageSource::mailbox;
    });
    if (!steering_queue_.empty())
      throw std::runtime_error(
          "session restoration requires an empty steering queue");
  }
  {
    std::scoped_lock followup_lock(followup_mutex_);
    if (!followup_queue_.empty())
      throw std::runtime_error(
          "session restoration requires an empty follow-up queue");
  }

  const auto previous = state_.model();
  const auto normalized = resolve_thinking_level(model, thinking);
  state_.set_session_state(std::move(model), normalized.level,
                           std::move(messages), std::move(session_id),
                           std::move(session_name));
  return {.previous = previous,
          .current = state_.model(),
          .thinking_level = normalized.level,
          .warning = normalized.warning};
}

void Agent::set_session_identity(std::string session_id,
                                 std::optional<std::string> session_name) {
  std::scoped_lock run_lock(worker_mutex_);
  if (state_.is_streaming())
    throw std::runtime_error("session changes require an idle agent");
  {
    std::scoped_lock steering_lock(steering_mutex_);
    std::erase_if(steering_queue_, [](const AgentMessageEnvelope &envelope) {
      return envelope.source == AgentMessageSource::mailbox;
    });
    if (!steering_queue_.empty())
      throw std::runtime_error(
          "session changes require an empty steering queue");
  }
  {
    std::scoped_lock followup_lock(followup_mutex_);
    if (!followup_queue_.empty())
      throw std::runtime_error(
          "session changes require an empty follow-up queue");
  }
  state_.set_session_identity(std::move(session_id), std::move(session_name));
}

void Agent::steer(std::vector<Message> messages) {
  steer_envelopes(make_message_envelopes(std::move(messages)));
}

void Agent::steer_envelopes(std::vector<AgentMessageEnvelope> messages) {
  std::scoped_lock lock(steering_mutex_);
  steering_queue_.insert(steering_queue_.end(),
                         std::make_move_iterator(messages.begin()),
                         std::make_move_iterator(messages.end()));
}

void Agent::clear_steering_queue() {
  std::scoped_lock lock(steering_mutex_);
  steering_queue_.clear();
}

void Agent::clear_mailbox_steering_queue() {
  std::scoped_lock lock(steering_mutex_);
  std::erase_if(steering_queue_, [](const AgentMessageEnvelope &envelope) {
    return envelope.source == AgentMessageSource::mailbox;
  });
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

void Agent::interrupt(TurnAbortReason reason) {
  // Synchronize with begin_run(), which replaces the stop source between
  // turns. The lock is not held while the worker executes.
  std::scoped_lock run_lock(worker_mutex_);
  {
    std::scoped_lock lock(interrupt_mutex_);
    interrupt_reason_ = reason;
  }
  state_.stop_source().request_stop();
}

void Agent::abort() { interrupt(TurnAbortReason::user_interrupt); }

void Agent::reset() {
  state_.reset();
  clear_steering_queue();
  clear_follow_up_queue();
}

void Agent::wait_for_idle() { state_.wait_until_idle(); }

void Agent::join_workers() {
  std::vector<std::jthread> workers;
  {
    std::scoped_lock lock(worker_mutex_);
    // An active worker must remain owned by Agent while callers perform the
    // idle check under worker_mutex_. In particular, a concurrent prompt must
    // fail immediately instead of joining a request that can only finish
    // after its caller drains the stream or interrupts it.
    if (state_.is_streaming())
      return;
    workers.swap(workers_);
  }
}

void Agent::begin_run() {
  std::scoped_lock lock(worker_mutex_);
  begin_run_locked();
}

void Agent::begin_run_locked() {
  if (state_.is_streaming()) {
    throw std::runtime_error(
        "Agent is already processing a prompt. "
        "Use steer() or follow_up() to queue messages, or wait for "
        "completion.");
  }
  state_.reset_stop_source();
  {
    std::scoped_lock lock(interrupt_mutex_);
    interrupt_reason_.reset();
  }
  state_.clear_error_message();
  state_.set_streaming(true);
  state_.set_complete(false);
}

void Agent::launch_worker(std::function<void()> worker) {
  std::scoped_lock lock(worker_mutex_);
  launch_worker_locked(std::move(worker));
}

void Agent::launch_worker_locked(std::function<void()> worker) {
  try {
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
  config.prepare_context = options_.prepare_context;
  config.get_abort_reason = [this] {
    std::scoped_lock lock(interrupt_mutex_);
    return interrupt_reason_.value_or(TurnAbortReason::unknown);
  };
  config.get_auth = options_.get_auth;
  config.get_api_key = options_.get_api_key;
  config.should_stop_after_turn = options_.should_stop_after_turn;
  config.get_steering_messages = [this]() {
    std::scoped_lock lock(steering_mutex_);
    std::vector<Message> messages;
    messages.reserve(steering_queue_.size());
    for (auto &envelope : steering_queue_)
      messages.push_back(std::move(envelope.message));
    steering_queue_.clear();
    return messages;
  };
  config.get_steering_envelopes = [this]() {
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
  if (options_.on_event) {
    try {
      options_.on_event(event);
    } catch (...) {
      static_cast<void>(0); // Observers must not take down the agent loop.
    }
  }

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
