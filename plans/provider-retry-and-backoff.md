# Retry transient provider errors with backoff

## Context

A live session on `meta/muse-spark-1.2` hit a mid-stream server error:

```json
{"type":"error","error":{"message":"Internal server error."}}
```

`MuseMessagesSseParser::feed_line` (`src/core/providers/muse_messages.cpp:291-304`)
does the right thing with it — records `error_message` verbatim, sets
`stop_reason = StopReason::error` — and the turn simply ends. The user has to
manually re-prompt. `"Internal server error."` does not appear anywhere in
this codebase (`grep -rn "Internal server error" src/` is empty), so it is a
real transient 5xx from Meta's backend, not a client bug. Muse's own protocol
docs classify this as `api_error`, alongside `rate_limit_error` and
`overloaded_error` — all conventionally retryable — versus
`invalid_request_error`/`authentication_error`/`permission_error`/
`not_found_error`, which are not (retrying a bad request just fails the same
way again).

`StreamOptions` already declares the knobs for this:

```cpp
// src/core/llm_client.h:42-43
std::optional<std::uint32_t> max_retries;
std::optional<std::uint32_t> max_retry_delay_ms;
```

They're threaded through `Agent::Options` (`src/core/agent.h:52-53`) into
`AgentLoopConfig` (`agent.cpp:452`) and then into `StreamOptions`
(`agent_loop.cpp:523`) — but **nothing reads them**. None of the three
provider clients (`openai_completions.cpp`, `openai_codex_responses.cpp`,
`muse_messages.cpp`) check `options.max_retries` anywhere
(`grep -rn "options\.max_retry" src/core/providers/` is empty). Worse: there
is no CLI flag or `config.toml` field to set them either
(`grep -n "max_retr" src/cli/args.h src/cli/args.cpp src/cli/config.cpp` is
empty) — so even once implemented, a user has no way to actually configure a
value. This plan finishes both halves: wiring the config surface, and
implementing the retry loop that consumes it.

Every request in this codebase is a streaming POST
(`HttpClient::post_streaming_authenticated`, `src/http/http_client.cpp`); there
is no non-streaming request path to worry about. `curl_easy_perform` blocks
for the whole request and only exposes the HTTP status code and headers
*after* it returns (`http_client.cpp:465-470`), via the same synchronous call
that already delivered zero or more `on_line` callbacks. That ordering
constraint drives the core design decision below.

## Executive design decision: only retry before any content has been shown

A partial answer has already streamed to the terminal/session/UI by the time
many errors occur (see the second `Internal server error.` in the user's
session, which arrived after nine prior `toolUse` turns completed normally
mid-conversation). Retrying *that* request from scratch would either silently
duplicate output the user already saw or require buffering and replaying —
neither is acceptable for a live streaming UX, and no provider here supports
resuming a partially-consumed stream.

So: each provider client tracks whether it has emitted **any** visible
`AssistantMessageEvent` to the caller's `on_event` for *this attempt*
(`AssistantMessageStartEvent` counts as the gate — text/thinking/tool-call
deltas by definition can't occur before it). If an error occurs before that
first visible event, the attempt is retryable (nothing to undo — the caller
has seen nothing yet). If it occurs after, retries stop; today's behavior
(surface the error, keep whatever partial `AssistantMessage` exists) is
unchanged. This mirrors how official streaming SDKs handle the same problem
and needs no protocol support from any provider.

Concretely: each client's single-attempt logic needs a local
`bool emitted_visible_content` flag, set the first time `on_event` is called
with a content-block-start-shaped event, checked before deciding to retry.

## Retry classification per provider

Not every error is retryable. Only classify as retryable:

- **Transport failures with no response at all**: `res != CURLE_OK` from curl
  (DNS failure, connect timeout, connection reset) or `state.callback_error`
  — *except* when `stop_tok.stop_requested()` (a user-requested abort must
  never be retried; keep existing `StopReason::aborted` handling as-is).
- **HTTP status 429, 500, 502, 503, 504** — read from
  `response->status_code` / the `on_response` callback's status, already
  available today.
- **Wire-level error envelopes**, provider-specific:
  - Muse (`muse_messages.cpp`): the parser currently only captures
    `error.message` (`feed_line` line ~295); extend it to also capture
    `error.type` and classify per the documented taxonomy —
    `rate_limit_error`, `overloaded_error`, `api_error` → retryable;
    `invalid_request_error`, `authentication_error`, `permission_error`,
    `not_found_error` → not retryable. `"refusal"` (already special-cased at
    `muse_messages.cpp:345-348`) is not retryable — the model made a
    decision, not a transient failure.
  - OpenAI-compatible (`openai_completions.cpp`): verify the exact error
    envelope shape it already parses (`error.message`/`error.type` around
    lines 665-698) against OpenAI's real error `type`/`code` conventions
    (`rate_limit_exceeded`, `server_error` → retryable;
    `invalid_request_error`, `invalid_api_key`, `context_length_exceeded` →
    not) before wiring classification — do not assume Muse's taxonomy
    applies here.
  - `openai-codex-responses` (`openai_codex_responses.cpp`): same audit;
    this is OAuth-gated, so also confirm an expired/invalid-token response
    is classified as *not* retryable here (that's `AuthResolver`'s job to
    refresh, not this loop's).
- **Never retryable regardless of provider**: anything already mapped to
  `StopReason::tool_use` or `StopReason::stop` (success paths), and
  `"refusal"`-shaped stops.

## Backoff

Exponential with full jitter, capped by `max_retry_delay_ms`:

```
delay = min(max_retry_delay_ms, base_delay_ms * 2^attempt)
sleep_ms = random(0, delay)
```

- `base_delay_ms`: start at 500ms.
- Defaults when `options.max_retries` / `options.max_retry_delay_ms` are
  unset (today's universal case, since nothing sets them yet): 2 retries (3
  attempts total), capped at 30000ms. Zero configured retries must mean "no
  retry loop at all" (today's exact behavior), not "retry with defaults" —
  `max_retries = 0` and `max_retries` unset are different only in that unset
  gets the *default* of 2, not that 0 is reinterpreted.
- If the server sends a `Retry-After` header (surfaced via
  `HttpClient::post_streaming_authenticated`'s `on_response` headers map on
  429/503), prefer it over the computed backoff when present and parseable,
  clamped to `max_retry_delay_ms`.
- The sleep must be interruptible by `stop_token` — a blind `sleep_for` would
  make Ctrl-C/interrupt hang for up to the full backoff window. There is no
  existing stop-token-aware sleep helper in this codebase
  (`grep -rn "wait_for.*stop_tok\|interruptible_sleep" src/` is empty); add
  one, e.g. `wait_or_stop(std::chrono::milliseconds, const std::stop_token&)`
  using `std::condition_variable_any::wait_for` with a token-aware predicate,
  and use it everywhere a retry delay is applied.

## Shared retry driver

Don't duplicate the loop three times. Add one small provider-agnostic driver
— e.g. `src/core/providers/retry.h` (header-only, or a `.cpp` if the jittered
backoff math wants a testable free function) — with roughly:

```cpp
struct RetryPolicy {
  std::uint32_t max_retries;
  std::uint32_t max_retry_delay_ms;
};

RetryPolicy resolve_retry_policy(const StreamOptions &options);

// attempt() returns {retryable, retry_after} alongside doing its normal
// single-attempt work and calling on_event as it goes; the driver only
// decides whether to loop, not how to build/parse a request.
template <typename AttemptFn>
void run_with_retries(const RetryPolicy &policy, std::stop_token stop_tok,
                       AttemptFn attempt);
```

Each provider's `stream()` keeps its existing single-attempt body (renamed to
something like `stream_once`), gains the `emitted_visible_content` gate, and
`stream()` itself becomes a thin call into `run_with_retries`. This keeps
per-provider error-taxonomy differences local to each provider file while
sharing the loop/backoff/interruption mechanics.

## Wiring the config surface

Currently there is no way to set these at all. Add:

- `--max-retries <n>` / `--retry-delay-ms <n>` CLI flags (`src/cli/args.h`,
  `src/cli/args.cpp`), following the existing pattern for `--thinking`.
- `[agent]` fields in `config.toml` (`max_retries`, `max_retry_delay_ms`),
  parsed in `src/cli/config.cpp` alongside `system_prompt`/`thinking`, and
  documented in `config.toml.example`.
- Threaded into `Agent::Options` at construction in `src/main.cpp` and
  `src/acp/main.cpp` (both already construct `Agent::Options` from parsed
  config/args — follow the existing `system_prompt`/`thinking` wiring, not a
  new mechanism).
- These are process-wide agent options, like `system_prompt` — no new RPC or
  ACP per-request surface is needed; do not add one.

## Observability

A multi-second silent pause during backoff looks like a hang. Surface it:

- On `--verbose`, print a line to stderr when a retry is about to happen,
  mirroring the existing `[sandbox: ...]` / `[model: ...]` verbose lines in
  `main.cpp` — e.g. `[retry 1/3 in 812ms: Internal server error.]`.
- Record retry attempts through the existing `StreamDiagnostics` hook
  (`options.diagnostics`, already passed into the SSE parsers — see
  `record_parser_event` in `src/core/stream_diagnostics.h`) so non-verbose
  diagnostic consumers (if any exist today) still see it.
- Do not log the request body/headers on retry — same secret-leakage
  discipline as the rest of the request path.

## Non-goals

- Retrying after any visible content has been emitted to the caller (see
  design decision above — explicitly out of scope, not deferred).
- Retrying client/validation errors (4xx other than 429) — retrying a bad
  request just fails identically.
- Cross-turn or cross-session circuit breaking / rate limiting. This is a
  per-request retry loop only.
- Idempotency guarantees for server-side tool-call side effects. If a
  provider's backend partially executed something server-side before
  erroring pre-first-byte (unlikely but not provably impossible), a retry
  could cause it twice. Document this as a known, accepted risk — do not
  attempt to solve it here (no provider exposes idempotency keys today).
- Non-streaming request paths — none exist in this codebase.

## Test plan

Use the existing faux-client pattern (`core::FauxClient`, see
`test/test_faux_client.cpp` and its use in `test_acp.cpp`) to script
responses deterministically:

- Transient 5xx / `overloaded_error` before any content → retried,
  eventually succeeds; caller sees one clean successful `AssistantMessage`
  with no duplicated events.
- Transient error repeated past `max_retries` → final failure surfaces
  exactly like today's unretried failure (same `StopReason::error`,
  same `error_message` from the last attempt).
- 4xx / `invalid_request_error` → no retry attempted at all, fails
  immediately (assert attempt count == 1).
- Transient error *after* a `AssistantMessageStartEvent`/first text delta has
  already fired → no retry; existing partial-failure behavior preserved
  byte-for-byte.
- `stop_token` cancelled during the backoff sleep → returns promptly
  (bounded by a small test timeout, not the full backoff window), reports
  `StopReason::aborted`, does not attempt a further retry.
- `Retry-After` header present on a 429/503 → honored over computed backoff
  (assert the actual sleep duration used, not just that a retry happened).
- Default behavior with `max_retries`/`max_retry_delay_ms` both unset →
  matches the "defaults when unset" values specified above, not "no retry."
- `max_retries = 0` explicitly → no retry loop at all, identical to today's
  behavior (distinguish from "unset").
- Per-provider error-taxonomy classification test for each of
  `openai-completions`, `openai-codex-responses`, `muse-messages` — confirm
  each provider's own retryable/non-retryable split, since they are not
  identical to each other.

Regression: existing single-attempt-success tests for all three providers
must keep passing unmodified — the retry loop must be a no-op wrapper around
the success path.

## Completion criteria

- `max_retries` / `max_retry_delay_ms` are settable via CLI flag and
  `config.toml`, and actually reach `StreamOptions` (verified by a config
  test, not just code inspection).
- All three provider clients retry only pre-first-byte transient errors,
  classified per their own real error taxonomy, with exponential+jitter
  backoff capped by `max_retry_delay_ms` and interruptible by `stop_token`.
- No behavior change for: successful requests, non-retryable errors, errors
  after visible content has streamed, or explicit `max_retries = 0`.
- Retries are visible in `--verbose` output and via `StreamDiagnostics`, and
  never log request bodies/headers/secrets.
- New/updated tests per the test plan above pass; full `make test` stays
  green.
