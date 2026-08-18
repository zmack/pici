# Server-side conversation compaction

## Status

This is an implementation plan for adding Codex-style server-side compaction to
pici. It is intentionally scoped as a plan, not an implementation. The first
milestone targets the existing OpenAI Codex Responses provider and its dedicated
`/responses/compact` endpoint. Local summarization remains the fallback for
providers that do not advertise server-side compaction.

This plan has been through one architecture review against the current
codebase (see "Review resolutions" below); the five must-fix findings from
that review are folded into the relevant sections in place, so the plan as
written is ready to start at Phase 0.

## Review resolutions (must-fix, folded into the sections below)

This plan went through an architecture review against the actual code
(`agent.cpp`, `agent_state.h`, `session_store.cpp`, `transform_messages.cpp`,
`event_types.h`). The review confirmed the overall shape — capability model,
unary operation, typed opaque item, durable-before-memory ordering, content
filtering — is sound, but found five issues that block a productive Phase 0
and must be resolved before implementation starts. Each is called out inline
where it applies, and summarized here for a single reading:

1. **Journal-write ordering race.** `Agent::prompt()` streams events to a
   caller-drained `EventStream`; `AgentSession` persists on `MessageEndEvent`
   from the *drain* thread, not the worker thread. A compaction record must
   never be written directly from the worker — it must flow through the same
   event-drain persistence path via a `CompactionEvent`, or replay can
   resurrect content the compaction was supposed to remove. See §5.
2. **No epoch guard against a concurrent prompt (not just a concurrent
   compaction).** §5's original snapshot/install description only guarded
   against two compactions racing each other. A prompt started between
   snapshot and install can have its messages silently discarded when the
   snapshot is installed. Fixed by reusing `Agent::worker_mutex_` with a
   monotonic transcript epoch. See §5.
3. **Parent-compacts-while-child-exists corruption.** `parent_offset` is a
   raw message-count index into the parent's replayed vector
   (`session_store.cpp:114`, set from `state().messages().size()` in
   `main.cpp:2191`). If the parent compacts, that index no longer means what
   it meant when the child forked. See §6.
4. **Contradictory/under-specified journal schema.** The original §6 example
   carried both a `through` offset and a full `messages` replacement, and
   never said what the loader does with a malformed compaction record (today
   it silently swallows unknown/malformed records — see `load_recursive` in
   `session_store.cpp`). Fixed by committing to full-replacement-only and
   requiring malformed records to throw. See §6.
5. **The opaque-item type was deferred ("prefer public typed variant") but
   Phase 1 cannot start without deciding it.** Resolved: it is a public 4th
   `Message` alternative. See Phase 0 and §1/§9 below.

The cross-plan dependency this document originally treated as aspirational
(sharing a lock/invariant with
`plans/configurable-providers-and-live-model-switching.md`) is in fact
already implemented today: `Agent::set_model` holds `worker_mutex_` and
rejects streaming/pending-tool/queued-steering state
(`agent.cpp:241-254`). Compaction should reuse that exact mechanism rather
than inventing a parallel one — see the Risks section.

## Goal

When a conversation approaches a model's context limit, pici should be able to
send the current Responses-compatible transcript to a provider-owned compaction
endpoint. The endpoint returns a replacement transcript containing retained
conversation items and an opaque compaction item. Pici installs that replacement
as the live context and persists it, so subsequent requests send the compacted
history rather than the old assistant/tool transcript.

The feature must support both:

- **Automatic compaction** before or during a later agent turn.
- **Manual compaction** from the CLI/session command surface.

Compaction is a transcript boundary, not merely a request-local truncation. A
successful operation must update the in-memory agent state and durable session
journal atomically from the user's perspective.

## Non-goals for the first milestone

- Reimplementing Codex's Rust rollout tracing system.
- Supporting every provider's proprietary compaction API.
- Replacing local Lua/add-on context preparation.
- Compaction while an individual HTTP stream or tool execution is still active.
- Changing the user's visible transcript into the provider's opaque encrypted
  summary. The opaque server item is retained internally and serialized safely.
- Implementing Codex's newer V2 `compaction_trigger` request item in the first
  pass. V2 should remain a follow-up extension once the V1 endpoint is stable.

## Reference implementation studied

The checked-out Codex implementation is under `vendor/codex/codex-rs/`.
Relevant files:

- `core/src/compact.rs` — local compaction lifecycle, replacement history,
  hooks, analytics, and window boundaries.
- `core/src/compact_remote.rs` — V1 remote compaction orchestration and filtering
  of the server-returned transcript.
- `core/src/compact_remote_request.rs` — request construction and invocation of
  `ModelClient::compact_conversation_history`.
- `core/src/compact_remote_v2.rs` and `compact_remote_v2_attempt.rs` — newer
  trigger-based remote protocol; defer unless the provider requires it.
- `core/src/client.rs` — unary `/responses/compact` request, timeout, headers,
  auth, request metadata, and response handling.
- `core/src/tasks/compact.rs` — provider capability dispatch between V2, V1,
  and local compaction.
- `core/src/session/turn.rs` — automatic-compaction trigger points.
- `core/src/state/auto_compact_window.rs` — compaction-window counters and IDs.
- `core/tests/suite/compact_remote.rs` — endpoint, replacement-history,
  authentication, retry, and follow-up tests.
- `core/tests/common/responses.rs` — mock `/responses/compact` responses.

The important Codex behavior is:

1. Clone the current history and normalize oversized function-call outputs when
   necessary.
2. Convert the history to the provider's Responses input representation.
3. POST a unary request to `/responses/compact` using the same model, tools,
   instructions, reasoning settings, authentication, and session metadata as a
   normal Responses request.
4. Receive a JSON object whose `output` is the replacement list.
5. Filter unsafe/stale context from that output, preserving real user content,
   hook prompts, assistant content supported by the provider, and compaction
   items while dropping stale developer wrappers and tool artifacts.
6. Advance the compaction window and install the replacement history.
7. Recompute token accounting and emit completion/error lifecycle events.
8. Retry transient transport failures and, where configured, fall back to local
   compaction or another model.

## Current pici baseline

Relevant existing code:

- `src/core/llm_client.h` exposes only `LLMClient::stream`; there is no unary
  provider operation abstraction.
- `src/core/providers/openai_codex_responses.cpp` already builds a Responses
  request, converts `UserMessage`, `AssistantMessage`, and
  `ToolResultMessage`, parses SSE, response IDs, and usage, and knows the Codex
  endpoint base URL.
- `src/core/message_types.h` has `Message` as a variant of user, assistant, and
  tool-result messages. It has no opaque compaction item.
- `src/core/agent_state.h` stores the complete in-memory transcript but has no
  context-window or compaction-window state.
- `src/core/agent_loop.cpp` estimates context size and supports request-local
  `prepare_context` and `transform_context` callbacks, but does not install a
  replacement transcript after a provider call.
- `src/core/session/agent_session.cpp` persists each `MessageEndEvent` and
  supports append-only `truncate` journal records. There is no replacement or
  compaction journal record.
- `src/core/session/session_store.{h,cpp}` reconstructs sessions from JSONL and
  already handles replayable journal operations.
- `src/core/models.cpp` and `Model` expose context-window limits and provider/API
  identity, which can drive capability selection.
- `src/core/event_types.h` and `src/core/stream_renderer.h` provide the event and
  renderer seams needed for a visible “compacting” status without coupling the
  provider to the UI.
- `src/core/cli` and `src/main.cpp` own commands, configuration, authentication,
  and the interactive session lifecycle.

The existing `openai-codex-responses` client uses SSE only for normal requests.
The compact endpoint must therefore be a separate unary method; do not fake a
compaction request by calling `stream()` and parsing a normal assistant answer.

## Proposed architecture

### 1. Provider capability and operation types

Add a small provider-neutral capability model, initially attached to `Model` or
resolved from its API identifier. A full `unsupported`/`responses_v1`/
`responses_v2` enum is premature for a single working provider; start with a
plain capability flag and widen it in Phase 6 alongside the code that
actually needs a second variant:

```cpp
bool supports_remote_compaction(const Model &model);
```

The first implementation should return `true` only for the existing Codex
Responses API. Unknown APIs and ordinary OpenAI-compatible completion
providers remain `false`, routing to local fallback.

Add a provider-neutral result type containing:

- Replacement `std::vector<Message>` or provider-neutral response items.
- Optional opaque compaction payload/token.
- Optional response ID.
- `TokenUsage`.
- Any server metadata needed for diagnostics.

Prefer returning a provider-neutral transcript item type rather than exposing
OpenAI JSON throughout the agent loop. If the opaque item cannot fit the current
`Message` variant cleanly, introduce an internal `ContextCompactionMessage` and
make it a first-class persisted message. Do not store opaque server state in a
side table that can become detached from the transcript.

### 2. Unary LLM operation

Extend `LLMClient` with a virtual operation, for example:

```cpp
virtual CompactionResult compact(
    const Model &model,
    const AgentContext &context,
    const CompactionOptions &options,
    std::stop_token stop_tok = {}) {
  return unsupported;
}
```

The default implementation should return a typed unsupported result, not throw.
`OpenAICodexResponsesClient` overrides it. This keeps orchestration independent
of HTTP and makes faux-client tests straightforward.

`CompactionOptions` should carry the same request concerns as streaming:

- auth and legacy API-key compatibility;
- headers and timeout;
- reasoning level/effort and summary mode;
- session ID and request metadata;
- diagnostics and response callback;
- stop token.

Add a dedicated `compact_conversation` helper in the HTTP/provider layer that:

- derives the compact URL from `Model::base_url` using the existing endpoint
  normalization;
- appends `/compact` to the Responses endpoint without double-appending it;
- sends JSON, not SSE;
- uses a full-request timeout because the endpoint is unary;
- preserves auth, custom headers, request IDs, and diagnostics;
- rejects malformed HTTP status or JSON responses with actionable errors.

### 3. Responses request and response schema

Reuse the existing `convert_input` logic, but refactor it into a shared internal
converter so normal Responses and compact Responses cannot drift. The compact
request should include the equivalent of Codex's payload:

```json
{
  "model": "...",
  "input": [],
  "instructions": "...",
  "tools": [],
  "parallel_tool_calls": true,
  "reasoning": {"effort": "...", "summary": "..."},
  "text": {"verbosity": "low"},
  "prompt_cache_key": "..."
}
```

Only include optional fields when supported/configured. Respect provider auth
rules: Codex omits service-tier-like fields for API-key requests if the normal
provider contract requires that behavior.

Parse a response object with an `output` array. Each item needs a strict, bounded
conversion path:

- ordinary user/assistant output messages map to pici message types;
- `compaction` items map to the new opaque compaction message;
- unknown items are ignored only if the provider contract marks them ignorable;
- malformed required items fail the compaction rather than silently installing a
  partial transcript;
- cap diagnostic/error text and never print opaque encrypted content.

Preserve the server compaction item's exact opaque content and relevant IDs. It
must round-trip through the next normal request and through session persistence.

### 4. Transcript representation and filtering

Add an internal/persisted message type for a server compaction item, with fields
such as:

- provider/API kind;
- opaque `encrypted_content` or equivalent payload;
- optional item ID;
- optional response/turn metadata;
- timestamp if useful for diagnostics.

Update all of the following together:

- `Message` variant and visitors;
- `json::to_json`, `json::to_jsonl_line`, and `json::from_json`;
- message equality/test helpers as applicable;
- provider conversion so the item becomes a Responses `compaction` input item;
- provider conversion for providers that cannot understand it (drop or fail by
  explicit policy, never send it accidentally to a different API);
- tool/history code that assumes every message is one of three roles;
- transcript renderers so opaque payloads are not displayed as raw data.

Implement a `filter_compacted_history` function modeled on Codex's
`should_keep_compacted_history_item`:

- retain real user messages and explicitly supported hook/add-on prompts;
- retain assistant messages only if returned and supported by the endpoint;
- retain compaction items;
- drop tool calls and tool results unless the provider explicitly says they are
  valid in compacted output;
- drop stale provider-generated instruction/developer wrappers;
- reject a result that has neither meaningful retained context nor a compaction
  item, unless the endpoint documents an empty result as valid.

The filter must operate on typed items after parsing, not on JSON string matching.

Codex's filter operates on a flat `ResponseItem` list, where tool calls, tool
outputs, and reasoning are independent items, and
`should_keep_compacted_history_item` (`compact_remote.rs:370`) drops *all*
`FunctionCall`, `FunctionCallOutput`, and `Reasoning` items unconditionally —
retained assistant content is text only. Pici's `Message` variant is not
flat: `AssistantMessage::content` is a `std::vector<ContentBlock>` that mixes
`TextContent`, `ThinkingContent`, and `ToolCall` in one message
(`src/core/message_types.h`), but the V1 filtering rule is simple, not
conditional: **project every retained assistant message to its `TextContent`
blocks only**, dropping `ThinkingContent` and `ToolCall` blocks
unconditionally, and drop the message entirely if that leaves it empty. Do
not build a "drop the ToolCall only if its paired result was dropped"
conditional — the pairing is never mixed for V1, so a conditional rule is
unneeded complexity that codex itself does not implement.

The pairing hazard that does matter is the mirror image, and it is not
specific to compaction: a retained `ToolResultMessage` with no preceding
`ToolCall` (e.g. one whose call was in a dropped assistant message) becomes a
bare `function_call_output` with no matching `function_call`, which the
Responses API rejects with a 400 (`openai_codex_responses.cpp:206-210`).
Pici already handles the opposite case — an orphaned `ToolCall` with no
result gets a synthesized `"No result provided"` result in
`transform_messages.cpp:169` — so fix this the same way, in
`transform_messages.cpp`, so every provider benefits, not just the
compaction path: drop (or synthesize a placeholder for) any
`ToolResultMessage` whose `ToolCall` is not present in the transcript being
sent. Add a compaction-specific test asserting the general invariant holds
after filtering (no orphaned `ToolResultMessage` survives), but implement the
fix once, upstream of compaction.

### 5. Compaction transaction and orchestration

Introduce a `CompactionManager` or equivalent core helper. It should own one
operation from trigger through installation:

1. Verify the agent is between turns and not executing tools.
2. Snapshot the current `AgentContext` and transcript under the existing state
   locking rules.
3. Emit a compaction-start event/status.
4. Run pre-compaction hooks if the add-on API supports them; cancellation must
   stop before making the network request.
5. Select remote or local strategy from provider capability and configuration.
6. Call the unary client with retry/backoff policy.
7. Parse and filter the replacement history.
8. Append a durable compaction/replacement journal record and confirm it is
   flushed to disk.
9. Install the replacement transcript in one in-memory state update, only after
   step 8 has confirmed durability.
10. Recompute estimated usage and clear/reset the current context-window budget.
11. Emit compaction-complete and post-compaction hook events.

Persist before installing in memory, not after. The plan's earlier durability
requirement ("update the in-memory agent state and durable session journal
atomically from the user's perspective") is only true if the durable record
lands first: if the process crashes between an in-memory install and a
not-yet-flushed journal write, a restart replays the old journal and silently
reverts a compaction the user was told succeeded, with no record that anything
was attempted. Writing the journal record first means the worst case of a crash
before the in-memory install is a session that, on reload, is already in the
post-compaction state — consistent, if not what was in memory at the instant of
the crash. If the durable write itself fails, abort before installing anything
in memory and report the error (see "Durable session journal").

**Do not write the compaction journal record from the compaction worker
thread.** `Agent::prompt()` streams events through an `EventStream` that the
*caller* drains; `AgentSession` today persists ordinary messages from
`MessageEndEvent` observed on that drain thread, not from the worker
(`agent_session.cpp:240-256`). If a compaction record is appended directly by
the worker, it can land in the JSONL file interleaved before a
`MessageEndEvent` that logically precedes it — on replay, the "old" message
gets appended *after* the replacement record, resurrecting content the
compaction removed. Route compaction the same way ordinary messages go:
introduce a `CompactionEvent` (or reuse `MessageEndEvent` with a payload
tag) carrying the replacement transcript, and have the existing
drain-thread persistence handler write the compaction journal record in
stream order alongside message records. This also gives Phase 4 (manual
compaction) its event pump for free (see "Manual command" below) instead of
needing a separate synchronous result path.

The state installation must not mutate the live transcript incrementally while
the request is in flight. On any failure, retain the original transcript and
report the error; do not persist a half-compacted state.

**Guard against a concurrent prompt, not only a concurrent compaction.** A
single operation mutex prevents two compactions from racing each other, but
does nothing to stop `Agent::prompt()` starting *after* step 1's idle check
and appending a user+assistant turn before step 9 replaces the whole message
vector — those messages would be silently discarded, and if the durable
compaction record was already written (per the ordering above), memory and
disk now permanently disagree. Reuse the exact mechanism `Agent::set_model`
already uses for the equivalent problem (`agent.cpp:241-254`): take
`worker_mutex_` for the full snapshot→journal→install critical section (not
just around the install), add a monotonic `transcript_epoch` to
`AgentState` bumped on every message append, and recheck the epoch after
acquiring the lock and before installing. A request that finds the epoch
changed since its snapshot fails with a stale-snapshot error and the caller
retries from fresh state. See "Risks" below — extracting
`Agent::with_idle_transition()` from `set_model`'s guard lets both features
share one implementation instead of two independently-maintained locks.

This process-local lock only guards one process's own compaction attempts
against its own prompts. It does not protect against a second process (a
resumed CLI, an attached ACP/RPC client) appending an ordinary message to the
same session file between this process's snapshot and its journal write.
`SessionStore` holds long-lived append `ofstream`s and exposes no offset API
(`session_store.h:47`), so a `file_size`-then-write generation check is a
TOCTOU race, not a real guard. For V1, either (a) take an advisory
`flock`/`fcntl` lock on the session file for the check-and-write span, or (b)
explicitly scope out cross-process concurrent writers for V1, document the
residual window, and fail closed (abort the compaction, do not install) if
the file's size at write time differs from the size at snapshot time. Do not
ship a check that looks like a guard but is actually racy — that is worse
than no check, because it hides the gap. This is a pre-existing issue for
`append_truncate` too, but a replacement record makes the failure mode
silent data loss instead of a merely confusing truncate.

### 6. Durable session journal

Extend `SessionStore` with a replayable replacement operation carrying the
**full typed replacement transcript only** — no offset field. The original
draft of this record carried both a `through` offset and a full `messages`
replacement, which is self-contradictory (the surrounding prose already says
"do not rely on a byte offset into a prior JSONL file," since forks and prior
truncation records make offsets ambiguous). Drop `through` entirely:

```json
{
  "type": "compaction",
  "messages": [ ... full replacement transcript ... ],
  "provider": "openai-codex",
  "model": "...",
  "summary": "server",
  "timestamp": 0
}
```

Replay rules:

- load the parent session first, as today;
- apply ordinary message and truncate records in order;
- when a compaction record is encountered, replace the current message vector
  wholesale with its `messages`;
- subsequent messages append after the replacement;
- **malformed compaction records must throw, not be silently skipped.** Today
  `load_recursive` silently `continue`s past both JSON parse failures and
  unknown `type` values (`session_store.cpp:76`, `92-105`). Under that
  behavior a malformed compaction record does not fail loudly — it silently
  restores the pre-compaction transcript, exactly the "corrupted session"
  outcome this plan already says must not happen silently. Add explicit
  `type == "compaction"` handling that throws on a malformed record, and add
  a regression test for it; do not rely on the existing fallthrough;
- old sessions without compaction records remain fully compatible; add a
  session-header `minVersion` (or equivalent) so an older pici binary that
  does not understand the `compaction` record type fails closed instead of
  silently replaying the pre-compaction transcript as if it were current.

Extend the JSONL record model from its current
`std::variant<Message, std::size_t>` (message vs. truncate-through) to a
third alternative for the compaction record, rather than overloading either
existing arm.

Add `append_compaction` and a corresponding `AgentSession::install_compaction`
(or equivalent) that updates memory and persistence as one coordinated action
through the event-drain path described in "Compaction transaction and
orchestration" above — not a direct worker-thread write. If persistence
fails, keep the in-memory state marked dirty and surface the error; do not
claim durable success.

**Forked sessions: handle both directions, not just child-compacts-child.**
The original draft only addressed a child compacting its own inherited
history ("compaction belongs to the child journal only... does not modify
the parent file") — that direction is fine as stated. But `parent_offset` is
a raw message-count index into the parent's fully replayed vector
(`session_store.cpp:114`, set at fork time from `state().messages().size()`
in `main.cpp:2191`), and nothing invalidates that index if the *parent*
later compacts. After a parent compaction, `parent_offset` either exceeds the
parent's new (shorter) message count — `session_store.cpp`'s loader already
throws `"corrupted fork"` in that case, so every existing child becomes
permanently unloadable — or it stays in range but now points at a different
logical position, so the child silently splices the wrong prefix. Pick one
before Phase 1 (it changes the header schema either way):

- **(a) Refuse:** reject compaction on any session that has children on
  disk, and require "fork, then compact the fork" as the supported flow; or
- **(b) Version it:** add a `parentCompactionEpoch` field to the child
  header, bump a matching epoch on the parent whenever it compacts, and fail
  loudly with an actionable error at load time on a mismatch instead of
  either throwing an opaque "corrupted fork" or silently misloading.

Option (a) is simpler and should be the V1 default; note (b) as the
follow-up if compacting a session with live children turns out to be a
common operator workflow.

### 7. Automatic trigger and token accounting

Add a context-window policy separate from the rough byte estimator. At minimum:

- use `Model::context_window` when available;
- reserve space for the next user message, tool definitions, and model output;
- trigger before the request when estimated usage crosses a configurable
  threshold, initially 80–90%;
- if the provider returns a context-window error, retry once through compaction
  before surfacing the error;
- avoid repeated compaction loops when a compacted result is still too large;
- record a per-window “already compacted” or generation marker.

Handle the degenerate case separately from the repeated-loop case above: a
single oversized first turn (e.g. a large pasted file as the first user
message) can exceed the threshold before any assistant/tool history exists to
discard. There is no prior compaction attempt to loop on, and compaction has
nothing to remove. Detect this case explicitly (no assistant turns yet, or a
single user message already at/over budget) and fail with an actionable error
distinct from "compaction did not reduce size enough" — do not silently attempt
a remote compaction call that the server will just return unchanged (or that
uses a full turn's worth of latency and quota to learn nothing).

Model the Codex auto-compaction window state with a small pici type:

- monotonically increasing window number;
- current and previous opaque IDs if useful for request metadata;
- prefill baseline from server-reported input usage when available;
- one-shot reminder/fallback flags.

Do not count the entire old transcript after installation. Recompute usage from
the replacement and treat the next request's server-reported input as the new
baseline. Existing `TokenUsage` fields should remain provider-agnostic.

Automatic compaction should happen at a safe boundary:

- before starting a new LLM request; or
- after a completed assistant/tool turn and before the next follow-up;
- never while an assistant stream is active;
- never while tool calls from that response are pending.

The first implementation can use a pre-turn trigger, but "pre-turn" needs a
precise definition: `Agent::prompt()` appends the new user message before the
turn's LLM call begins, so a trigger that fires "before starting a new LLM
request" already has that pending user message sitting in the transcript
being compacted — it must not be summarized away. Codex resolves the
equivalent case with `insert_initial_context_before_last_real_user_or_summary`;
pici has no analogous seam yet. Rather than build one, define the V1 trigger
point as **after a completed assistant/tool turn and before the next
prompt's user message is appended** — i.e. compact `messages[0..n]` as they
stand at turn-end, not mid-append. This is already one of the two safe
boundaries this section lists, so it requires no new logic, only picking it
over the other. Mid-turn compaction should be a separate milestone because it
requires preserving the final user message, active tool state, and
cancellation semantics.

### 8. Manual command and UI behavior

Expose manual compaction through the existing command dispatch rather than
embedding it in the provider. The command should:

- refuse while a turn or tool execution is active, or queue safely;
- show a renderer status such as `Compacting context...`;
- use the active model/provider and auth;
- leave the session usable after success;
- make failure explicit without replacing the transcript.

Add renderer/event types for start, completion, and failure. Raw and pipe
renderers should receive concise plain text; viewport/markdown renderers should
not render the opaque server payload. Include retained-message counts and token
before/after values in verbose diagnostics only.

### 9. Retry and fallback policy

Follow Codex's separation between remote attempt and local fallback:

- retry transient transport failures using the existing retry limits/backoff;
- do not retry cancellation, authentication failure, invalid request, malformed
  response, or context-window errors indefinitely;
- preserve one original error if a fallback also fails, while including fallback
  context in diagnostics;
- if remote compaction is unsupported or explicitly disabled, call the existing
  local/add-on compaction path;
- make fallback configurable so deployments can require remote compaction and
  fail closed rather than sending a local summary.

Never send the opaque compaction payload to a provider/API that did not create
it. On model/provider switching, either remote compact again from the canonical
messages or discard incompatible opaque items according to an explicit policy.

### 10. Observability and privacy

Add structured diagnostics/metrics for:

- trigger: manual, automatic, context-window retry;
- strategy: remote V1 or local fallback;
- provider/model/API;
- duration and retry count;
- estimated tokens before/after;
- retained item/message counts;
- result: success, cancelled, unsupported, failed;
- HTTP status and response ID where safe.

Do not log request bodies, user text, tool output, or encrypted compaction
content. Existing `StreamDiagnostics` JSONL tracing should identify a compact
request as a distinct operation and redact payload contents by default.

## Suggested implementation sequence

### Phase 0 — contract and fixtures

- Document the exact provider request and response shape used by the checked-out
  Codex implementation.
- Add representative JSON fixtures for a normal transcript, a compact response,
  malformed output, empty output, and opaque compaction content.
- **Decided:** the opaque item is a public 4th `Message` variant,
  `ContextCompactionMessage{api, provider, model, encrypted_content, item_id,
  response_id, timestamp}`. This is not deferrable — Phase 1 ("add the
  compaction message/item type") cannot start without it, and Phase 1 depends
  on this decision, so it is made here rather than left as a Phase 1 task.
  Carrying `api`/`provider`/`model` on the item lets it reuse the *existing*
  same-model gate in `transform_messages.cpp:102` (which already restricts
  redacted/encrypted reasoning content to the exact provider/api/model that
  produced it) to satisfy the "never send opaque payload to a provider that
  did not create it" requirement in "Retry and fallback policy" below, with
  no new policy layer needed.
- Add a feature/config flag, defaulting to automatic remote compaction off until
  the end-to-end path is complete.

### Phase 1 — typed transcript support

- Add the compaction message/item type.
- Implement JSONL serialization and replay.
- Refactor message visitors and Responses conversion to handle it.
- Add `SessionStore::append_compaction`, replay, and fork tests.
- Add tests proving opaque content is never rendered or logged.

### Phase 2 — unary client operation

- Add `CompactionOptions` and `CompactionResult`.
- Add the default unsupported `LLMClient` method.
- Refactor shared Responses input conversion.
- Implement the Codex Responses `/compact` URL and JSON POST.
- Parse usage, response ID, output items, HTTP errors, timeout, and cancellation.
  Codex scales the compact request's timeout relative to the normal idle
  timeout (`COMPACT_REQUEST_TIMEOUT_IDLE_MULTIPLIER = 4` in `client.rs:165`)
  rather than inventing a new constant; use the same multiplier so the
  timeout has a documented origin instead of a guessed value.
- Add a fake LLM client and HTTP mock coverage.
- **Phase exit gate:** before starting Phase 3, prove the seam works
  end-to-end at the fixture level — a faux/fake client round-trips a
  canned compact-response fixture through typed parsing → filtering →
  journal write → reload, without a real `CompactionManager` yet. Phases 1–2
  are otherwise dead code until Phase 3 lands; this gate is what makes that
  acceptable instead of accumulating unverified layers.

### Phase 3 — replacement transaction

- Implement typed filtering and validation of compacted output.
- Add `CompactionManager` with snapshot, operation lock, install, and rollback
  behavior.
- Add start/complete/error events and renderer handling.
- Integrate durable replacement journaling.
- Add local fallback and retry policy.

### Phase 4 — manual flow

- Add the manual command to CLI/RPC/ACP surfaces where appropriate.
- Ensure it uses the active session and provider configuration.
- Add interactive, non-TTY, and cancellation behavior.
- Add session reload tests after manual compaction.

### Phase 5 — automatic pre-turn flow

- Add context-window threshold configuration and token-budget state.
- Trigger compaction before a new turn when safe.
- Retry one context-window failure through compaction.
- Prevent duplicate compaction and infinite retry loops.
- Add model/provider switch behavior and compatibility checks.

### Phase 6 — hardening and optional V2

- Run the full test suite, format, lint, and sanitizer builds.
- Add failure-injection tests for every boundary: timeout, malformed JSON,
  cancellation, persistence failure, stale snapshot, and fallback failure.
- Evaluate Codex V2 `compaction_trigger` support. If needed, add a capability
  enum branch, request trigger item, streaming compaction output parser, and
  separate V2 tests without changing V1 semantics.

## File-level change map

Likely pici files to modify or add:

- `src/core/message_types.h/.cpp` — typed compaction item, JSON round-trip,
  usage/visitor support.
- `src/core/llm_client.h/.cpp` — unary compaction interface and default result.
- `src/core/providers/openai_codex_responses.h/.cpp` — compact request,
  response parser, shared Responses item conversion.
- `src/http/http_client.h/.cpp` — authenticated unary JSON POST if the current
  helpers do not already cover it.
- `src/core/agent_state.h/.cpp` — compaction generation/window state and safe
  replacement API, if kept in state rather than a manager.
- `src/core/agent_loop.h/.cpp` — safe pre-turn trigger and context-window error
  retry seam.
- `src/core/session/session_record.h` — compaction metadata if needed.
- `src/core/session/session_store.h/.cpp` — compaction journal write/replay.
- `src/core/session/agent_session.h/.cpp` — coordinated memory/persistence
  installation.
- `src/core/event_types.h/.cpp` and `src/core/stream_renderer.*` — lifecycle
  events/status.
- `src/cli/args.*`, `src/cli/config.*`, `src/main.cpp` — flags, command, and
  policy wiring.
- `test/test_faux_client.cpp` — fake unary compaction behavior.
- `test/test_core.cpp`, `test/test_config.cpp` — serialization/config tests.
- new `test/test_compaction.cpp` — manager, filtering, rollback, and token state.
- new or extended `test/test_session_store.cpp` — journal replay/fork tests.
- `test/test_openai_codex_responses.cpp` — request/response and HTTP behavior.
- `CMakeLists.txt` — new sources and test target.

Names may be adjusted to match the final abstraction, but keep provider-specific
JSON out of `AgentSession` and keep persistence independent of HTTP.

## Test matrix

### Serialization and persistence

- Compaction item serializes and parses with opaque content unchanged.
- Unknown/legacy session records remain loadable.
- Replacement records replay in order after messages and truncations.
- A compacted child fork does not modify its parent.
- Corrupt replacement records fail loudly.
- Persistence failure does not report a successful durable compaction.

### Provider request/response

- URL normalization produces `/responses/compact` exactly once.
- Request contains model, instructions, converted input, tools, reasoning, and
  session metadata as configured.
- Auth and custom headers match normal Responses requests.
- API-key and non-API-key request variants follow provider policy.
- JSON response output is parsed into typed messages and opaque compaction item.
- Assistant/tool artifacts are filtered according to the documented policy.
- Empty, malformed, unknown, and oversized responses fail safely.
- Timeout, cancellation, 401/403, 429, 5xx, and invalid JSON have distinct
  behavior.

### Agent behavior

- Manual compaction replaces the transcript only after a successful response.
- Failed/cancelled compaction leaves the exact original transcript intact.
- Follow-up requests contain the replacement history and opaque compaction item,
  not the pre-compaction assistant/tool history.
- Automatic compaction triggers once at the threshold.
- Context-window error triggers at most one compaction retry.
- A second compaction is not recursively triggered by an already compacted request.
- Compaction cannot race with streaming or tool execution.
- Provider unsupported uses local fallback; strict remote mode fails clearly.

### UX and observability

- Interactive UI shows lifecycle status without exposing opaque content.
- Pipe/RPC/ACP clients receive machine-readable lifecycle/error events.
- Diagnostics contain counts/timing/status but no prompts, tool output, or tokens.
- Compaction events are correctly handled by every built-in renderer.

## Acceptance criteria

The first milestone is complete when:

1. An `openai-codex-responses` session can manually compact through the dedicated
   `/responses/compact` endpoint.
2. The replacement transcript and opaque compaction item survive a session save,
   reload, and follow-up request.
3. The old transcript is retained on any failed or cancelled request.
4. Automatic pre-turn compaction works behind a configuration flag and uses the
   same transaction path as manual compaction.
5. Unsupported providers continue using the existing local behavior.
6. Tests cover request shape, filtering, persistence, retries, cancellation,
   forked sessions, and no-secret/no-payload logging.
7. `make test`, `make format-check`, and the applicable lint/sanitizer checks pass.

## Risks and decisions to revisit

- **Opaque item compatibility:** Compaction payloads may be provider- and
  model-specific. Treat them as owned by the originating API and invalidate or
  recompact when switching providers/models. This is not a hypothetical future
  collision: `plans/configurable-providers-and-live-model-switching.md` is an
  active, sibling plan for the same codebase, and it already establishes a
  concrete precedent for exactly this problem — e.g. "images become
  placeholders when switching to a text-only model" and a "safe live switching
  contract" that locks out switching while a turn/tool-use is active. Unlike
  the original draft of this section, this is **not** an aspirational
  parallel to build toward — the mechanism already exists and should be reused
  directly, not merely "shared in spirit":
  - `Agent::set_model` (`agent.cpp:241-254`) already takes `worker_mutex_` and
    rejects a switch while streaming, while tool calls are pending, or while
    steering/follow-up work is queued. That is the exact same guard
    compaction needs (see "Compaction transaction and orchestration" above,
    which now specifies taking `worker_mutex_` for the
    snapshot→journal→install critical section). Extract the guard as
    `Agent::with_idle_transition()` and have both `set_model` and the
    `CompactionManager` call it, rather than maintaining two copies of the
    same four-condition check;
  - a switch away from the provider that produced an opaque compaction item
    should, per that plan's placeholder precedent, replace the opaque item with
    a textual placeholder/marker (or trigger a fresh local/remote compaction
    against the destination provider) rather than forwarding provider-A opaque
    bytes to provider B. The `api`/`provider`/`model` fields committed on
    `ContextCompactionMessage` (Phase 0, above) are what make this check
    possible, via the same gate `transform_messages.cpp:102` already applies
    to redacted reasoning content;
  - because the shared mechanism already exists, this plan has no hard
    ordering dependency on the live-switching plan landing first — extracting
    `with_idle_transition()` is within this plan's own scope.
- **Message model expansion:** Adding a fourth/fifth variant touches many
  visitors. This is preferable to an untyped side channel, but should be done
  in one deliberate change with compiler errors guiding every visitor.
- **Tool retention:** Codex's remote output currently drops tool artifacts. If
  pici users depend on exact tool history for UI/session display, keep the full
  durable journal separately from the active model context, or add a distinct
  display/history projection rather than sending tools back to the provider.
- **Token estimates:** Byte-based estimates are useful for triggering but not
  authoritative. Prefer server usage after the first request and expose the
  estimate's uncertainty in diagnostics.
- **Provider capability discovery:** Start with an explicit API/model capability
  table. Do not infer support solely from a URL that happens to contain
  `responses`.
- **Mid-turn behavior:** Codex supports more advanced mid-turn paths. Defer
  those until pre-turn replacement, persistence, and cancellation semantics are
  proven in pici.
