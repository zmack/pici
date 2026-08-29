# Subagent pane: a mounted, self-driving panel (region-only)

## Status: redesigned twice, this is the settled version

This file has gone through three shapes. Keeping the history briefly, since
each revision corrected a real mistake in the one before it, and future
readers should know why the design looks like this rather than the simpler
things that were tried first:

1. **Draft 1** proposed fixing `VerboseRenderer`'s forwarding gap (root
   cause #1) plus an optional `activity_watcher` thread gated per-renderer,
   with a from-scratch mutex audit of `ViewportRenderer` as a prerequisite.
   Discarded: it patched the symptom (a broken forward) without addressing
   why there was no dedicated delivery mechanism in the first place.
2. **Draft 2** introduced `SubagentPanel` as a component a renderer
   *mounts* — but gave it its own `std::jthread`, ticking on a fixed
   16ms/200ms interval and pushing via `set_subagent_pane()`. This was
   corrected in conversation: the renderer already owns a redraw loop
   (`paint_loop`); a second independent clock next to it is redundant, and
   the two ended up needing to agree on cadence for no reason.
3. **This version** replaces the timer with an event-driven push (wake
   exactly when the data actually changes, never on a schedule), and — the
   part that took real digging to get right — discovered and closed a
   genuine cross-thread terminal-write race that draft 2's design would
   have introduced at idle. See §3 for why that race is real and how it's
   closed; it is the least obvious part of this plan and the part most
   worth reading carefully before implementing.

Grounded in a direct read of the tree as of 2026-08-23, tip commit
`3058b8a`, plus the uncommitted `/memory`-table fix already applied this
session to `src/core/region_renderer.cpp`/`src/core/stream_renderer.cpp`
(the `fence_command_output()` helper — unrelated, doesn't conflict).

## The architectural principle this follows

**The renderer owns rendering and redraw cadence. The component owns
composing data.** Concretely: `SubagentActivityBridge` already composes
"what to show" (`pane_rows()`) cheaply or immediately, and knows nothing
about terminals. `RegionRenderer` already owns *when the screen actually
updates* — during a turn, that's `paint_loop`'s existing 16ms tick pulling
from `state_`; between turns, it's a synchronous repaint triggered
explicitly. Every other state setter on `RegionRenderer`
(`on_text_delta`, `on_tool_start`, `set_status_line`, …) already follows
exactly this shape: mutate state, mark dirty, let the *renderer's own*
existing mechanism decide when that becomes visible. This plan makes
`set_subagent_pane()` behave the same way, and removes the version of
itself (draft 2) that didn't.

This is the same pattern real compositors use — a client updates its
buffer whenever it wants; the compositor composites whatever's current at
its own vsync tick, and the client never drives that timing directly. The
"idle" case (§3) is the one place a terminal app doesn't have a running
compositor tick to lean on, and needs its own small answer.

## §1: What was actually broken, and what wasn't

The mid-turn push runs through `VerboseRenderer`, a thin per-turn adapter
that forwards `owns_status_line()`/`set_status_line()` correctly but has no
equivalent override for the pane capability — it silently inherits the base
class's `false`/no-op. So mid-turn, `SubagentActivityBridge::drain(vr)`
always concludes the renderer can't show a pane and falls back to its
ambient-line path, which for region appends straight into the *current
turn's own transcript block* — a subagent's activity reads as if the root
said it. **This is genuinely broken and is the entire user-visible
complaint.**

The **idle** path (`main.cpp:2424`, `:2430` — current line numbers,
`activity->drain(*renderer)`) was, and remains, correct: it's called with
the raw renderer, not `vr`, so it already reaches `set_subagent_pane()`
properly. Nothing in this plan needs to change it, and an earlier draft of
this file was wrong to propose suppressing it for region — that would have
*broken* the one path that already worked. Left alone below.

## §2: Fix — event-driven push, mid-turn

Give `SubagentActivityBridge` a second, independent wake slot — separate
from the existing `wake_` (still wired to `mailbox_wake->notify()` at
construction, `main.cpp:1678-1679`, untouched):

```cpp
// src/core/subagent_activity.h — add alongside the existing WakeCallback
void set_pane_wake(WakeCallback cb); // may be empty to clear
```

```cpp
// src/core/subagent_activity.cpp — enqueue() fires both, independently
void SubagentActivityBridge::enqueue(AgentTaskId id, std::string line) {
  WakeCallback wake, pane_wake;
  {
    std::scoped_lock lock(mutex_);
    ...
    wake = wake_;
    pane_wake = pane_wake_;
  }
  if (wake) wake();
  if (pane_wake) pane_wake();
}
```

A second named slot rather than a general subscriber list, because the
cardinality is exactly one: at most one panel mounts against at most one
bridge in this design. No id/handle/removal machinery needed —
`unmount()` just calls `set_pane_wake({})`.

`SubagentPanel` (`src/core/subagent_panel.h`/`.cpp`, new) is now genuinely
thin — **no thread, no timer, no member state beyond the bridge
reference**:

```cpp
class SubagentPanel {
public:
  explicit SubagentPanel(SubagentActivityBridge &bridge) : bridge_(bridge) {}

  void mount(Renderer &renderer) {
    if (!renderer.owns_subagent_pane())
      return;
    bridge_.set_pane_wake(
        [this, &renderer] { renderer.set_subagent_pane(bridge_.pane_rows()); });
  }

  void unmount() { bridge_.set_pane_wake({}); }
  ~SubagentPanel() { unmount(); }

private:
  SubagentActivityBridge &bridge_;
};
```

`enqueue()` calls `pane_wake()` synchronously, on whatever thread produced
the event — per earlier analysis, that's a **child agent's own worker
thread** for tool-start/tool-end/message events, or whatever thread called
`spawn()` for the spawn event. So `renderer.set_subagent_pane(rows)` above
executes on an arbitrary background thread, synchronously, every time. This
needs to be safe — see §3 for why it already is, once one gap is closed.

**Why this needs no polling and no dedup vector-comparison** (draft 2 had
both): every call here is triggered by a *real* bridge event, and
`pane_rows()`'s `last_activity` field changes on essentially every event
(each one appends a new activity line), so consecutive real pushes almost
never carry identical data — there's no "spinning on nothing" case to guard
against, because nothing calls this except in response to something
actually happening.

## §3: The idle hazard, and why it's smaller than it looks

`RegionRenderer::set_subagent_pane()` today (`region_renderer.cpp`,
current lines — re-check at implementation time given the `/memory`-fix
diff already in the tree):

```cpp
void set_subagent_pane(const std::vector<SubagentPaneRow> &rows) override {
  bool paint_now = false;
  {
    std::scoped_lock lock(mutex_);
    state_.subagent_rows = rows;
    state_.pane_rows_reserved = rows.empty() ? 0 : kSubagentPaneRows;
    state_.revision = ++revision_;
    mark_dirty_locked();
    paint_now = !turn_active_;
  }
  if (paint_now)
    paint_idle_synchronously();
}
```

**Mid-turn** (`turn_active_ == true`): `paint_now` is always `false`, so
this method never writes to the terminal — it only mutates `state_` under
`mutex_` and marks dirty. `paint_loop()` (region's own dedicated background
thread, already the sole writer during a turn) picks the change up on its
next ~16ms tick. **This is already fully safe for an arbitrary calling
thread, today, with zero changes** — it was already proven safe in the
superseded draft's analysis, and remains the load-bearing fact that makes
§2 cheap.

**Idle** (`turn_active_ == false`): `paint_now` is `true`, and this method
calls `paint_idle_synchronously()` **directly, inline, on whatever thread
called `set_subagent_pane()`**. Today that's always the main thread — the
only two callers are `main.cpp`'s idle-loop `drain()` calls, both running
after `readline()` has already returned control to the main thread
(`readline()` itself is not concurrently executing at that point — the two
never overlap because they're sequential on one thread). **§2 breaks that
invariant**: `SubagentPanel`'s wake fires from arbitrary child-agent
threads *at any time*, including while the main thread is blocked inside
`readline()` actively repainting the composer in response to a keystroke.
Two threads calling `write_best_effort()` (`terminal.h:28-36`) on the same
fd concurrently is a real corruption risk, not a hypothetical one —
confirmed by reading the primitive itself: it loops over `::write()` and
explicitly anticipates partial writes, so a full frame is not one atomic
syscall and cannot rely on `PIPE_BUF` atomicity.

**The fix is one guard, not a new subsystem.** `RegionRenderer` captures
its own construction thread id once (`main.cpp:2124`,
`auto renderer = make_renderer(args);` — always called from the main
thread):

```cpp
// RegionRenderer member, set in the constructor (region_renderer.cpp:682)
const std::thread::id main_thread_id_ = std::this_thread::get_id();
```

and the idle branch becomes:

```cpp
if (paint_now && std::this_thread::get_id() == main_thread_id_)
  paint_idle_synchronously();
```

A background-thread call while idle now safely no-ops the *write* — it
still marks `state_` dirty (that part already happened above, unconditionally)
— and relies on the **pre-existing, unmodified idle mechanism**
(`mailbox_wake` → `readline()` exits → `main.cpp:2424`/`:2430`'s
`activity->drain(*renderer)` → this same setter, called again, this time
correctly on the main thread) to perform the actual paint moments later. It
already fires for this exact case — `SubagentActivityBridge`'s original
`wake_` (unrelated to the new `pane_wake_`) has been calling
`mailbox_wake->notify()` on every `enqueue()` since before this plan
existed. The two wakes fire from the same `enqueue()` call, independently;
the panel's is now just redundant-but-harmless at idle, and load-bearing
only mid-turn.

**This also means the coalescing question resolves itself, not via a new
scheduler.** A burst of rapid subagent events at idle no longer causes a
burst of paints from multiple threads (no thread but the main one ever
writes), and on the main-thread side, the existing self-pipe wake
(`cli::ReadlineWake`) already coalesces multiple `notify()` calls into a
single pending wake by construction — whatever `pane_rows()` returns *when
the main thread eventually gets there* already reflects every event that
fired in the meantime. No two-flag "in-flight / pending" state machine is
needed anywhere in this design; it would have been solving a problem that
the corrected design doesn't have. (The general pattern — coalesce bursts
into the read of *current* state rather than queuing per-event work — is
still the right one to reach for elsewhere; it's just already present here,
in the self-pipe, rather than needing to be built.)

## §4: `main.cpp` wiring

Construct the bridge exactly as today (`main.cpp:1678-1679`) — unchanged.
After the renderer is constructed (`main.cpp:2124`):

```cpp
core::SubagentPanel subagent_panel(*activity);
subagent_panel.mount(*renderer); // no-ops internally if !owns_subagent_pane()
```

Declare `subagent_panel` immediately after `renderer`, both well after
`activity` — so on scope exit (end of `cmd_run`), `subagent_panel` is
destroyed first (LIFO), unmounting before `renderer` or `activity` are
touched. Same destruction-order discipline as prior drafts; still the one
placement rule worth a direct review check rather than trusting the
compiler to catch it.

**Only the mid-turn drain call sites need suppressing for region** — this
is narrower than the previous draft claimed:

- `run_and_persist`'s activity argument (`main.cpp:2277`):
  `(isatty(STDIN_FILENO) != 0 && !args.print_mode) ? activity : nullptr` →
  add `&& !panel_mounted` (compute `panel_mounted = renderer->owns_subagent_pane();`
  once, alongside the `mount()` call above).
- `run_idle_mailbox_turns`'s equivalent (`main.cpp:2305-2306`): same
  addition.

Both guard the *mid-turn* `activity->drain(vr)` call inside
`run_turn_impl` (`main.cpp:848`) — the one that's still routed through the
unfixed `VerboseRenderer` and would otherwise double-deliver (once
correctly via the panel, once incorrectly into the transcript via the old
ambient-line fallback).

**Leave `main.cpp:2424` and `:2430` (`activity->drain(*renderer)`)
completely alone, for every renderer, unconditionally.** They are not part
of the bug and are now, after §3, load-bearing for the idle case again —
suppressing them would silently break idle updates for region, which is
exactly the mistake the previous draft of this file made.

## §5: Scope decision — region-only, enforced not assumed

Unchanged from the previous draft. `ViewportRenderer` currently also
advertises pane support (shipped in `e3991ce`). Per explicit direction,
revert its `owns_subagent_pane()`/`set_subagent_pane()` overrides to the
base class defaults, so the capability flag `main.cpp` gates `mount()` on
means exactly what it says, rather than gating on a side-channel like a
render-mode string comparison.

**Cleanup that goes with the revert:**
- `ViewportRenderer::owns_subagent_pane()`/`set_subagent_pane()`: delete
  the overrides (fall through to base).
- `subagent_rows_` member, `pane_rows()`, `paint_subagent_pane()`: delete.
- `set_scroll_region()`/`repaint()`'s `- pane_rows()` terms: revert to the
  pre-`e3991ce` fixed `h - 2`.
- `on_resize()`'s and `on_turn_start()`'s `paint_subagent_pane()` calls:
  delete.
- `test/test_region_renderer.cpp`'s
  `test_subagent_pane_is_rendered_in_viewport_mode` (added in `e3991ce`):
  delete.

**Consequence**: viewport falls back to the same ambient-line behavior
`RawStreamRenderer`/`DiffMarkdownRenderer` already have — not a new
limitation, its pre-`e3991ce` shape. Flagged as a real, if small, reversal
of already-shipped work — confirmed acceptable per direct instruction.

## Test plan

`SubagentPanel` is a normal `pi-core` class — no anonymous-namespace
imprisonment problem, fully unit-testable, unlike `VerboseRenderer`. And
because delivery is now synchronous and event-driven rather than
timer-driven, tests don't need to sleep-and-poll at all: the whole chain
from `bridge.observe(event)` down to a painted frame happens on the calling
thread, inline, by construction.

Add `test/test_subagent_panel.cpp`:

1. **Mid-turn decoupling, the core claim.** Mount against a real
   `RegionRenderer` (`make_region_renderer(pipe_fd)`), call
   `renderer->on_turn_start()` to simulate an active turn (no further
   `dispatch_event` calls after this — that's the point), then
   `bridge.observe(AgentTaskSpawnedEvent{...})` directly. Assert the pane
   content is present after the paint loop's next tick — a short bounded
   wait (~50ms, one frame interval) is still needed here specifically
   because *this* path legitimately defers to `paint_loop`'s own cadence by
   design; that's different from claiming the push itself is synchronous.
2. **Idle safety — the regression this plan exists to prevent
   reintroducing.** Mount against a real `RegionRenderer`, do **not** call
   `on_turn_start()` (renderer stays idle), observe an event from a
   background thread (spin up a `std::thread` in the test specifically to
   exercise the non-main-thread path), and assert **no output arrives**
   from that call alone — proving the `main_thread_id_` guard actually
   no-ops the write rather than racing. This test would have failed against
   draft 2's design and must keep failing if the guard is ever removed.
3. **Idle liveness still works end-to-end.** Same idle setup, but this
   time also drive the *existing* mechanism explicitly (call
   `bridge.drain(*renderer)` from the main thread, as `main.cpp`'s idle
   loop already does) after the background-thread `observe()` — assert
   output *does* appear this time, proving the two mechanisms compose
   correctly rather than the guard silently eating updates forever.
4. **`owns_subagent_pane() == false` is a true no-op**: mount against
   `make_viewport_renderer(...)` (post the §5 revert, so `false`) and
   assert `bridge_.set_pane_wake()` was never actually invoked with a live
   callback — `mount()` should return before touching the bridge at all.
5. **Clean unmount**: mount, observe an event, `unmount()`, observe more —
   assert nothing further arrives.

## Phases

1. **`SubagentActivityBridge::set_pane_wake()` + `SubagentPanel` + its
   tests.** Self-contained, no `main.cpp` or `RegionRenderer` changes yet.
2. **`RegionRenderer`'s `main_thread_id_` guard.** One member, one
   conditional. Small and independently reviewable — this is the line the
   whole plan's safety argument rests on, worth its own focused look rather
   than folding into a larger diff.
3. **Viewport scope-decision revert + cleanup** (§5). Mechanical,
   independent of phases 1-2.
4. **`main.cpp` wiring** (§4): construct + mount; guard *only* the two
   mid-turn drain call sites; leave the idle ones untouched. Manual
   verification: `pi-cli --render region`, spawn a subagent, confirm the
   pane updates continuously mid-turn (the scenario nothing before this
   plan could do), *and* confirm it still updates correctly when a
   subagent is spawned and worked entirely while the root sits idle at the
   prompt (the scenario that already worked and must keep working).

## Risks

1. **Destruction order.** `subagent_panel` must be declared/destroyed
   before `renderer`/`activity`. Same as prior drafts; still a
   review-time check, not something tests reliably catch.
2. **A missed mid-turn suppression guard.** Both `:2277` and `:2305-2306`
   need the `&& !panel_mounted` addition, or region gets the correct
   panel-driven pane *and* the old misrouted-into-transcript line
   simultaneously for whichever call site was missed.
3. **Accidentally also guarding the idle call sites.** The opposite
   mistake — this is what the previous draft of this file got wrong. If
   `:2424`/`:2430` are suppressed "for consistency" with the mid-turn
   guards, idle updates silently stop working for region. They must stay
   unconditional.
4. **The `main_thread_id_` guard is the crux of the whole plan's safety
   argument** — if it's ever removed (e.g. by someone "simplifying" what
   looks like a redundant check), the cross-thread write race in §3 comes
   back. Test 2 above exists specifically to catch that.
5. **Reverting already-shipped viewport behavior.** Real, user-visible,
   confirmed acceptable per direct instruction — see §5.

## Critical files for implementation

- `src/core/subagent_activity.h`/`.cpp` (`set_pane_wake()`)
- `src/core/subagent_panel.h`/`.cpp` (new)
- `src/core/region_renderer.cpp` (`main_thread_id_` guard — the one
  behavioral change to this file)
- `src/core/stream_renderer.h` (no change in this version — the
  `SubagentPaneRow::operator==` addition from draft 2 is no longer needed,
  since there's no dedup comparison left to make)
- `src/core/stream_renderer.cpp` (`ViewportRenderer` — delete pane support)
- `src/main.cpp` (construct + mount `subagent_panel`; guard the two
  mid-turn drain call sites only)
- `test/test_subagent_panel.cpp` (new)
- `test/test_region_renderer.cpp` (delete the viewport pane test)
- `CMakeLists.txt` (new test target + `subagent_panel.cpp` added to
  `pi-core`'s source list)
