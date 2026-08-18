#pragma once

#include "core/agent_state.h"
#include "core/event_types.h"
#include "core/llm_client.h"
#include "core/message_types.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace pi::core {

enum class CompactionTrigger {
  manual,
  automatic_pre_turn,
  context_window_retry,
};

std::string_view compaction_trigger_to_string(CompactionTrigger trigger);

// Provider-neutral, static capability query: does this model's API expose a
// dedicated remote compaction endpoint? V1 recognizes only the OpenAI Codex
// Responses API; every other API (including ordinary OpenAI-compatible
// completion providers) routes to local fallback. This is advisory
// diagnostics/UI plumbing, not a hard gate inside run_compaction(): a
// concrete LLMClient reports per-call support via
// CompactionResult::supported (the default LLMClient::compact()
// implementation already returns supported=false), which remains the
// authoritative signal so test doubles are not forced into this table.
bool supports_remote_compaction(const Model &model);

// Result of validating a parsed CompactionResult's retained items against
// the V1 filtering policy (see plans/server-side-compaction.md §4). By the
// time this runs, provider-level parsing (parse_compact_response) has
// already dropped tool calls/results/reasoning and projected assistant
// content to text only, so this is a defensive re-check plus the
// empty-result rejection — not an active filtering pass with real
// item-dropping work to do.
struct CompactedHistoryValidation {
  bool ok{true};
  std::string error;
};

CompactedHistoryValidation
filter_compacted_history(const std::vector<Message> &messages);

// Outcome reported through the EventStream Agent::compact() returns. This is
// the informational value available at the moment the compaction worker
// finishes (network call + parse + filter) — it does NOT yet reflect
// whether the drain-side durable-write-then-install step succeeded, since
// that happens after this value is produced by whichever thread observes
// the `complete`/`error` CompactionEvent. Callers that need the end-to-end
// result should use AgentSession::compact_active_session (or replicate its
// drain loop), not this struct alone.
struct CompactionOutcome {
  bool success{false};
  bool unsupported{false};
  bool cancelled{false};
  std::optional<std::string> error;
  std::size_t retained_message_count{0};
};

// Everything one compaction attempt needs, independent of Agent, so the
// orchestration is unit-testable without a running agent.
struct CompactionRunRequest {
  AgentContext context;
  std::uint64_t snapshot_epoch{0};
  std::shared_ptr<LLMClient> llm_client;
  CompactionOptions options;
  CompactionTrigger trigger{CompactionTrigger::manual};
  std::uint32_t max_retries{2};
  std::uint32_t max_retry_delay_ms{4000};
};

// Runs one compaction attempt: pushes a `start` CompactionEvent via `push`,
// calls the client (retrying transient transport failures with backoff),
// parses/filters/validates the response, and pushes exactly one
// `complete` or `error` CompactionEvent. Never mutates any Agent/session
// state itself — installation and durable persistence are the caller's
// responsibility, triggered by observing the `complete` event on whatever
// thread drains it (see Agent::compact() / AgentSession::compact_active_session
// for the intended wiring, which keeps that drain on the caller's thread
// rather than this function's own worker thread).
CompactionOutcome run_compaction(const CompactionRunRequest &request,
                                 const std::function<bool(AgentEvent)> &push,
                                 const std::stop_token &stop_tok);

// Phase 5: automatic pre-turn trigger and context-window-error retry (plan
// §7). This is deliberately a separate, dedicated policy rather than a reuse
// of agent_loop.cpp's/agent.cpp's rough byte estimators, which exist to
// drive `prepare_context` and a `set_model` warning respectively — neither
// is meant to be an authoritative "should we spend a compaction round-trip"
// signal. This one is: it is the only estimator that knows about
// `Model::context_window` and a configurable threshold fraction.
struct ContextBudgetPolicy {
  // Fraction of Model::context_window at which automatic compaction should
  // trigger. Reserves the remainder for the next user message, tool
  // definitions, and model output, per plan §7.
  double threshold_pct{0.85};
};

// Conservative, provider-agnostic token estimate for the context as it
// currently stands (system prompt + messages + tool definitions). Uses the
// same bytes-divided-by-4 heuristic as the other ad hoc estimators in this
// codebase; kept local to this policy rather than shared so each caller's
// intent (warning vs. prepare_context budget vs. this threshold policy)
// stays independently adjustable.
std::size_t estimate_context_budget_tokens(const AgentContext &context);

// True when Model::context_window is known (non-zero) and the estimated
// usage has already crossed threshold_pct of it. A model with an unknown
// (zero) context window never triggers automatic compaction.
bool exceeds_context_budget(const AgentContext &context,
                            const ContextBudgetPolicy &policy);

// True when no assistant turn in `context` completed successfully — i.e.
// compaction has nothing to discard. Assistant messages whose stop_reason
// is `error` do not count: an errored request contributes no compactable
// content, so a first-turn context-window error still reads as "no prior
// turns." Shared by both degenerate-case checks below so the two trigger
// paths (our own budget estimate vs. a provider-reported context-window
// error) apply the same definition of "nothing to compact."
bool has_compactable_history(const AgentContext &context);

// The degenerate case plan §7 calls out separately from the ordinary
// repeated-compaction-loop guard: no compactable history exists yet and the
// context is already at or over *our own estimated* budget — e.g. a single
// oversized pasted-file user message as the first turn. Used by the
// automatic pre-turn threshold trigger, which only has our own estimate to
// go on (see the context-window-error path in AgentSession for the
// provider-reported-error variant, which does not require this estimate to
// agree since the provider has already said the request is too large).
bool is_degenerate_oversized_context(const AgentContext &context,
                                     const ContextBudgetPolicy &policy);

// Best-effort detection of a provider "context window exceeded" error from
// its untyped error message string. No pici provider today reports a
// structured error kind for this (see AssistantMessage::error_message), so
// this is a heuristic over vocabulary common across OpenAI/Anthropic-style
// APIs. False negatives just fall back to the ordinary error path (safe);
// false positives trigger one extra, otherwise-harmless compaction attempt
// that a stale-snapshot-free retry then either fixes or gives up on.
bool looks_like_context_window_error(std::string_view message);

} // namespace pi::core
