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

} // namespace pi::core
