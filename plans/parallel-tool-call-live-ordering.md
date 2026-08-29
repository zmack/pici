# Emit ToolExecutionEndEvent as each parallel tool call actually finishes

## Context

A user session using `addons/minimal_dot.lua` showed this for four parallel
`read`/`ls`/`grep` calls:

```
● read  README.md

● ls  test

● ls  docs

● ls  addons

● grep  CMakeLists.txt

● read  Makefile
  ↳ # pi-cpp — C++23 Agent Loop (+221 more)
  ↳ test_acp.cpp (+23 more)
  ↳ antipatterns.md (+3 more)
  ↳ agents.lua (+37 more)
  ↳ CMakeLists.txt-251- (+158 more)
  ↳ SHELL := /usr/bin/env bash (+90 more)
```

All six `●` call lines print, then all six `↳` result lines print as a
second batch. This is not a Lua/addon artifact — `VerboseRenderer` forwards
`ToolExecutionStartEvent`/`ToolExecutionEndEvent` to the Lua
`format_tool_call`/`format_tool_result` hooks 1:1 as they arrive
(`src/core/stream_renderer.cpp:839-844`); there is no reordering in that
layer. (As an unrelated but related fix, `addons/minimal_dot.lua`'s
`format_tool_result` was already updated to repeat `tool_name` + a short arg
summary in every result line, so each line is self-identifying regardless of
how the two batches end up interleaved — see that file's header comment.
That change stays; it is not superseded by this plan, since even genuinely
interleaved output benefits from not having to eyeball-match a `↳` line back
to a `●` line above it.)

### Root cause: `execute_tool_calls_parallel` batches emission by design

`execute_tool_calls_parallel` (`src/core/agent_loop.cpp:754-874`):

1. Loops over all N tool calls on the calling thread and synchronously emits
   *every* `ToolExecutionStartEvent` first (`agent_loop.cpp:769-770`) —
   before any tool has actually started running.
2. Launches each non-immediate call via
   `std::async(std::launch::async, ...)` (`agent_loop.cpp:804-844`) — one OS
   thread per call, no pool, no cap (gated only by
   `is_sequential_tool`/`ToolExecutionMode::sequential`, which routes the
   *entire* batch to `execute_tool_calls_sequential` instead;
   `agent_loop.cpp:890-899`).
3. Blocks on every future via `future.get()` in `slots[]` index order
   (`agent_loop.cpp:847-850`).
4. Only then emits `MessageStartEvent`, `MessageEndEvent`, and
   `ToolExecutionEndEvent` for every call, in one final loop, in **source**
   order (`agent_loop.cpp:862-870`).

The comment at `agent_loop.cpp:838-842` confirms this is deliberate:

> `ToolExecutionEndEvent` is NOT emitted here — it is emitted after all
> futures complete, in source order, so that the renderer sees a consistent
> event sequence matching the sequential path.

This is exactly what's tested by an existing regression test,
`test_parallel_completion_vs_source_order`
(`test/test_agent_loop.cpp:1275-1378`): it releases a `BlockingTool` named
`tool_b` before `tool_a`, so B completes first, and asserts
`end_event_order == ["tool_a", "tool_b"]` — source order, not completion
order. That test's `BlockingTool` harness (release flags +
`end_order_counter`, `test_agent_loop.cpp:1218-1273`) is reused below for the
new test.

Note: **result ordering in the actual conversation transcript was never
wrong** — `result.messages`/`context.messages` are built from `slots[]`,
populated by index regardless of completion order, so tool-result messages
already appear in call order in the model's context. What's being fixed here
is purely the *live event stream* (and therefore the terminal display),
which currently discards real completion timing rather than being
"wrong" about final state.

### Why this is safe to change: the concurrency chain already exists

`emit` (the `StreamCallback` threaded through `execute_tool_calls`/
`execute_tool_calls_parallel`) resolves, at the top of the call chain, to the
`publish` lambda in `run_agent_loop_worker_impl`
(`agent_loop.cpp:918-922`):

```cpp
auto publish = [&](AgentEvent event) {
  std::visit([&](auto &value) { value.sequence = ++sequence; }, event);
  emit(event);       // → Agent::process_event → options_.on_event(event)
  push(std::move(event)); // → EventStream::push_state (mutex-protected)
};
```

Crucially, `publish` is **already called from inside a `std::async` worker
thread today** — `execute_tool_safely`'s progress-update callback
(`agent_loop.cpp:369-374`, wired into `tool->execute(...)`) captures `emit`
by value and calls it for `ToolExecutionUpdateEvent`s from whatever thread
is running that tool. So the thread-safety of everything downstream of
`publish` is already exercised in production, not hypothetical:

- `push()` → `EventStream::push_state` is mutex-protected
  (`src/core/stream.h:267`) — safe for concurrent producers.
- `Agent::process_event` → `state_.append_message`/
  `add_pending_tool_call`/`remove_pending_tool_call` all take
  `AgentState`'s own `mutex_` (`src/core/agent_state.h:89-92,154-163`
  and throughout) — safe.
- `options_.on_event` → the Lua `on_event` hook's `call_on_event` takes its
  own `mutex_` (`src/core/lua_tool.cpp:944-950`) — safe.

The renderer that actually prints to the terminal (`VerboseRenderer`, which
our `format_tool_call`/`format_tool_result` Lua hooks feed) is driven from
`run_turn`'s `session.run_prompt(input, [&vr](event){ dispatch_event(event,
vr); })` (`src/main.cpp:571-573`) — this runs on a single consumer thread
(whatever thread calls `run_prompt`, i.e. the CLI's interactive loop), never
directly on a `std::async` tool-worker thread. Since `EventStream::push()`
is the mutex-protected multi-producer/single-consumer boundary between the
worker threads and this one consumer, moving emission earlier only changes
*when* `push()` is called (from the worker thread, at true completion time)
— the single consumer still dequeues and calls `dispatch_event` one at a
time, now in real arrival order instead of a deferred batch. No new
producer/consumer hazard is introduced.

### The one real gap: `sequence` is not thread-safe today

`publish`'s `std::visit([&](auto &value) { value.sequence = ++sequence; },
event)` increments a plain, non-atomic `std::uint64_t sequence` captured by
reference (`agent_loop.cpp:917,919`) with **no lock**. This is already a
live data race whenever a parallel-executed tool reports progress via
`ToolUpdateCallback` (the `ctx.update(value)` mechanism documented in
`addons/README.md`'s Phase 1 section) — `publish` gets called concurrently
from multiple worker threads today, and `++sequence` is not synchronized
across them. Once `ToolExecutionEndEvent` is also emitted from inside the
worker lambda (this plan's actual change), *every* parallel batch will hit
this race, not just tools that happen to report progress — so fixing it is
a hard prerequisite, not an optional cleanup.

Checked whether anything depends on `AgentEvent.sequence` reflecting
call/source order specifically (`grep -rn "\.sequence\b" src/`): only
`event_json.cpp:98` reads it, and only to pass it through into a JSON
payload (`{"sequence", base.sequence}`) — a straight serialization, not a
sort key or ordering assumption. (`src/acp/task_events.cpp:63,83` has its
own, unrelated `sequence`/`generation_` counter for child-task event replay
— a different field on a different struct, not `AgentEvent.sequence`.) So
making `sequence` correctly monotonic under real concurrency is a strict
correctness improvement with no other consumer to break.

## Executive design decision

Two changes, in order:

1. **Make `sequence` assignment thread-safe** (prerequisite).
2. **Emit `ToolExecutionEndEvent` from inside each async worker lambda**,
   right where it's computed, instead of batching it into the post-loop.

Everything else in the parallel path — `MessageStartEvent`,
`MessageEndEvent`, and `result.messages` construction — stays exactly where
it is today, in the post-loop, in deterministic source order. That loop is
what feeds `Agent::process_event`'s `state_.append_message(ev.message)`
(`src/core/agent.cpp:500-501`), which is how `AgentContext.messages` (the
actual conversation transcript) gets built. Reordering *that* would make
tool-result message ordering in the live context nondeterministic run to
run, which is out of scope and not something this bug report is about — the
transcript's final ordering was never wrong (see Context above). Only the
observability signal (`ToolExecutionEndEvent`, consumed by the renderer and
by `state_.remove_pending_tool_call`, `agent.cpp:504-505`, neither of which
has a cross-call ordering dependency) needs to move.

## Fix 1: thread-safe `sequence`

In `run_agent_loop_worker_impl` (`agent_loop.cpp:912-922`), change:

```cpp
std::uint64_t sequence = 0;
auto publish = [&](AgentEvent event) {
  std::visit([&](auto &value) { value.sequence = ++sequence; }, event);
  ...
```

to an atomic counter:

```cpp
std::atomic<std::uint64_t> sequence{0};
auto publish = [&](AgentEvent event) {
  std::visit([&](auto &value) { value.sequence = sequence.fetch_add(1) + 1; },
             event);
  ...
```

(Keep the existing "starts at 1" semantics — `fetch_add(1) + 1` matches
today's pre-increment `++sequence` starting from 0.) `publish_failure`
(`agent_loop.cpp:1135-1153`) uses its own separate local `sequence` and is
single-threaded (only called from the one worker thread on the failure
path) — no change needed there.

## Fix 2: emit `ToolExecutionEndEvent` at true completion time

In the async lambda inside `execute_tool_calls_parallel`
(`agent_loop.cpp:811-844`), replace the "NOT emitted here" comment block
with an actual emission, using the `finalized` value already computed right
there (`agent_loop.cpp:823-826`):

```cpp
auto finalized = finalize_tool_call(
    context, assistant_message, call.tool_call,
    std::move(execution.result), execution.is_error, config,
    call.args_json, stop_tok, execution.status);
#ifdef PI_CPP_OTEL_ENABLED
...
#endif
emit(ToolExecutionEndEvent(finalized.tool_call.id, finalized.tool_call.name,
                           finalized.result, finalized.is_error,
                           finalized.status, std::source_location::current()));
return std::make_pair(idx, std::move(finalized));
```

Then remove the now-duplicate `ToolExecutionEndEvent` emission from the
post-loop (`agent_loop.cpp:866-868`), leaving `MessageStartEvent`/
`MessageEndEvent`/`result.messages.push_back` untouched:

```cpp
for (const auto &finalized : finalized_calls) {
  auto msg = make_tool_result_message(finalized.tool_call, finalized.result);
  emit(MessageStartEvent(msg, std::source_location::current()));
  emit(MessageEndEvent(msg, std::source_location::current()));
  result.messages.push_back(std::move(msg));
}
```

`tool_span->End()` (the OTel span) already happens inside the worker lambda
today (`agent_loop.cpp:836`) — unaffected by this change, no OTel-side
adjustment needed.

## Downstream effect (no addon changes required)

Once this lands, `VerboseRenderer`'s `on_tool_end` — and therefore every
`format_tool_result` Lua hook, including all twelve addons built this
session — receives events in real completion order. The four-call example
above would show `↳ ls docs` before `↳ read README.md` if `docs` finished
first, interleaved with whichever `●` start lines print after it, instead
of two separate batches. This requires no changes to any addon: they all
already consume `ToolExecutionEndEvent`/`ctx` the same way regardless of
when it arrives.

This is also the prerequisite for a possible follow-up (not this plan): a
live-updating status buffer (e.g. extending `ViewportRenderer`, which
already redraws via cursor-position escapes) that shows real per-call
progress instead of printed lines. Without this fix, such a buffer would
have no real completion-order data to display and would just be a fancier
rendering of the same simultaneous-batch information. See Non-goals.

## Non-goals

- **Reordering `MessageStartEvent`/`MessageEndEvent`.** These drive
  `state_.append_message` and must stay deterministic/source-ordered — see
  Executive design decision above. Explicitly out of scope, not deferred.
- **`execute_tool_calls_sequential`.** Already interleaves naturally (each
  iteration does start → execute → end before moving to the next call,
  `agent_loop.cpp:679-749`) — no change needed.
- **A virtual/alt-screen live tool-status buffer** (extending
  `ViewportRenderer` or similar). That's a separate, larger renderer-layer
  feature that only becomes worth building once this event-timing fix is in
  place; not part of this plan.
- **`should_terminate_tool_batch`'s wait-for-all-results semantics**
  (`agent_loop.cpp:872`). Turn termination inherently needs every result in
  the batch regardless of display timing — unaffected and unchanged.
- **A concurrency cap/scheduler for parallel tool calls.** Every
  non-sequential call in a batch still gets its own unbounded `std::async`
  task, exactly as today. Not addressed here.
- **`publish_failure`'s separate `sequence` variable**
  (`agent_loop.cpp:1135-1153`) — single-threaded, not part of this race.

## Test plan

- **Update `test_parallel_completion_vs_source_order`**
  (`test/test_agent_loop.cpp:1275-1378`): its `BlockingTool` harness
  (`release_a`/`release_b` flags, `end_order_counter`,
  `test_agent_loop.cpp:1218-1273`) is exactly the right tool for the new
  assertion. Release `tool_b` first (as today), but now assert
  `end_event_order == ["tool_b", "tool_a"]` (real completion order) while
  `message_start_order` and `turn_tool_results` stay `["tool_a", "tool_b"]`
  (source order, unchanged — still built in the post-loop). Rename the test
  to describe the new behavior, e.g. "Parallel: ToolExecutionEndEvent
  follows completion order; MessageStart/results stay source order."
- **New test**: with the same `BlockingTool` harness, assert every emitted
  `AgentEvent.sequence` across the whole run is unique and strictly
  increasing in actual emission order (not call order) — verifies Fix 1
  didn't just avoid UB but produces meaningful values once end events can
  arrive out of source order.
- **Stress test under `make tsan`** (target already exists:
  `Makefile:73`, builds `build-tsan` with
  `-fsanitize=thread -fno-omit-frame-pointer`): N≥8 concurrent
  `BlockingTool`-style calls released in reverse order. Confirms the
  `sequence` race is real and TSan-catchable before Fix 1, and clean after.
  `make check` (`Makefile:82`, the CI/pre-push gate) already runs
  `test lint tsan`, so this is covered by existing tooling, not a new CI
  surface.
- **Regression**: `test_parallel_mixed_immediate_source_order`
  (`test_agent_loop.cpp:1382-1450`) only asserts `turn_tool_results`
  ordering, which this plan does not touch — must keep passing unmodified.
  This is a useful confirmation that the fix's blast radius is correctly
  isolated to `ToolExecutionEndEvent` only.
- **Regression**: `test_per_tool_sequential_override` and all
  `execute_tool_calls_sequential`-path tests must keep passing unmodified —
  that function is not touched.
- Full `make test` and `make check` stay green.

## Completion criteria

- `ToolExecutionEndEvent` for a parallel tool call is emitted at that call's
  actual completion time (from within its `std::async` worker), not batched
  with the rest of the group after every future resolves.
- `MessageStartEvent`/`MessageEndEvent`/`result.messages` ordering is
  byte-for-byte unchanged (still deterministic source order) — no change to
  `AgentContext.messages` construction or session-transcript ordering.
- `event.sequence` assignment is race-free under concurrent `publish` calls
  (verified by the `make tsan` stress test), and remains a meaningful,
  strictly-increasing value reflecting true emission order.
- No addon/Lua-side changes required; existing `format_tool_call`/
  `format_tool_result` hooks (all twelve built this session) automatically
  benefit — verified live by rerunning the parallel-call smoke test used
  when this bug was first reported and confirming interleaved `●`/`↳`
  output instead of two batches.
- New/updated tests per the test plan pass; `make test` and `make check`
  (which includes `tsan`) stay green.
