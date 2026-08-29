# Live subagent progress display — persistent reserved pane

## Status: implemented (superseded)

Everything below shipped in commits `03fad0e` (data layer:
`SubagentActivityBridge`, `main.cpp` wiring, `AgentTaskManager` fan-out) and
`e3991ce` (`Add live subagent progress pane and clean lint`: the
`Renderer::owns_subagent_pane()`/`set_subagent_pane()` pair, region- and
viewport-mode reserved bands, `pane_rows()`, and the tests in
`test/test_subagent_activity.cpp` and `test/test_region_renderer.cpp`). This
document is kept for historical/design-rationale reference only — do not use
its line numbers or "uncommitted"/"critical finding" framing as current
state.

The remaining real gap — the pane doesn't actually update *live* while a
subagent is working mid-turn, only when the root agent's own turn happens to
emit an event — is scoped in
[`plans/subagent-pane-live-midturn-updates.md`](subagent-pane-live-midturn-updates.md).

---

Implementation plan for the **chosen direction**: a persistent reserved pane
— a fixed region at the bottom of the terminal that always shows a
live-updating one-line-per-agent status table (id/name, status, last
activity) — across all of pici's render modes, without requiring any
keypress. This replaces the "ambient inline lines interleaved into the
transcript" direction explored in the previous draft of this plan (now
superseded, but its data-layer half turns out to already be implemented —
see "Critical finding" below).

Grounded in a direct re-read of the current tree as of 2026-08-23:
`src/core/agent_task.h`/`.cpp`, `src/core/stream_renderer.h`/`.cpp`,
`src/core/region_renderer.h`/`.cpp`, `src/core/terminal.h`/`.cpp`,
`src/core/subagent_activity.h`/`.cpp`, `src/main.cpp`, `src/cli/config.cpp`,
`src/acp/server.cpp`, `src/acp/task_events.cpp`, `test/test_agent_tasks.cpp`,
`test/test_mailbox_coordinator.cpp`.

## Critical finding: the data layer already exists, uncommitted

`git status` shows `src/core/subagent_activity.h` and
`src/core/subagent_activity.cpp` as **untracked new files**, and
`src/main.cpp` as **modified**, with no corresponding commit (`git log`
shows the tip commit is `b4a7617 Patch tool`, unrelated). Someone has
already built and wired up `core::SubagentActivityBridge` — a
mutex-guarded, worker-thread-safe aggregator of `AgentTaskEvent`s into
per-agent name/status/last-N-lines state — as the direct realization of the
*previous* draft's §1 "ambient inline lines" idea:

- `src/core/subagent_activity.h:18-45` — class shape: `observe(AgentTaskEvent)`,
  `drain(Renderer&)`, `recent(id)`, `summary()`.
- `src/core/subagent_activity.cpp:57-120` — `observe()` turns
  spawn/status-changed/closed/tool-start/tool-end/message-end events into
  short formatted lines, keyed and buffered per `task_id` (`kMaxActivity =
  50`, `subagent_activity.cpp:36`).
- `src/core/subagent_activity.cpp:25-36` — `status_word()` already
  implements the "no false done" vocabulary this plan requires:
  `completed → "idle"`, `errored → "errored"`, `interrupted →
  "interrupted"` — reuse directly, do not reinvent.
- `src/main.cpp:1631-1638` — constructed and spliced into `task_callbacks`
  alongside `mailbox_observer`, ahead of `core::fan_out_agent_task_callbacks`
  (`main.cpp:1647`).
- `src/main.cpp:845-848` — drained on the main thread from inside the root
  turn's own dispatch callback (`activity->drain(vr)` after every
  `dispatch_event`) — this already solves the "drain while the root turn is
  in flight" problem the previous draft flagged as missing.
- `src/main.cpp:2278-2286` — `activity->summary()` folded into the
  aggregate status line via `update_terminal_ui()`.
- `src/main.cpp:2229`, `2257` — gated by `(isatty(STDIN_FILENO) != 0 &&
  !args.print_mode) ? activity : nullptr` when passed into
  `run_turn`/`run_message_turn` (the non-interactive/`-p` exclusion this
  plan needs already has a working precedent here).
- `src/main.cpp:2376`, `2382` — drained again at the idle prompt
  (mailbox-wake path and the normal post-`readline()` path).

**This plan builds the reserved pane on top of `SubagentActivityBridge` as
the data/aggregation layer — it does not need a new event-consumption or
aggregation mechanism.** The work is specifically: (a) route the render
side away from `on_command_output` and into a new dedicated reserved-pane
paint path for the modes that can support it, (b) leave the existing
`on_command_output`-based ambient lines as the fallback for the modes that
structurally cannot.

**Known bugs in the current uncommitted wiring**, verified directly, that
motivate *not* keeping `on_command_output` as the universal sink:

- `RegionRenderer::on_command_output` (`src/core/region_renderer.cpp:1050-1081`)
  appends the text into the **active turn's own `RegionTextBlock`**
  (`turn.blocks.back()`, line 1059-1063) — a subagent's ambient line is
  visually indistinguishable from the root assistant's own `WORK`
  narration. Confirmed by reading the method body directly.
- `ViewportRenderer::on_command_output` (`src/core/stream_renderer.cpp:454-461`)
  resets `scanner_`, `fin_cache_`, and unconditionally sets
  `scroll_offset_rows_ = 0` (`stream_renderer.cpp:459`) on every call — an
  ambient subagent line yanks the user's scroll position and invalidates
  the finalized-content cache mid-turn.

Both are real, reproducible defects in the currently-uncommitted code, not
hypothetical.

**Corrected from independent review**: these two bugs are *not* the reason
region/viewport get a dedicated pane instead of extending
`on_command_output` — that reason is structural (§3/§4's pane lives outside
`RegionState.turns` entirely; extending `on_command_output` further was
never actually on the table once a fixed band was chosen). The bugs are
independent, pre-existing defects on the paths that *still* call
`on_command_output` in those two modes today (e.g. `/tools`, `/model`,
`/memory` output) — worth fixing on their own merits (flagged as a Phase 0
item below), but citing them as motivation for this plan overstates the
connection. Note also viewport's `scroll_offset_rows_ = 0` reset may be
intentional there (jump-to-bottom on new output) rather than an outright
bug — confirm intent before "fixing" it.

## Goal

Every running/idle-but-alive subagent is visible at a glance, continuously,
with zero user action, via a fixed-position table pinned to N bottom rows
of the terminal — distinct from (not interleaved into) the main scrolling
transcript — in every render mode that can support cursor-addressed
layout, with a safe, already-proven fallback (the existing ambient-line
mechanism above) in modes that structurally cannot.

## Non-goals

Carried over from the prior draft, unchanged:
- Codex-style attach/drive (typing into a subagent's own thread).
- Changing `AgentTaskManager`, `ChildAgentEvent`, or the event model —
  already sufficient.
- A new `role`/nickname field on `spawn_agent` — reuse `task_name`.
- Touching `src/acp/` — separate consumer (`TaskEventHub`), separate
  `AgentTaskManager` instance (see below), out of scope.

## Existing infrastructure

| Piece | Location | Reuse |
|---|---|---|
| `SubagentActivityBridge` (data/aggregation layer) | `src/core/subagent_activity.h/.cpp` (uncommitted, already wired) | Event aggregation, per-id name/status/history, thread-safe queue + wake — the pane's data source; extend with a snapshot accessor, don't rebuild |
| Fan-out wiring, CLI's own `AgentTaskManager` | `src/main.cpp:1631-1650` | Confirmed current; `SubagentActivityBridge` already occupies the subscriber slot this plan would otherwise need to add |
| `ChildAgentEvent`/`AgentTaskEvent` variant | `src/core/agent_task.h:219-227` | Unchanged; `SubagentActivityBridge::observe` already the consumer |
| Non-terminal "completed" status semantics | `src/core/agent_task.cpp:194-211` (`agent_task_status_to_string`), reused by `status_word()` at `src/core/subagent_activity.cpp:25-36` | Vocabulary source; `subagent_activity.cpp` already gets this right |
| ACP's separate `TaskEventHub` consumer | `src/acp/server.cpp:62-66`, `src/acp/task_events.cpp:50,60` | Confirmed a *separate* `AgentTaskManager` instance from the CLI's (`main.cpp:1662-1666`); out of scope, unaffected by anything below |
| `Renderer` interface, `owns_status_line()`/`set_status_line()` pattern | `src/core/stream_renderer.h:147-148` | Exact pattern to mirror for a new `owns_subagent_pane()`/`set_subagent_pane()` pair |
| `RegionRenderer`'s reserved-composer-rows mechanism (DECSTBM + CUP) | `src/core/region_renderer.cpp:35-39,655-658,1422-1466,1516-1524,1526-1591` | **The concrete, already-proven mechanism to extend** for the pane — see Design |
| `ViewportRenderer`'s thinner reserved-status-row mechanism | `src/core/stream_renderer.cpp:515-523` (`set_scroll_region`), `634-691` (`paint_status`) | Same technique, smaller scale; missing `on_resize()` override (gap, see Risks) |
| `resize_generation()`/`install_resize_handler()` | `src/core/terminal.cpp:157-172`; polled at `region_renderer.cpp:1340-1343` and `cli/readline.cpp:1519-1523` | Reused as-is; no new resize plumbing needed |
| `truncate_ansi_line`/`display_columns` | `src/core/terminal.h:96-100` | Reuse for pane row truncation, exactly as `status_sequence()` already does (`region_renderer.cpp:1574,1577`) |
| `[display]` config section | `src/cli/config.cpp:531-533` (parse), `:716` (`merge_str`/OR-merge pattern) | Natural home for a new toggle; `Args::mailbox_enabled`/`mailbox_enabled_explicit` pair (`cli/args.h:83-84`) is the existing precedent for "explicit override on top of a computed runtime default" |
| `test/test_agent_tasks.cpp`, `test/test_mailbox_coordinator.cpp` | — | Zero `EventCallback`/`AgentTaskEvent`-firing coverage confirmed (no matches for those symbols in `test_agent_tasks.cpp`); throwing-observer isolation **is** covered (`test/test_mailbox_coordinator.cpp:81-86`, path corrected — lives under `src/core/mailbox/mailbox_coordinator.cpp`) |
| `SubagentActivityBridge` test coverage | — | **Zero.** No `test-subagent-activity` CMake target exists (checked `CMakeLists.txt`); `subagent_activity.cpp` is only listed as a `pi-core` source (`CMakeLists.txt:471`) |
| `test/test_region_renderer.cpp` — pipe-capture harness | 1458 lines, e.g. `test_composer_rows_reserved_in_region_mode` (`:678-716`), `diff_region_rows` coverage (`:164-185`) | **Confirmed by independent review to already exist and already assert exactly the row-map invariants this plan changes** (DECSTBM boundary string, status-row position). This is the best de-risking asset for Phase 1 and the original research pass missed it — extend it with a pane-active assertion rather than treating region-mode rendering as uncovered |

## Per-render-mode feasibility

Render mode → factory confirmed at `src/core/stream_renderer.cpp:875-879`:
`raw`→`make_raw_renderer`, `markdown`→`make_diff_renderer`,
`viewport`→`make_viewport_renderer`, `region`→`make_region_renderer`;
`auto` (the default when `--render` is unset) resolves to `markdown` on a
TTY or `raw` otherwise (`stream_renderer.cpp:867-871`) — **`auto` never
resolves to `viewport` or `region`.** This matters for defaults, see Open
Questions.

| Mode | Output style | Owns screen space? | Reserved-pane feasible? |
|---|---|---|---|
| `raw` (`RawStreamRenderer`, `stream_renderer.cpp:76-121`) | Pure append-only byte passthrough, explicitly for non-TTY/pipes | `owns_status_line()` not overridden → `false` (default) | **No.** No cursor addressing anywhere in the class; this is its entire design contract. Fallback: existing ambient lines via `on_command_output` (`stream_renderer.cpp:89-93`, unmodified). |
| `markdown` (`DiffMarkdownRenderer`, `stream_renderer.cpp:139-330`) | "Scrollback-safe streaming renderer" (its own doc comment, `stream_renderer.cpp:120`) — cursor-up + `\033[J` only ever rewrites its own still-live tail block, deliberately never touches anything already committed to scrollback | `owns_status_line()` not overridden → `false` | **No.** Reserving fixed bottom rows here would require alt-screen/DECSTBM, defeating the documented purpose of this mode (safe under scrollback, safe when piped through `less`/tmux copy-mode, etc). Fallback: existing ambient lines via `on_command_output` (`stream_renderer.cpp:248-252`, unmodified). |
| `viewport` (`ViewportRenderer`, `stream_renderer.cpp:332-720`) | Full-screen immediate-mode compositor: `AltScreenSession` (`:333`) + DECSTBM scroll region (`set_scroll_region()`, `:509-517`, reserves rows `1..h-2`) + one absolute-addressed status row (`paint_status()`, `:652-693`, row `h-1`) | `owns_status_line()` → `true` (`:400`) | **Yes, extending the existing DECSTBM band**, but two gaps to close first (see Risks): (1) no `on_resize()` override exists anywhere in this file, so a resize while idle at the prompt never resyncs the scroll-region boundary until the next turn starts; (2) `on_command_output`'s scroll-reset bug above must not be hit by the pane's own updates (it won't be, since the pane uses a new dedicated method, not `on_command_output`). |
| `region` (`RegionRenderer`, `region_renderer.cpp:661-1608`) | Persistent single alt-screen session for the whole process lifetime, DECSTBM content region + absolute-addressed status row + absolute-addressed composer rows, throttled ~60fps paint thread with diff-painted content and full resize handling | `owns_status_line()` → `true` (`:1151`) | **Yes — the strongest fit.** Already has every primitive needed: DECSTBM band math (`scroll_region_sequence`, `:655-658`), a proven "reserved band below live scrolling content" layout (`kComposerRows`, `:39`), full resize handling (`on_resize()` override, `:1231-1247`, polling `resize_generation()`), a background paint thread with dirty-flag coalescing (`paint_loop`, `:1367-1417`) *and* a synchronous idle-repaint path for exactly the case where a background thread must not race the foreground prompt (`paint_idle_synchronously()`, `:1348-1365`, already used by `set_status_line()`, `:1152-1160`, whose dual-path pattern is the direct template for the new pane setter). |

**Non-interactive/piped/`-p` exclusion**: `readline()` itself falls back to
plain `getline` when `isatty(STDIN_FILENO) == 0` (`cli/readline.cpp:1404-1414`),
bypassing all cursor-addressed rendering entirely; `args.print_mode`
short-circuits before the interactive loop even starts (`main.cpp:2313-2314`).
The existing gate `(isatty(STDIN_FILENO) != 0 && !args.print_mode)` at
`main.cpp:2229,2257` is the exact condition to reuse for the new pane as
well — it is already load-bearing for the current ambient-line mechanism,
so this is a proven, not speculative, exclusion.

## Event plumbing confirmation (re-verified)

- `ChildAgentEvent`/`AgentTaskEvent` variant: `src/core/agent_task.h:219-227`.
- `EventCallback` fan-out seam, CLI's own `AgentTaskManager`:
  `src/main.cpp:1631-1650` (current line numbers; shifted from the prior
  draft's `1628-1637` due to the uncommitted `SubagentActivityBridge`
  insertion).
- `AgentTaskManager::list()`/`get()`: `src/core/agent_task.h:268-270`;
  consumed today by `addons/agents.lua`'s `list_output()`
  (`addons/agents.lua:95-112`).
- Threading: `ChildAgentEvent` fires on the child task's own worker thread,
  inside `execute_work`'s per-event callback (`agent_task.cpp:836-848`,
  `emit()` at `:840`); `AgentTaskSpawnedEvent` fires on whatever thread
  called `spawn()` (`agent_task.cpp:822-827`, `emit()` call after `if
  (published)`). `SubagentActivityBridge::observe()`/`enqueue()`
  (`subagent_activity.cpp:42-55`) already handles this correctly via a
  mutex-guarded `pending_`/`history_` plus a `WakeCallback` — no new
  thread-safety work needed for the data layer.
- Fan-out isolation: `fan_out_agent_task_callbacks`
  (`mailbox_coordinator.cpp:74-88`) catches per-callback exceptions;
  `AgentTaskManager::emit()` (`agent_task.cpp:421-429`) catches again at the
  source; covered by `test/test_mailbox_coordinator.cpp:80-85`. Confirmed
  still true; no new test needed here.
- ACP scoping: `src/acp/server.cpp:61-66` constructs its own
  `AgentTaskManager` (`task_root`, a fresh `AgentSession`) with its own
  `EventCallback` publishing into `TaskEventHub` (`task_events.cpp:50,60`)
  — entirely separate from the CLI's instance at `main.cpp:1662-1666`.
  Nothing in this plan touches `src/acp/`.

## Design

### 1. Renderer interface addition

Add to `Renderer` (`src/core/stream_renderer.h`), mirroring the existing
`owns_status_line()`/`set_status_line()` pair at `:145-148`:

```cpp
struct SubagentPaneRow {
  std::string id;             // plain string, not AgentTaskId
  std::string name;
  std::string status;         // pre-formatted by status_word(), not an enum
  std::string last_activity;  // pre-truncated by the caller is NOT assumed;
                               // renderer truncates per its own current width
};

virtual bool owns_subagent_pane() const { return false; }
virtual void set_subagent_pane(const std::vector<SubagentPaneRow> &rows) {}
```

**Corrected from independent review**: use plain `std::string` fields, not
`AgentTaskId`/`AgentTaskStatusKind`. `agent_task.h` pulls in `agent.h`,
`session/agent_session.h`, `<thread>`, `<stop_token>` — dragging that into
`stream_renderer.h`, which is included nearly everywhere, is an
unnecessary coupling with no upside: `status_word()`
(`subagent_activity.cpp:25-36`) already produces exactly the display
string the renderer wants, so there's no reason for the renderer layer to
see the enum at all.

Default no-op everywhere except `ViewportRenderer` and `RegionRenderer`,
which override `owns_subagent_pane()` to `true`. This is a capability flag,
analogous to `owns_status_line()`, so `SubagentActivityBridge`'s
renderer-facing code can branch on it exactly the way `main.cpp` already
branches on `owns_status_line()` (e.g. `main.cpp:2307`, `2333`). **Note**:
as of this plan, `owns_subagent_pane()` is `true` for exactly the same two
renderers as `owns_status_line()` — it's a distinct flag for future-proofing
(it encodes "can reserve additional fixed rows for a live table," a
narrower capability than "owns a status line"), not because any renderer
differs on the two today. Document that distinction in the interface
comment so a future reader doesn't wonder why there are two identical-today
flags.

### 2. `SubagentActivityBridge` gains a pane snapshot accessor

Add `std::vector<SubagentPaneRow> pane_rows() const` alongside the existing
`recent()`/`summary()` (`subagent_activity.h:27-28`), built from the
already-tracked `names_`/`statuses_`/`history_` maps
(`subagent_activity.h:41-43`) — `history_[id].back()` is already the most
recent formatted line, `status_word()`-equivalent logic already exists
(`subagent_activity.cpp:25-36`) and should be reused directly rather than
duplicated. No new data structures.

At each of `SubagentActivityBridge`'s existing call sites where renderer
output happens (`main.cpp:845-848` mid-turn, `main.cpp:2376`, `2382` idle),
add: if `renderer.owns_subagent_pane()`, call
`renderer.set_subagent_pane(activity->pane_rows())` in addition to
(region/viewport) or instead of (once fixed) draining ambient lines through
`on_command_output` for those two modes. For `raw`/`markdown`
(`owns_subagent_pane() == false`), behavior is **unchanged** — keep
draining through `drain()`/`on_command_output` exactly as already
implemented.

This is the one place genuinely new "glue" code is needed in `main.cpp`;
everything else is renderer-internal.

### 3. Region-mode mechanism — extend the existing composer-row reservation

This is the core of the plan and the strongest simplification over the
prior draft: **the pane is not transcript content, so it is not a
`RegionState`/`RegionTurn` concern at all.** The prior draft's biggest
identified risk — retrofitting `RegionState` with a cross-turn `subagents`
map and its own addressing/diffing alongside `tool_index`/`tool_addresses`
(which are turn-scoped and cleared on every `on_turn_start()`, that
draft's citation) — is entirely avoided, because the pane is painted the
same way the status bar already is: a fixed absolute-addressed band,
outside `build_region_frame()`'s pure scrolling-content builder, never
touching `RegionState.turns`.

**Corrected from independent review — three load-bearing geometry
decisions, resolved here rather than left open:**

**(a) Row placement.** The plan's earlier phrasing ("pinned to N bottom
rows") is wrong and would misdirect an implementer. The bottom
`kComposerRows` (`= 6`, `:39`) rows belong to readline's `InputRenderer`,
and `position_prompt_cursor()` (`:1518-1526`) **unconditionally erases rows
`prompt_anchor+1..height` on every `prepare_for_prompt()`** — any pane
placed there gets wiped before every prompt redraw. The pane must sit
**immediately above the status row**, not at the true bottom. Full row map
with `P = pane_rows_reserved` (0 or `kSubagentPaneRows`):

```
rows 1            .. h-1-kComposerRows-P   content (scrolling transcript)
rows h-kComposerRows-P .. h-1-kComposerRows  subagent pane (new)
row  h-kComposerRows                       status row (unchanged position)
rows h-kComposerRows+1 .. h                 composer (readline's, unchanged)
```

Status row and composer keep their current absolute positions always —
only the content/pane boundary moves. This is what makes the "transition ≈
resize" mitigation below safe: status and composer are invariant, so a
pane appear/disappear can never corrupt readline's own reserved rows.

**(b) Mid-turn geometry policy.** `region_renderer.cpp:1282-1325`
documents that mid-turn geometry changes were **empirically confirmed via
forkpty+tmux to corrupt the screen**, and the fix already shipped is that
`render_frame` uses a *pinned* height (`turn_layout_height`, set at
`on_turn_start()`) rather than live `term_height(fd_)` for the whole
duration of an active turn (`:1430-1432`) — geometry simply does not
change mid-turn, by design, full stop. A subagent spawning mid-turn is
exactly this class of event, and the plan must not wave it through by
analogy to a *different*, already-solved hazard (see (c) below) without
addressing this one.

**Decision: `pane_rows_reserved` is pinned for the duration of an active
turn, exactly like `turn_layout_height`, and only re-evaluated at
`on_turn_start()` or during an idle synchronous repaint.** Consequence: if
a subagent spawns mid-turn, the pane band doesn't appear until the next
turn boundary or the next idle repaint (whichever comes first) — not
instantly. This is an accepted trade-off, not a bug: it reuses the
existing, tested-safe pin mechanism instead of introducing a new "is a
pane-only geometry change actually safe mid-turn" hazard that has not been
verified the way the resize corruption was. If real usage shows the lag is
unacceptable (a long single turn that spawns agents early and the user
gets no pane until it ends), revisit with an actual forkpty+tmux repro
test proving safety, per the existing precedent's own methodology — don't
assume safety by argument.

**(c) `layout_changed` must actually notice the transition.** The existing
predicate at `region_renderer.cpp:1451` is `width != last_width_ || height
!= last_height_` — it has no way to detect "pane went from inactive to
active at the same terminal size," so as written the "reissue
`scroll_region_sequence` and force a full repaint" mitigation described
below **would silently never fire on a pane-only transition**. Add a
`last_pane_rows_` member alongside `last_width_`/`last_height_`, and extend
the predicate to `width != last_width_ || height != last_height_ ||
pane_rows_reserved != last_pane_rows_`. This is a two-line fix but it is
the two lines that make the "inherits the already-hardened resize handling"
claim below actually true instead of aspirational.

Concretely, with (a)-(c) settled:

- Add a `subagent_rows` field to `State : RegionState`
  (`region_renderer.cpp:1273`, the renderer-private struct that already
  carries `custom_status_line`, `status_text`, `has_error`, etc. on top of
  the public `RegionState`) — a `std::vector<SubagentPaneRow>`.
- Add `set_subagent_pane()` following the exact dual-path pattern already
  used by `set_status_line()` (`region_renderer.cpp:1152-1160`): mutate
  `state_.subagent_rows` under `mutex_`, bump `state_.revision`, call
  `mark_dirty_locked()`, and if `!turn_active_`, call
  `paint_idle_synchronously()` directly (the proven synchronous-repaint
  path for exactly this "must not race the foreground prompt while idle"
  hazard, `region_renderer.cpp:1348-1365`). Per (b), while a turn *is*
  active the row-count portion of this update is recorded but the DECSTBM
  boundary itself does not move until the next `on_turn_start()`/idle
  repaint — only the row *contents* (name/status/activity text) repaint
  live during a turn, within whatever band is already reserved.
- Introduce a new reserved band, `kSubagentPaneRows` (constant or
  config-derived, capped e.g. at 4-6 rows + one "+N more" line — same shape
  as `kComposerRows`, `:39`), sized dynamically to `min(active_agent_count,
  cap) + (1 if truncated else 0)`, but with **only two states that matter
  for layout math**: "no pane" (0 rows, today's behavior) and "pane active"
  (fixed `kSubagentPaneRows`, populated with placeholder/blank rows if
  fewer agents are active than the cap). This avoids per-agent-count
  DECSTBM churn — the boundary only moves twice per "burst" of subagent
  activity (first spawn, last close), not on every status change, and per
  (b) only at turn boundaries/idle in any case.
- Extend `scroll_region_sequence()` (`:655-658`) and the `content_rows`
  calc in `render_frame()` (`:1433`) from `height - 1 - kComposerRows` to
  `height - 1 - kComposerRows - pane_rows_reserved`, per the row map in
  (a).
- Add a new `subagent_pane_sequence()` static function, structurally
  identical to `status_sequence()` (`:1524-1591`): for each reserved row,
  `\033[<row>;1H\033[2K` (absolute CUP + erase-line) followed by the
  formatted, colored, `truncate_ansi_line()`-truncated row content — same
  primitives `status_sequence()` already uses (`display_columns`,
  `truncate_ansi_line`, `:1574,1577`). Row addresses are `h-kComposerRows-P`
  through `h-1-kComposerRows`, per (a) — not the bottom of the screen.
- Row content format: `<icon/color> <name>  <status>  <last_activity>` —
  icon/color per status using the `has_error`-vs-`\033[2m` dim precedent
  already in `status_sequence()` (`:1585`) for visual consistency (e.g.
  bold-yellow running, dim errored-red, dim neutral idle — exact palette is
  an implementation-time pick, not load-bearing).
- **Transition handling (append/reclaim rows)** is modeled as the same
  class of event as a live resize, gated by the fixed (c) predicate above:
  when the "no pane"/"pane active" state flips (only ever evaluated at a
  turn boundary or idle repaint, per (b)), reissue
  `scroll_region_sequence(height)` and force a full repaint by clearing
  `last_frame_lines_` — exactly what already happens today when
  `layout_changed` is detected (`region_renderer.cpp:1451-1454`, `output +=
  scroll_region_sequence(height)` at `:1460`), now that `layout_changed`
  actually includes the pane-row term. This reuses the same mitigation path
  the renderer's own comments (`:1280-1325`) document as already hardened
  against orphaned/unreachable rows from a moving DECSTBM boundary — but
  only *because* (a) keeps status/composer positions invariant and (b)
  confines all boundary moves to the same safe-by-construction moments
  (turn start, idle) that `turn_layout_height` already restricts real
  resizes to.
- `force_full_repaint()` (`:1254-1272`, used after `/tree`/`/model` pickers
  draw over the alt screen) must also cover the pane band — since it
  already clears `last_frame_lines_` unconditionally and the pane's own row
  cache would need the same treatment, this is a small addition to that
  existing method, not new design.

### 4. Viewport-mode mechanism — same technique, smaller scale, one prerequisite fix

- Extend `set_scroll_region()` (`stream_renderer.cpp:515-523`) from `h - 2`
  to `h - 2 - kSubagentPaneRows` (dynamically, same "no pane"/"pane active"
  two-state model as region — including region's (b) mid-turn-pin policy;
  viewport has no separate `turn_layout_height`-style pin today, so verify
  during implementation whether the same "boundary only moves at turn
  start/idle" restriction needs to be added here too, or whether viewport's
  simpler single-status-row precedent already makes mid-turn moves safe —
  don't assume parity with region without checking).
- Extend `repaint()`'s `content_rows` calc (`:528`, currently `h - 2`) the
  same way.
- Add a `paint_subagent_pane()` method mirroring `paint_status()`
  (`:634-691`) — absolute CUP addressing at the newly reserved rows,
  positioned immediately above the status row (same placement logic as
  region's (a), adjusted for viewport's simpler single-status-row layout —
  no composer rows to route around here), same truncation/formatting
  approach.
- **Prerequisite fix, in scope for this phase**: `ViewportRenderer` has no
  `on_resize()` override anywhere (confirmed by grep across
  `stream_renderer.cpp` — only the base no-op applies), so a SIGWINCH while
  idle at the prompt (the path that calls `renderer->on_resize()` via
  `main.cpp:2365`'s `on_prompt_resize` lambda, itself driven by
  `readline.cpp:1519-1523`'s `resize_generation()` poll) currently does
  nothing for viewport mode; the scroll region only resyncs at the next
  `on_turn_start()` (`:352-361`, which calls `set_scroll_region()` again).
  This is a **pre-existing gap**, not introduced by this plan, but the new
  pane's explicit "must survive resize" requirement makes it actively
  wrong (the pane's row math would be stale exactly when
  idle-resize-then-subagent-still-running is the motivating scenario). Add
  `void on_resize() override` that recomputes `set_scroll_region()`,
  repaints content, and repaints both `paint_status()` and the new
  `paint_subagent_pane()` — following the same shape
  `RegionRenderer::on_resize()` already uses (`region_renderer.cpp:1231-1247`).

### 5. `raw`/`markdown` — no change beyond what already exists

Both modes keep the existing (uncommitted, already-working) ambient-line
mechanism unmodified: `SubagentActivityBridge::drain()` →
`Renderer::on_command_output()`. No reserved pane is attempted — see
Feasibility table for why this is a structural, not effort, limitation.

### 6. Content, bounding, and vocabulary

- One row per active agent: `id`/`name` (prefer `name` = `task_name`, fall
  back to a shortened id if empty — **correction from independent review**:
  the earlier citation of `docs/region-renderer.md`'s REQUEST section as a
  "display-name fallback chain" precedent was wrong; that section documents
  REQUEST *source* labels like `REQUEST | MAILBOX | /root/luna`, not
  agent-name fallback. There is no existing precedent to reuse here — this
  is new, small logic, not a lookup into prior art), status (colored, using
  `status_word()`'s existing
  vocabulary, `subagent_activity.cpp:25-36` — `completed → "idle"`, never
  "done"), last-activity line (`history_[id].back()`, truncated via
  `truncate_ansi_line()`).
- Cap visible rows at a small constant (start at 4-6, matching
  `kComposerRows`'s scale, `:39`); when more agents are active than fit,
  collapse the overflow into a trailing `"+N more"` row — same idea the
  prior draft's Codex/pi research surfaced, applied here as a fixed-row-
  budget detail rather than a toggleable expand/collapse (no keypress
  requirement per the new direction removes the need for pi's `Ctrl+O`-style
  toggle).
- Width handling: reuse `truncate_ansi_line(line, width)` /
  `display_columns(line)` (`terminal.h:96-100`), exactly as
  `status_sequence()`/`paint_status()` already do for the status row.

### 7. Update cadence and thread safety

**Corrected from independent review**: the previous version of this
section described renderer-side coalescing and stopped there — it never
explained the mechanism that actually delivers the "zero keypress" part of
the goal while the user is idle at the prompt, which is the whole point of
choosing a persistent pane over option A/D from the earlier comparison.
That mechanism already exists (it's what makes today's ambient lines
update without a keypress) and needs to be stated explicitly because it
has real costs and one real gap:

**How idle updates actually happen today.** At idle, the main thread is
blocked inside `readline()` — nothing can call `set_subagent_pane()` from
there directly. The existing flow: `SubagentActivityBridge`'s
`WakeCallback` (wired at `main.cpp:1632-1633`) fires on the event's own
thread and signals `mailbox_wake`; `readline()` exits with
`ReadlineExit::mailbox_wake` (`main.cpp:2373`); the main loop drains the
bridge (`:2376`), runs `run_idle_mailbox_turns()`, then re-enters
`readline()`. The pane's `set_subagent_pane()` call is added to this same
drain point (§2) — it does **not** need a new wake path, it rides the one
that already exists.

**Costs this plan must account for, not wave away:**
- This is a full readline teardown/rebuild per drain, not a cheap repaint.
  With 3-4 agents each producing spawn/tool-start/tool-end/message-end
  events, every single one currently bounces readline out and back. Verify
  during implementation whether `WakeCallback` already coalesces bursts of
  `enqueue()` calls into one wake, or fires once per event — if the latter,
  add coalescing (e.g. a small debounce window, or drop redundant wakes
  while one is already pending) before shipping the pane, or a 4-agent
  session will flicker the prompt continuously. This is a real risk the
  original draft's "no additional debounce needed" conclusion missed by
  only evaluating renderer-side frame coalescing, not the readline-bounce
  cost that precedes it.
- `readline_wake_fd` is `-1` when `autonomous_budget_exhausted`
  (`main.cpp:2356-2358`) — the pane (and today's ambient lines) simply
  freeze at idle once the autonomous budget is exhausted, with no visible
  indication why. Either extend wake-fd construction to not depend on that
  gate (mirroring the earlier draft's note that `ReadlineWake` needing to
  be non-mailbox-gated was a known constraint), or explicitly document the
  freeze as accepted behavior — don't leave it as a silent gap.
- An alternative considered and rejected for now: let `RegionRenderer`'s
  own background `paint_loop()` repaint the pane band while idle, instead
  of routing through readline. Rejected because `paint_loop()` deliberately
  refuses to paint while `!turn_active_` (`:1383-1391`) — the foreground
  prompt owns the fd at idle — so this would need new
  prompt/paint-thread coordination that doesn't exist today. The
  readline-bounce approach is not pretty but it's proven; revisit only if
  the coalescing fix above still isn't enough in practice.

Ride each renderer's existing coalescing for the paint itself, once the
call arrives:
- **Region**: `set_subagent_pane()` follows `set_status_line()`'s exact
  dual-path pattern (mutex-guarded state mutation + dirty flag, painted
  either by the 16ms-coalesced background `paint_loop()` during an active
  turn, or synchronously via `paint_idle_synchronously()` when idle) —
  already proven safe for exactly this cross-thread hazard.
- **Viewport**: has no background paint thread at all; all its methods
  draw synchronously on whatever thread calls them, per the base
  `Renderer` contract's documented threading note (`stream_renderer.h:48-50`,
  "implementations that touch UI state must marshal to the appropriate
  thread themselves"). `set_subagent_pane()` must only ever be called from
  the same main-thread injection points `drain()` already safely uses today
  (`main.cpp:845-848` mid-turn, `:2376`/`2382` idle) — no new marshaling
  required, just don't call it from `SubagentActivityBridge::observe()`
  directly (which runs on arbitrary worker threads).
- Debounce: `SubagentActivityBridge`'s own event volume is already bounded
  by its filtering (spawn/status-change/close/tool-start/tool-end/message-end
  only — no raw per-token deltas, confirmed by reading `observe()`,
  `subagent_activity.cpp:57-119`), so *paint* updates are inherently
  coarse-grained once a drain happens; the debounce gap that actually
  matters is upstream, on the readline-bounce cadence described above, not
  the renderers' own frame coalescing.

### 8. Resize handling

- Region: already correct (`on_resize()`, `region_renderer.cpp:1231-1247`);
  extend to also account for `kSubagentPaneRows` in its row math, no new
  resize-detection code.
- Viewport: needs the `on_resize()` override added per §4 above — this is
  new code, but small, and directly modeled on region's existing override.

### 9. Lifecycle and row reclaim

Pane appears on first `AgentTaskSpawnedEvent`, disappears when the agent
count returns to zero (last `AgentTaskClosedEvent`) — both
`SubagentActivityBridge::observe()`'s existing `statuses_` map
(`subagent_activity.h:43`) already tracks enough to derive "any agent
currently non-closed" for `pane_rows()`. Row reclaim on disappearance is
handled by the same "state transition ≈ resize" full-repaint path
described in §3/§4 — not a bespoke wipe routine.

**Correction from independent review — pre-existing defects in the
uncommitted aggregator that affect this section directly**, found while
verifying the plan against source:

- `statuses_.erase()` runs on `AgentTaskClosedEvent`
  (`subagent_activity.cpp:88`), but `names_` and `history_` are **never
  erased** — unbounded per-session growth over a long-lived session with
  many spawn/close cycles, and no defined answer for "which map is
  authoritative for 'alive'" once they disagree. `pane_rows()` should
  derive membership from `statuses_` alone (already the plan's intent) but
  the leak in the other two maps should be fixed alongside it, not left as
  a silent accumulation — add eviction of `names_`/`history_` entries at
  the same point `statuses_` erases.
- No tombstone: an agent that errors and is immediately closed has its row
  simply vanish from the pane with no trace, which may read as "did that
  even happen" to a user who wasn't looking at that exact moment. Not
  fixing this in the baseline, but flagging it as a deliberate scope
  decision rather than an oversight — pi's/Codex's research in the earlier
  draft suggested a brief "closed" flash before removal; revisit as a
  Phase 4 stretch if real usage shows it's missed.

### 10. Config

`[display]` section already exists and is parsed via the same
`str`/`boolean` helper pattern (`cli/config.cpp:531-533`); add:
```
cfg.subagent_pane = boolean("display", "subagent_pane");   // parse
out.subagent_pane = config.subagent_pane || cli.subagent_pane;  // merge (config.cpp:~717, verbose's pattern)
```
Runtime default, computed at the call site the same way the existing
`activity` gate already is (`main.cpp:2229,2257`): on when
`isatty(STDIN_FILENO) && !args.print_mode && renderer->owns_subagent_pane()`,
off otherwise. The config bool acts as an explicit override on top of that
computed default — same two-flag shape as
`Args::mailbox_enabled`/`mailbox_enabled_explicit` (`cli/args.h:83-84`) if
an explicit-vs-default distinction turns out to be needed; simple OR-merge
(verbose's pattern) is sufficient if not.

## Phases

### Phase 0 — test coverage foundation (no behavior change)

**Corrected from independent review**: the original framing here was
backwards. It's not that "nothing is tested" — `test/test_region_renderer.cpp`
(1458 lines) already has a working pipe-capture harness that asserts
exactly the row-map invariants §3 changes, including
`test_composer_rows_reserved_in_region_mode` (`:678-716`, asserts the
DECSTBM boundary string and status-row position) and direct
`diff_region_rows` coverage (`:164-185`). The *rendering* layer — the
riskier half of this feature — already has a real safety net. The
*aggregator* — the part that looks trivially testable — is what's actually
uncovered. Revised scope:

1. `SubagentActivityBridge` has zero test coverage (no
   `test-subagent-activity` CMake target; confirmed by grep across `test/`
   and `CMakeLists.txt`) despite being the data layer this entire feature
   depends on. Add a focused test exercising `observe()` →
   `pane_rows()`/`recent()`/`summary()` for a spawn → tool-start → tool-end
   → status-change → close sequence, including the terminal-state
   vocabulary (`completed` renders as `"idle"`, never `"done"`). While
   writing this test, fix the two defects independent review found by
   direct inspection of the uncommitted code (both are one-line-scale
   fixes, not new design):
   - `subagent_activity.cpp:134-135`: `renderer.on_command_output(name.empty()
     ? activity.line : activity.line + "\n")` — the trailing newline is
     conditioned on the *name* being non-empty, which has no logical
     connection to whether a newline is needed. Almost certainly a
     leftover from earlier iteration; fix to a consistent newline policy.
   - `summary()` (`:148-164`) only counts `running` and
     `completed`/`idle` statuses. `observe()` sets `pending_init` on spawn
     (`:65`), which is neither, so `summary()` reports empty immediately
     after a spawn until the agent's first status transition. Fix the
     count to include `pending_init` (as "starting" or folded into
     "running", implementation's choice) so the aggregate status line
     isn't blank right when a spawn just happened.
2. Extend `test/test_region_renderer.cpp`'s existing harness with a
   pane-active assertion — following the same pattern as
   `test_composer_rows_reserved_in_region_mode` — so the row map from §3(a)
   (content/pane/status/composer boundaries, including the "no pane" vs.
   "pane active" DECSTBM string) is verified by CI, not eyeballed. This is
   the single most valuable Phase 0 addition given how much of §3's safety
   argument rests on getting that row map exactly right.
3. `test/test_agent_tasks.cpp`'s zero coverage of full
   `EventCallback`/`AgentTaskEvent` ordering (`AgentTaskSpawnedEvent →
   ChildAgentEvent(s) → AgentTaskStatusChangedEvent → AgentTaskClosedEvent`,
   via `FauxClient`) is real but lower priority than items 1-2 — it's a
   bigger lift and the aggregator test in item 1 already exercises the
   event *shapes* the pane depends on, even without asserting the
   manager's own emission ordering end-to-end. Demoted to a stretch item:
   pick it up if time allows, not a blocker for Phase 1.

Do not build the pane on top of an untested aggregator; items 1-2 are
small and directly de-risk everything after them.

### Phase 1 — Renderer interface + region-mode pane (primary target)

`owns_subagent_pane()`/`set_subagent_pane()` on `Renderer`;
`SubagentPaneRow`; `SubagentActivityBridge::pane_rows()`; full region-mode
implementation per §3 (band reservation, `subagent_pane_sequence()`,
transition-as-resize repaint, `force_full_repaint()` coverage). Region is
the primary target because every supporting primitive (DECSTBM band math,
background paint thread + idle-sync path, resize handling) already exists
and is proven. Acceptance: a faux-control scenario spawning a subagent
shows a live table row that survives the root turn ending, updates without
merging into `WORK`/`ANSWER` content, and disappears cleanly (no orphaned
rows) when the subagent closes.

### Phase 2 — viewport-mode pane + `on_resize()` prerequisite fix

Per §4: `set_scroll_region()`/`repaint()` extension,
`paint_subagent_pane()`, and the new `ViewportRenderer::on_resize()`
override (needed regardless of this feature, but bundled here since it's a
direct prerequisite). Acceptance: same faux-control scenario as Phase 1,
plus an explicit idle-resize test proving the layout resyncs without
waiting for the next turn.

### Phase 3 — wiring, config, gating polish

`main.cpp` glue at the existing `drain()` call sites (`:845-848`, `:2376`,
`:2382`) to branch on `owns_subagent_pane()`; `[display] subagent_pane`
config key; verify the non-interactive/`-p`/piped exclusion end-to-end for
both new modes; verify `raw`/`markdown` behavior is provably unchanged
(regression coverage, since their code paths aren't touched but the shared
`SubagentActivityBridge` call sites are).

### Phase 4 — stretch (not required for the baseline)

Extract a shared "reserved band" helper if the region/viewport duplication
proves painful in practice (both phases above intentionally implement it
twice, following each renderer's existing independent-implementation
pattern, rather than speculatively abstracting before there are two
concrete implementations to compare).

## Risks and mitigations

1. **Building on uncommitted code.** This plan's baseline assumes the
   current working-tree state of `subagent_activity.h/.cpp` and the
   `main.cpp` wiring around them. If that work is committed, reverted, or
   further modified before implementation starts, the cited line numbers
   (and possibly the exact `pane_rows()`/`drain()` integration points) need
   re-verification. Flagged explicitly rather than treated as a stable
   foundation.
2. **DECSTBM transition glitches on pane appear/disappear.** Mitigated by
   treating the transition identically to a live resize (full repaint via
   `last_frame_lines_` clear + `scroll_region_sequence` reissue) — reusing
   region mode's already-hardened mitigation for this exact class of
   hazard (documented in-code at `region_renderer.cpp:1280-1325`). **This
   mitigation is only actually true given three fixes independent review
   found necessary and folded into §3**: the pane must sit above the
   status row, not at the literal bottom (composer rows are readline's and
   get wiped every prompt); the DECSTBM boundary must only move at turn
   boundaries/idle, matching `turn_layout_height`'s existing mid-turn pin,
   not mid-turn (the resize corruption `:1280-1325` documents was
   specifically a mid-turn geometry change); and `layout_changed`
   (`:1451`) must be extended with a `last_pane_rows_` term or the
   "reissue on transition" mitigation silently never fires. Without these
   three, citing `:1280-1325` as precedent would have been citing a
   mechanism that (on the original phrasing) didn't actually apply.
3. **Idle-update cost and a silent freeze gap.** Every subagent update
   currently drives a full readline teardown/rebuild via the
   `mailbox_wake` bounce (§7) — with several agents producing frequent
   events, this may flicker the prompt if `WakeCallback` doesn't already
   coalesce bursts; verify and add coalescing if not. Separately,
   `readline_wake_fd` is `-1` when `autonomous_budget_exhausted`
   (`main.cpp:2356-2358`), so the pane silently stops updating at idle in
   that state today — decide explicitly whether to fix the wake-fd gate or
   document the freeze, rather than leaving it undiscovered until a user
   reports it.
4. **Two pre-existing defects in the uncommitted aggregator.** A
   newline-emission bug conditioned on the wrong variable
   (`subagent_activity.cpp:134-135`) and `summary()` undercounting
   `pending_init` agents (`:148-164`, blank aggregate right after a spawn)
   — both scoped into Phase 0 item 1, not deferred.
5. **Viewport's pre-existing `on_resize()` gap.** Not introduced by this
   plan, but the pane's resize-survival requirement makes it load-bearing
   where before it was a latent cosmetic bug (idle-resize simply didn't
   repaint the 2-row footer correctly until the next turn). Scoped
   explicitly into Phase 2 rather than left as a silent dependency.
6. **Two-tier experience.** `raw`/`markdown` never get a true reserved
   pane, by design (structural limitation of scrollback/pipe safety, not
   effort). They keep the already-implemented ambient-line fallback, so no
   mode regresses to zero visibility — same acceptance bar the prior draft
   used, still valid here.
7. **Region/viewport code duplication.** No shared abstraction exists today
   between the two renderers' reserved-row mechanisms (each implements its
   own `set_scroll_region`/`scroll_region_sequence`, its own status-row
   painter). This plan deliberately implements the pane twice rather than
   extracting a premature shared helper; flagged as Phase 4 stretch, not a
   blocker.
8. **Untested event-emission and aggregation path.** Addressed head-on as
   Phase 0.
9. **Config default vs. `auto` render mode.** See Open Questions — `auto`
   never resolves to `region`/`viewport` (`stream_renderer.cpp:867-871`),
   so the pane cannot activate unless the user (or their config's
   `[display] render`) explicitly picks one of those two modes, regardless
   of the new toggle's value.

## Open questions

- Should `[display] subagent_pane = true` have any effect on `auto`'s TTY
  resolution (currently `markdown` on a TTY, `raw` otherwise — never
  `region`/`viewport`), or is it acceptable that the pane is simply inert
  unless the user has separately opted into `--render region`/`--render
  viewport`? Leaning toward "inert is fine, document it" — changing
  `auto`'s resolution is a much larger, unrelated decision.
- Should the `raw`/`markdown` ambient-line fallback be gated by the *same*
  new `[display] subagent_pane` toggle, or keep its current
  unconditional-when-interactive-TTY behavior (`main.cpp:2229`'s existing
  condition, independent of any new config key)? Leaning toward gating it
  by the same toggle for a single consistent on/off switch, but this
  changes currently-shipping (if uncommitted) behavior and should be
  confirmed before Phase 3.
- Exact pane row cap (`kSubagentPaneRows`) and icon/color palette —
  implementation-time detail, not architecturally load-bearing; start at
  4-6 rows matching `kComposerRows`'s existing scale.
- Whether `print_mode` combined with an explicit `--render region`/`--render
  viewport` (a real, reachable combination since `--render` is independent
  of `-p`) should show a one-shot pane for the single turn it runs, or stay
  excluded like today's `activity` gate does. Leaning toward staying
  excluded — `print_mode` returns immediately after one turn
  (`main.cpp:2313-2314`), so the value is marginal and the gating is
  already proven safe as-is.

## Critical files for implementation

- `src/core/subagent_activity.h`
- `src/core/subagent_activity.cpp`
- `src/core/region_renderer.cpp`
- `src/core/stream_renderer.cpp`
- `src/core/stream_renderer.h`
- `src/main.cpp`
- `src/cli/config.cpp`
