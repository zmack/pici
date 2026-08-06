# Server-side conversation compaction

## Status

This is an implementation plan for adding Codex-style server-side compaction to
pici. It is intentionally scoped as a plan, not an implementation. The first
milestone targets the existing OpenAI Codex Responses provider and its dedicated
`/responses/compact` endpoint. Local summarization remains the fallback for
providers that do not advertise server-side compaction.

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
resolved from its API identifier:

```cpp
enum class RemoteCompactionSupport {
  unsupported,
  responses_v1,
  responses_v2,
};
```

The first implementation should resolve `responses_v1` only for the existing
Codex Responses API. Unknown APIs and ordinary OpenAI-compatible completion
providers remain `unsupported`.

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
outputs, and reasoning are independent items. Pici's `Message` variant is not
flat: `AssistantMessage::content` is a `std::vector<ContentBlock>` that mixes
`TextContent`, `ThinkingContent`, and `ToolCall` in one message
(`src/core/message_types.h`). A message-level "keep or drop" decision is
therefore insufficient. `filter_compacted_history` must scrub at the content-block
level within retained assistant messages:

- if a `ToolCall` block's paired `ToolResultMessage` is dropped by policy, the
  `ToolCall` block itself must also be stripped from the retained assistant
  message; a retained tool call with no corresponding result is an invalid
  transcript for the next request, not merely an unwanted one;
- `ThinkingContent`/encrypted reasoning blocks follow the same drop policy as
  standalone reasoning items unless the provider contract says otherwise;
- an assistant message that becomes empty after block-level scrubbing is
  dropped entirely rather than sent as a content-less message;
- add a test asserting that no retained `ToolCall` block ever survives without
  a retained `ToolResultMessage`, and vice versa.

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

The state installation must not mutate the live transcript incrementally while
the request is in flight. On any failure, retain the original transcript and
report the error; do not persist a half-compacted state.

Use a single operation mutex or generation counter to prevent concurrent manual
and automatic compaction. A request that starts after a snapshot but before
installation must either wait or fail with a stale-snapshot error and retry from
fresh state.

This in-process mutex only guards one process's own compaction attempts. It
does not protect against a second process (a resumed CLI, an attached ACP/RPC
client) appending an ordinary message to the same session file between this
process's snapshot and its journal write. `SessionStore` has no generation or
version marker on the journal today, so a replacement record written by one
process can silently strand or misorder a message appended concurrently by
another. Add a lightweight generation check (e.g. record the journal's last
known length/offset at snapshot time and fail the compaction if the file has
grown by the time the durable write happens) rather than assuming single-writer
access. This is a pre-existing gap for `append_truncate` too, but a replacement
record makes the failure mode silent data loss instead of a merely confusing
truncate.

### 6. Durable session journal

Extend `SessionStore` with a replayable replacement operation, for example:

```json
{
  "type": "compaction",
  "through": 42,
  "messages": [ ... replacement messages ... ],
  "provider": "openai-codex",
  "model": "...",
  "summary": "server",
  "timestamp": 0
}
```

A better long-term form is a replacement record containing the full typed
replacement transcript. Do not rely on a byte offset into a prior JSONL file:
forks and prior truncation records make offsets ambiguous.

Replay rules:

- load the parent session first, as today;
- apply ordinary message and truncate records in order;
- when a compaction record is encountered, replace the current message vector;
- subsequent messages append after the replacement;
- malformed compaction records are reported as a corrupted session rather than
  silently skipped;
- old sessions without compaction records remain fully compatible.

Add `append_compaction` and a corresponding `AgentSession::install_compaction`
(or equivalent) that updates memory and persistence as one coordinated action.
If persistence fails, keep the in-memory state marked dirty and surface the error;
do not claim durable success.

For forked sessions, compaction belongs to the child journal only. A child may
compact inherited history without modifying the parent file.

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

The first implementation can use a pre-turn trigger. Mid-turn compaction should
be a separate milestone because it requires preserving the final user message,
active tool state, and cancellation semantics.

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
- Decide whether the opaque item is public in `Message` or an internal session
  record type; prefer the public typed variant if it must be sent on follow-up
  requests.
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
- Add a fake LLM client and HTTP mock coverage.

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
  contract" that locks out switching while a turn/tool-use is active. The
  compaction feature should extend that same contract rather than inventing a
  parallel one:
  - a switch away from the provider that produced an opaque compaction item
    should, per that plan's placeholder precedent, replace the opaque item with
    a textual placeholder/marker (or trigger a fresh local/remote compaction
    against the destination provider) rather than forwarding provider-A opaque
    bytes to provider B;
  - the live-switching contract's "reject switching while streaming/tool
    execution is active" rule and this plan's "compaction cannot race
    streaming or tool execution" rule are the same invariant and should share
    one lock/check, not two independently maintained ones;
  - this needs resolving before Phase 5 (automatic compaction), since
    automatic triggers make the provider-switch-with-opaque-history case
    routine rather than rare.
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
