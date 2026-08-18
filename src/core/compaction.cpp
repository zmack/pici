#include "core/compaction.h"

#include "core/agent_state.h"
#include "core/event_types.h"
#include "core/llm_client.h"
#include "core/message_types.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <ranges>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace pi::core {

std::string_view compaction_trigger_to_string(CompactionTrigger trigger) {
  switch (trigger) {
  case CompactionTrigger::manual:
    return "manual";
  case CompactionTrigger::automatic_pre_turn:
    return "automatic_pre_turn";
  case CompactionTrigger::context_window_retry:
    return "context_window_retry";
  }
  return "unknown";
}

bool supports_remote_compaction(const Model &model) {
  return model.api == "openai-codex-responses";
}

CompactedHistoryValidation
filter_compacted_history(const std::vector<Message> &messages) {
  bool has_meaningful_content = false;
  bool has_compaction_item = false;

  for (const auto &msg : messages) {
    if (std::holds_alternative<ContextCompactionMessage>(msg)) {
      has_compaction_item = true;
      continue;
    }
    if (const auto *assistant = std::get_if<AssistantMessage>(&msg)) {
      // Defensive re-check: parse_compact_response already projects
      // retained assistant content to TextContent only and never
      // constructs a ToolCall/ThinkingContent item from V1 output, so this
      // should be unreachable in practice. It guards against a future
      // change to the parser (or a different provider's parser) silently
      // reintroducing items the V1 retention policy forbids.
      for (const auto &block : assistant->content) {
        if (std::holds_alternative<ToolCall>(block) ||
            std::holds_alternative<ThinkingContent>(block))
          return {.ok = false,
                  .error = "compacted history retained a non-text assistant "
                           "content block, which the V1 retention policy "
                           "forbids"};
      }
      has_meaningful_content = true;
      continue;
    }
    if (std::holds_alternative<UserMessage>(msg)) {
      has_meaningful_content = true;
      continue;
    }
    // ToolResultMessage (or any future variant) is never valid in compacted
    // output for V1.
    return {.ok = false,
            .error = "compacted history retained an unsupported message "
                     "type (only user/assistant text and a compaction item "
                     "are valid)"};
  }

  if (!has_meaningful_content && !has_compaction_item)
    return {.ok = false,
            .error = "compaction response contained neither retained "
                     "content nor a compaction item"};

  return {.ok = true, .error = {}};
}

std::size_t estimate_context_budget_tokens(const AgentContext &context) {
  std::size_t bytes = context.system_prompt.size();
  for (const auto &message : context.messages)
    bytes += json::to_json(message).size();
  for (const auto &tool : context.tools)
    bytes += tool->name().size() + tool->description().size() +
             tool->schema().serialize().size();
  return ((bytes + 3) / 4) + (context.messages.size() * 8);
}

bool exceeds_context_budget(const AgentContext &context,
                            const ContextBudgetPolicy &policy) {
  if (context.model.context_window == 0)
    return false;
  const auto budget = static_cast<std::size_t>(
      static_cast<double>(context.model.context_window) * policy.threshold_pct);
  return estimate_context_budget_tokens(context) >= budget;
}

bool has_compactable_history(const AgentContext &context) {
  return std::ranges::any_of(context.messages, [](const Message &message) {
    const auto *assistant = std::get_if<AssistantMessage>(&message);
    return assistant != nullptr && assistant->stop_reason != StopReason::error;
  });
}

bool is_degenerate_oversized_context(const AgentContext &context,
                                     const ContextBudgetPolicy &policy) {
  if (has_compactable_history(context))
    return false;
  return exceeds_context_budget(context, policy);
}

bool looks_like_context_window_error(std::string_view message) {
  std::string lower(message);
  std::ranges::transform(lower, lower.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  static constexpr std::array<std::string_view, 10> needles{
      "context_length_exceeded",
      "context length",
      "context window",
      "maximum context length",
      "too many tokens",
      "reduce the length",
      "prompt is too long",
      "input is too long",
      "exceeds the model's maximum",
      "exceeds context",
  };
  return std::ranges::any_of(needles, [&](std::string_view needle) {
    return lower.find(needle) != std::string::npos;
  });
}

namespace {

TokenUsage last_known_usage(const std::vector<Message> &messages) {
  for (const auto &message : std::ranges::reverse_view(messages)) {
    if (const auto *assistant = std::get_if<AssistantMessage>(&message))
      return assistant->usage;
  }
  return {};
}

// Transient transport failures (no response at all, HTTP 429, or 5xx) are
// retried; cancellation, authentication failures, invalid requests, and
// malformed responses are not, per plan §9. CompactionResult does not carry
// a structured error taxonomy, so http_status is the signal: unset means
// the request never got a response (transient); set means the provider
// responded and its status decides.
bool is_retryable(const CompactionResult &result) {
  if (result.cancelled || !result.error_message)
    return false;
  if (!result.http_status)
    return true;
  return *result.http_status == 429 || *result.http_status >= 500;
}

CompactionResult compact_with_retry(LLMClient &client, const Model &model,
                                    const AgentContext &context,
                                    const CompactionOptions &options,
                                    std::uint32_t max_retries,
                                    std::uint32_t max_retry_delay_ms,
                                    const std::stop_token &stop_tok) {
  CompactionResult result;
  std::uint32_t delay_ms = 250;
  for (std::uint32_t attempt = 0;; ++attempt) {
    result = client.compact(model, context, options, stop_tok);
    if (!result.error_message || result.cancelled || !result.supported)
      return result;
    if (attempt >= max_retries || !is_retryable(result) ||
        stop_tok.stop_requested())
      return result;

    const auto capped_delay = max_retry_delay_ms == 0
                                  ? delay_ms
                                  : std::min(delay_ms, max_retry_delay_ms);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(capped_delay);
    while (std::chrono::steady_clock::now() < deadline &&
           !stop_tok.stop_requested())
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    delay_ms = max_retry_delay_ms == 0
                   ? delay_ms * 2
                   : std::min(delay_ms * 2, max_retry_delay_ms);
  }
}

} // namespace

CompactionOutcome run_compaction(const CompactionRunRequest &request,
                                 const std::function<bool(AgentEvent)> &push,
                                 const std::stop_token &stop_tok) {
  CompactionEvent start(CompactionEventKind::start);
  start.snapshot_epoch = request.snapshot_epoch;
  start.provider = request.context.model.provider;
  start.model = request.context.model.id;
  if (push)
    push(AgentEvent{std::move(start)});

  CompactionOutcome outcome;

  if (!request.llm_client) {
    outcome.unsupported = true;
    outcome.error = "no LLM client configured";
    CompactionEvent error_event(CompactionEventKind::error);
    error_event.snapshot_epoch = request.snapshot_epoch;
    error_event.provider = request.context.model.provider;
    error_event.model = request.context.model.id;
    error_event.error_message = outcome.error;
    error_event.unsupported = true;
    if (push)
      push(AgentEvent{std::move(error_event)});
    return outcome;
  }

  const auto usage_before = last_known_usage(request.context.messages);

  auto result =
      compact_with_retry(*request.llm_client, request.context.model,
                         request.context, request.options, request.max_retries,
                         request.max_retry_delay_ms, stop_tok);

  if (!result.supported) {
    outcome.unsupported = true;
    outcome.error = result.error_message.value_or(
        "remote compaction is not supported by this provider");
  } else if (result.cancelled) {
    outcome.cancelled = true;
    outcome.error = result.error_message.value_or("compaction was cancelled");
  } else if (result.error_message) {
    outcome.error = result.error_message;
  } else {
    const auto validation = filter_compacted_history(result.messages);
    if (!validation.ok) {
      outcome.error = validation.error;
    }
  }

  if (outcome.error || outcome.unsupported || outcome.cancelled) {
    CompactionEvent error_event(CompactionEventKind::error);
    error_event.snapshot_epoch = request.snapshot_epoch;
    error_event.provider = request.context.model.provider;
    error_event.model = request.context.model.id;
    error_event.error_message = outcome.error.value_or(
        outcome.unsupported ? "remote compaction is not supported by "
                              "this provider"
                            : "compaction was cancelled");
    error_event.cancelled = outcome.cancelled;
    error_event.unsupported = outcome.unsupported;
    if (push)
      push(AgentEvent{std::move(error_event)});
    return outcome;
  }

  outcome.success = true;
  outcome.retained_message_count = result.messages.size();

  CompactionEvent complete(CompactionEventKind::complete);
  complete.snapshot_epoch = request.snapshot_epoch;
  complete.replacement_messages = std::move(result.messages);
  complete.provider = request.context.model.provider;
  complete.model = request.context.model.id;
  complete.summary = "server";
  complete.response_id = result.response_id;
  complete.usage_before = usage_before;
  complete.usage_after = result.usage;
  complete.retained_message_count = outcome.retained_message_count;
  if (push)
    push(AgentEvent{std::move(complete)});

  return outcome;
}

} // namespace pi::core
