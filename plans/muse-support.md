# Add Muse Spark support via the Anthropic-compatible Messages API

## Context

Meta's Muse Spark (`muse-spark-1.1`) is a reasoning model reachable through
three wire-compatible surfaces documented in `docs/muse/`: Chat Completions,
Responses, and Messages (Anthropic `/v1/messages`-shaped). We're integrating
via the **Messages API** specifically because it may provide a stateless
encrypted-reasoning replay path through `redacted_thinking` content blocks —
not because it exposes raw chain-of-thought. Muse keeps raw reasoning private;
readable `thinking` content is a summary. Chat Completions redacts
`reasoning_content` to empty for external callers, and the Responses API's
richer reasoning-replay story is a much bigger, separately-scoped lift (new
wire format entirely, no existing client shape in pici to build from).

pici's `ContentBlock` already has a `ThinkingContent{text, thinking_signature,
redacted}` shape (`src/core/message_types.h:44-49`) that maps almost exactly
onto Anthropic/Muse's `thinking`/`redacted_thinking` blocks, and
`Model.thinking_level_map` exists specifically to translate pici's
`ThinkingLevel` enum to a provider-specific string — this integration is the
first real consumer of that field. The goal is a new `LLMClient` alongside the
existing `OpenAICompatibleClient`, not a variant of it (the wire shapes don't
overlap enough to share request-building logic), while reusing the
provider-agnostic `HttpClient` SSE transport and `transform_messages()`
cross-model history normalization as-is.

Verified during planning: `HttpClient::post_streaming` (`src/http/http_client.cpp:121-225`)
already sends `Authorization: Bearer <key>` and passes every raw non-empty
line (including `event:` lines) through to the caller's `on_line` callback —
no changes needed there. `transform_messages.cpp:102-134` already has a
`same_model` gate (`am.provider == model.provider && am.api == model.api &&
am.model == model.id`) that keeps/drops thinking blocks appropriately across
model switches — the new client's round-trip design plugs into this
unchanged as long as `AssistantMessage.provider`/`.api` get set correctly in
`stream()`, mirroring what `openai_completions.cpp` already does.

## New files: `src/core/providers/muse_messages.{h,cpp}`

`class MuseMessagesClient : public LLMClient`, `provider_name()` → `"meta"`,
`api_id()` → `"muse-messages"`. Key functions, mirroring the shape of
`openai_completions.{h,cpp}` (`convert_messages`, `build_request_json`,
`process_sse_line`/`stream`, `map_finish_reason`) but for the Anthropic wire
format:

1. **`convert_messages(model, context)`** — builds Muse's `messages` array +
   top-level `system` string from `context.messages`/`context.system_prompt`,
   after running through the existing `transform_messages()`.
2. **`convert_tools(context.tools)`** — Muse's Messages tools use
   `{"type":"custom","name","description","input_schema","strict",
   "defer_loading"}` (flat, not nested under `function` like Chat
   Completions).
3. **`effort_for_thinking_level(model, level)`** — looks up
   `model.thinking_level_map`; see mapping policy below.
4. **`build_request_json(...)`** — assembles `model`, `messages`, `system`,
   required `max_tokens` (fall back to `model.max_tokens` if
   `options.max_tokens` is unset — Muse 400s without it), `stream`,
   temperature passthrough, optional adaptive thinking configuration plus
   `output_config.effort`, `tools`, and validated string-valued `metadata`.
   `top_p` is intentionally not supported in this phase because pici has no
   corresponding option; adding it requires plumbing through `Agent::Options`,
   `AgentLoopConfig`, and `StreamOptions`. Never emit
   `stop_sequences`, `top_k`, `container`, `inference_geo` (all 400 on Muse).
   **Omit `tool_choice` entirely** — pici's `AgentContext`/`StreamOptions`
   has no field this would come from today, and omission selects Muse's
   default `"auto"`. Muse also supports `any`, `none`, and
   `disable_parallel_tool_use`; named tool choices are unsupported. Adding
   configurable choices is a separate interface change, not part of this
   plan.
5. **SSE parser** — translates Anthropic-shaped `message_start` /
   `content_block_start` / `content_block_delta` / `content_block_stop` /
   `message_delta` / `message_stop` events into pici's existing
   `AssistantMessageEvent` variant (`*StartEvent`/`*DeltaEvent`/`*EndEvent`
   per block type: text/thinking/tool_call), the same way
   `process_sse_line()` does for OpenAI's shape. Needs to track the most
   recent `event:` line to interpret the following `data:` line correctly.
   The state machine must track content-block indices and safely recognize,
   skip, or finalize every declared block type even when a phase does not yet
   expose that block as a pici event. This prevents ignored reasoning/tool
   blocks from corrupting later text indices.
   Within `content_block_delta`, handle all three delta shapes explicitly by
   name: `text_delta` (visible text), `input_json_delta` (tool-call argument
   accumulation), and `signature_delta` (thinking-block signature, when
   present — see the content-block translation section for how rare that
   is). Also expect and no-op on `ping` keep-alive events, which Anthropic-
   style streams send periodically and which don't carry a `data:` payload
   worth acting on.
6. **`map_stop_reason`**: `end_turn`→`stop`, `tool_use`→`tool_use`,
   `max_tokens`→`length`, `refusal`→`error` (with an error message noting the
   refusal, since `StopReason` has no dedicated refusal state).
7. **Usage and error handling** — parse usage from `message_start` and
   `message_delta` into `TokenUsage` (`input_tokens`, `output_tokens`,
   `cache_read_input_tokens`, reasoning-token details, total tokens), then
   call `compute_cost`. Three error shapes must all funnel into
   `result->error_message`:
   a non-2xx response body carrying Muse's Anthropic-style envelope
   (`{"type":"error","error":{"type","message"}}`), a mid-stream `event: error`
   frame, and `HttpClient::post_streaming`'s own synthetic error line
   (`http_client.cpp:228-233` emits an *OpenAI-shaped*
   `{"error":{"message":"HTTP status N..."}}` after a transport failure, not
   the Anthropic envelope) — both shapes expose `error.message`; the Muse
   envelope additionally has a top-level `type` and inner `error.type`. The
   current transport may consume newline-terminated non-2xx bodies before it
   synthesizes its fallback line, so tests must cover complete and truncated
   transport errors. Do not promise HTTP status/header preservation without
   expanding `HttpClient`.
8. **`stream()`** — POST to `{model.base_url}/v1/messages` via
   `HttpClient::post_streaming` (always streaming; no non-streaming fallback
   path needed, unlike OpenAI's local-server compat case).

## Content-block translation (the crux of this integration)

**Building requests (pici → Muse):**
- `TextContent` ↔ `{"type":"text","text":...}` in user/assistant messages.
- `ImageContent` → `{"type":"image","source":{"type":"base64","media_type":mime_type,"data":data}}`.
- `ToolCall` (in assistant history) → `{"type":"tool_use","id","name","input"}`.
- `ThinkingContent{redacted=false, thinking_signature=present}` → replay as
  `{"type":"thinking","thinking":...,"signature":*thinking_signature}` **only
  when `same_model` is true** (already gated by `transform_messages`).
- `ThinkingContent{redacted=false, thinking_signature=absent}` → **expected,
  not an edge case**: `docs/muse/messages_api.md` never documents a
  `signature` field on `thinking` blocks (unlike native Anthropic) — Muse's
  visible `thinking` block is a plain summary with no signature at all; the
  replayable chain-of-thought lives only in `redacted_thinking`'s encrypted
  blob. So most `thinking` blocks parsed from Muse will have no signature.
  `transform_messages.cpp:117-122` already passes these through unchanged
  when `same_model`; on the request-building side, replay the `thinking`
  block as plain `{"type":"thinking","thinking":...}` with no `signature` key
  when absent — don't require a signature to replay a same-model thinking
  block, only to include the field.
- `ThinkingContent{redacted=true}` → replay as
  `{"type":"redacted_thinking","data":*thinking_signature}`. **Design
  decision:** store Muse's `encrypted_content` blob in
  `ThinkingContent::thinking_signature` (leave `.thinking` empty) so the
  existing `redacted` bool + `thinking_signature` optional pair maps directly
  with no new field needed.
  **Observed phase-2 behavior:** a live Messages request rejected a top-level
  `display` field as unknown, but the default response included a
  `redacted_thinking` block with the encrypted replay payload in `data`.
  Treat this observed Messages behavior as the contract for this integration;
  do not copy the Responses API's `include` parameter into Messages requests.
- `ToolResultMessage` → a `tool_result` content block nested inside a
  `user`-role message (`{"role":"user","content":[{"type":"tool_result",
  "tool_use_id","content":...,"is_error"}]}`). Map all supported tool
  result text/image blocks, preserve `is_error`, and define the behavior for
  empty content. **Important:** consecutive
  `ToolResultMessage`s (parallel tool calls) must be **coalesced into one
  `user` message** with multiple `tool_result` blocks — Anthropic's Messages
  convention requires this, unlike OpenAI's separate `tool`-role messages.
  Implement this coalescing pass explicitly in `convert_messages`.

**Parsing responses (Muse → pici):**
- `text` → `TextContent`.
- `thinking` → `ThinkingContent{.thinking=block.thinking, .thinking_signature=block.signature, .redacted=false}`.
- `redacted_thinking` → `ThinkingContent{.thinking="", .thinking_signature=block.data, .redacted=true}`.
- `tool_use` → `ToolCall{.id, .name, .arguments=block.input}`.
- `server_tool_use` (built-in `web_search`/`tool_search`) — out of scope for
  now; no built-in tools are being wired up in this pass.

The parser test seam is part of the implementation: keep the SSE state
machine in a small internal parser class or separate compilation unit with a
testable `feed_line()`/`finish()` surface. Do not make the test depend on a
live HTTP server or on anonymous-namespace functions.

## `ThinkingLevel` → `output_config.effort` policy

Muse always reasons (`thinking: {type:"disabled"}` → 400) and has no
`"minimal"`/`"none"` effort value. The Messages API's default request shape
also streams visible text incrementally, while explicitly sending adaptive
thinking batches the visible answer into a single delta in live testing. Keep
the default `ThinkingLevel::off` request free of a `thinking` field; this does
not disable Muse reasoning, it selects the API default. Express explicit
effort control entirely via
`Model.thinking_level_map` (first real consumer of that field) rather than
hardcoding it in the client:

```cpp
.thinking_level_map = {
  {"off",     std::nullopt}, // omit output_config.effort — model's own default depth
  {"minimal", "low"},        // no Muse "minimal"; map down to lowest available
  {"low",     "low"},
  {"medium",  "medium"},
  {"high",    "high"},
  {"xhigh",   "xhigh"},
}
```

For any non-off level, send `thinking: {"type":"adaptive"}` and set
`output_config.effort` when the map yields a value. For `off`, omit both
fields; Muse still reasons using its default behavior.

## Interface boundaries and unknowns

- `StreamOptions::metadata` is arbitrary JSON, while Muse requires metadata
  values to be strings. The client will accept only a JSON object whose values
  are strings and omit or report invalid metadata deterministically; it will
  not silently stringify nested values.
- `top_p` and configurable `tool_choice` are intentionally deferred. Neither
  exists in the current Agent/AgentLoop/StreamOptions chain, and adding either
  is a separate cross-cutting API change.
- Muse's docs do not publish a context-window or pricing value for this model.
  The current `Model` type has no unknown representation. A temporary model
  entry may use zero as an explicit unknown sentinel for development only; it
  must not be presented as a real context limit or free pricing in a release.
  Resolve this before calling the integration production-ready.

## `src/core/models.cpp`

New comment-headed group in the existing style:

```cpp
// Meta Muse — docs/muse/messages_api.md (Messages API, Anthropic-compatible)
// TODO: verify pricing/context-window before presenting this as a product-ready
// registry entry. Model currently has no explicit unknown representation.
{ .id="muse-spark-1.1", .name="Muse Spark 1.1", .api="muse-messages", .provider="meta",
  .base_url="https://api.meta.ai", .reasoning=true,
  .input_capabilities={"text", "image"},
  .cost={.input_per_mtok=0.0 /*TODO*/, .output_per_mtok=0.0 /*TODO*/,
         .cache_read_per_mtok=0.0 /*TODO*/, .cache_write_per_mtok=0},
  .context_window=0 /*unknown; do not treat as a real limit*/, .max_tokens=8192 /*TODO*/,
  .thinking_level_map={
    {"off", std::nullopt}, {"minimal", "low"}, {"low", "low"},
    {"medium", "medium"}, {"high", "high"}, {"xhigh", "xhigh"},
  } },
```

## `src/core/env_api_keys.cpp`

```cpp
{"meta", try_vars({"MODEL_API_KEY", "META_API_KEY"})},
```
`MODEL_API_KEY` first (the vendor doc's literal name), `META_API_KEY` as a
disambiguating fallback since `MODEL_API_KEY` is unusually generic — mirrors
the two-candidate pattern already used for `"anthropic"`/`"github-copilot"`.

## Registration wiring

`void register_muse_messages_client()` in `muse_messages.cpp`, registering
`"muse-messages"` with `LLMClientRegistry::instance()`. Call it explicitly
from `src/main.cpp:933` and `src/acp/main.cpp:37`, right after the existing
`register_openai_completions_client()` calls — that's the real registration
path; don't bother copying `openai_completions.cpp`'s extra static
self-registering initializer, it looks vestigial.

## Out of scope

- `POST /v1/messages/count_tokens` — no token-counting hook exists anywhere
  in `LLMClient` today; adding one is a separate, unscoped interface change.
- `server_tool_use` built-in tools (`web_search`, `tool_search`).
- Video/document content blocks (pici's `input_capabilities` vocabulary only
  covers `text`/`image` today).

## Tests

New `test/test_muse_messages.cpp`, following `test/test_openai_completions.cpp`'s
hand-rolled `main()`-harness pattern (no gtest), wired into `CMakeLists.txt`
the same way (`add_executable(test-muse_messages ...)`, `add_test(...)`, add
`muse_messages.cpp` to the `pi-http` target's sources). Cover:
- `build_request_json`: basic shape, `system` field placement, `max_tokens`
  fallback, default `thinking` omission for `off`, explicit adaptive thinking,
  and `output_config.effort` present/absent per `ThinkingLevel` (explicitly checking `off`→omitted and
  `minimal`→`"low"`), validated metadata, and that `top_p`,
  `tool_choice`, `stop_sequences`/`top_k`/`container`/`inference_geo` are
  absent.
- Content-block conversion: a `redacted_thinking` block in prior history
  round-trips correctly when `same_model` holds and is dropped (not
  downgraded) when it does not. Add a separate test showing that readable,
  non-redacted thinking can downgrade to text across models.
- Parallel `ToolResultMessage` coalescing into one `user` message with
  multiple `tool_result` blocks.
- SSE parsing: canned `event:`/`data:` sequences through the parser seam,
  including text, `input_json_delta`, `signature_delta`, redacted blocks,
  `ping`, block-index transitions, malformed/unknown events, CRLF input,
  both error envelopes, and message usage. Assert on the resulting
  `AssistantMessage` content and emitted event sequence.
- `map_stop_reason`: all four known values plus an unknown fallback, including
  refusal error text and error-event behavior.

No live-API integration test, consistent with every other provider in the
codebase.

## Phasing (land each phase with `make test` / `ctest --test-dir build --output-on-failure` green)

1. **Skeleton + text output, no thinking/tool events yet.**
   `muse_messages.{h,cpp}` builds requests from text-only history and sends
   the API-default thinking behavior with no effort field. Its parser must nevertheless
   track every block boundary/index and safely skip unsupported reasoning/tool
   blocks while emitting text events. Add usage parsing, the
   `models.cpp`/`env_api_keys.cpp` entries and registration wiring, plus
   `test_muse_messages.cpp` structural/request/parser-seam tests. This is the
   point where a real API key can be used for a text-stream smoke test; it is
   not yet the reasoning-continuity milestone.
2. **Thinking/reasoning blocks**: implement the effort-mapping policy and the
   `thinking`/`redacted_thinking` round-trip using the observed default
   Messages response shape. Add request-side replay, response-side parsing,
   and a live smoke test that verifies a subsequent turn accepts the retained
   encrypted block. Land this phase in its own commit, isolated from tool
   calling.
3. **Tool calling**: `tools`, `tool_use` block parsing, partial JSON
   accumulation, and `ToolResultMessage` → coalesced `tool_result` block
   conversion. `tool_choice` remains omitted because pici has no setting for
   it. Add the complete streaming lifecycle and tests in this phase.
4. **Transport/error hardening**: malformed/unknown SSE handling, both error
   envelopes, CRLF behavior, and any `HttpClient` changes needed to preserve
   response status/body metadata. This is not presentation polish; phases
   that introduce a block type must already provide its complete streaming
   lifecycle.

Use `cmake --build build --target test-muse_messages --parallel &&
./build/test-muse_messages` as the narrow dev loop within a phase, per
`AGENTS.md`.

## Current implementation status

Phases 1–4 are implemented: request construction for text/images, thinking
replay, and tools; required `max_tokens`; adaptive thinking configuration with
effort mapping; validated metadata; usage accounting; text/thinking/redacted
thinking/tool-use SSE parsing; transport/refusal errors; malformed/duplicate
frame handling; model/key registration; and focused offline tests are
complete. Live smoke tests have confirmed two-turn encrypted reasoning replay
and a client-executed `bash` tool loop. Configurable sampling/tool choice
remain intentionally outside the current interface, and built-in server tools
remain out of scope. The Messages contract used here is the observed default
response shape: `redacted_thinking.data`; the unsupported top-level `display`
field is not sent.

## Verification

- `cmake --build build --parallel && ctest --test-dir build --output-on-failure`
  after each phase.
- Manual smoke test once phase 1 lands and a real `MODEL_API_KEY` is
  available: run the CLI against `muse-spark-1.1` for a simple prompt and
  confirm streamed text renders correctly.
- After phase 2, manually verify a multi-turn conversation preserves
  reasoning quality (e.g. a multi-step math/coding prompt) compared to a
  single-turn baseline, since that's the entire point of choosing this API
  surface.
