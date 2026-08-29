# New Renderer: 60fps Region Compositor for Concurrent Tool Output

> **Revision note:** this plan was reviewed (Opus) against the actual code
> before handoff. The review caught several correctness/safety gaps in the
> original design — they're folded into the sections below (P0 added,
> threading model corrected, diff/paint/shutdown details tightened). If you
> are implementing this, read the whole document; do not skip to the code
> sketches.

## Goal

Add a new `Renderer` implementation (`RegionRenderer`, registry name `"region"`)
that:

1. Paints at a bounded, throttled frame rate (~60fps / 16ms budget) instead of
   doing a full synchronous repaint on every streamed token.
2. Gives each concurrently-running tool call a stable, non-interleaved output
   area ("region") keyed by `tool_call_id`, so parallel tool output lines up
   instead of scrambling into `std::cout` in whatever order it happens to
   arrive.

This is a **new renderer alongside the existing ones** (`raw`, `markdown`,
`viewport`), not a rewrite of `ViewportRenderer`. It is opt-in via
`--render region` (same mechanism `viewport` already uses) until it's proven
out; do not change `make_auto_renderer`'s default in this work.

## Execution milestones/status

- [x] M1 (P0): extract shared `AltScreenSession` and signal handling.
- [x] M2 (P1-P3): extend the renderer interface, route tool ownership, and
  expose shared terminal/markdown helpers.
- [x] M3: scaffold `RegionRenderer` and implement transcript paint/diff loop.
- [x] M4: add tool regions and support concurrent updates.
- [x] M5: finish scrolling, status/error handling, hardening, manual checks,
  and documentation. Add the markdown performance cache only if profiling
  measures a need.

### Final M5 acceptance notes

- `RegionRenderer` clamps line/page/top/bottom scrolling in wrapped physical
  rows, including saturating upward scroll requests before the next frame
  computes the real document extent.
- Status, token usage, command output, and errors are rendered inside the
  compositor. The renderer owns the status row, while `VerboseRenderer`
  forwards owned errors and suppresses duplicate stderr usage/error output.
- Paints are restricted to active turns. Idle status, scroll, command, and
  error updates synchronously save and restore the readline cursor; the paint
  mutex serializes those writes with the background frame loop.
- Resize invalidates the row diff cache and reapplies the scroll region.
  Short/failed writes invalidate the cache so the next frame repaints fully;
  paint exceptions are contained and renderer teardown joins the paint thread
  before leaving the alternate screen.
- Completed transcript blocks persist across turns. A new turn resets only
  transient state (active-tool lookup, thinking, status, usage, and scroll),
  hides the readline cursor, and diffs against the existing frame; it does not
  erase the alternate screen. Tail-following and history navigation use
  DECSTBM-scoped scroll operations before painting newly exposed rows.
- Deterministic unit coverage exercises scrolling, SGR continuation rows,
  status/command output, idle save/restore painting, diffing, and teardown.
  No markdown performance cache was added because no profiling evidence
  showed it was needed for the bounded 16ms paint loop.
- Manual smoke coverage used a pipe-backed renderer with a synthetic
  multi-paragraph command transcript, idle scroll/status/error updates, and
  destruction; no live-model interactive PTY or Ctrl-C run was available in
  this pass.

## Why not just patch `ViewportRenderer`

`ViewportRenderer` (`src/core/stream_renderer.cpp:296-777`) already does a
full `\033[H\033[J` + rewrite of the whole content region on every
`on_text_delta` (`repaint()`, lines 498-605), has no per-tool-call content
model (just a one-line `active_tools_` status summary at line 765), and
assumes single-threaded calls despite tool execution being genuinely
concurrent (`Renderer` doc comment at `stream_renderer.h:38-40` says
implementations must marshal to a UI thread themselves; `ViewportRenderer`
doesn't). Retrofitting a paint-loop + region model onto that class means
fighting its existing full-repaint design at every step. A fresh class
sharing its *helpers* (not its `repaint()` logic) is cleaner and keeps the
existing renderers untouched and low-risk.

## Prerequisite fixes (do these first, independently useful)

These are small, mechanical, and unblock the rest of the work — do them as
an early commit/PR before building `RegionRenderer` itself.

### P0. Extract shared alt-screen/signal-handling session (mandatory, not optional)

`ViewportRenderer` owns global statics for terminal restoration:
`current_`, `atexit_registered_`, `sigint_pending_`
(`stream_renderer.cpp:761, 775-776`), a `sig_handler` installed via
`sigaction` for `SIGINT`/`SIGTERM`/`SIGHUP` (`enter()`, lines 438-457), and
the free functions `notify_sigint()`/`consume_sigint()`
(`stream_renderer.cpp:783-787`) that are hardwired to
`ViewportRenderer::note_sigint`/`take_sigint`. `run_turn`'s
`interrupt_watcher` thread (`main.cpp:670-678`) polls
`core::consume_sigint()` to trigger `session.agent().interrupt(...)`.

**If `RegionRenderer` gets its own copy of this machinery in its own `.cpp`,
Ctrl-C will silently stop working under it** — `RegionRenderer`'s sigint
flag would never be read by `run_turn`, which only ever checks the
`ViewportRenderer`-backed global. This is not cosmetic; it's a hard
correctness requirement for a full-screen renderer (the user has no other
way to interrupt a runaway turn while the alt screen is up).

Before building `RegionRenderer`, extract a shared session type — e.g.
`AltScreenSession` in `terminal.h`/`terminal.cpp` — that owns: alt-screen
enter/leave, `SIGINT`/`SIGTERM`/`SIGHUP` install + restore, the
async-signal-safe `restore_terminal()` path, and a *single* global
sigint-pending flag with `notify_sigint()`/`consume_sigint()` reading from
it regardless of which renderer is active. Both `ViewportRenderer` and
`RegionRenderer` hold one as a member and delegate to it. Update
`ViewportRenderer` to use the extracted type in the same change, so there is
exactly one sigint flag in the process, not two racing ones.

This changes the earlier "treat as optional cleanup" framing — with two
full-screen renderers in the codebase, duplicating signal-handling statics
is the kind of bug that only shows up when a user actually hits Ctrl-C in
the field. Do this extraction first, and make "Ctrl-C restores the terminal
and interrupts the turn cleanly under `--render region`" an explicit
acceptance check in phase 2 below, not just phase 5's manual pass.

### P1. Add `on_tool_update` to the `Renderer` interface

`ToolExecutionUpdateEvent` (`src/core/event_types.h:226-238`) already flows
end-to-end: `BashTool::execute` reports partial output via
`ToolExecutionContext.on_update` (`src/core/builtin_tools.cpp:1108,1219-1220`),
which `execute_tool_safely` turns into `ToolExecutionUpdateEvent`
(`src/core/agent_loop.cpp:396-404`). But `Renderer` has no method for it and
`dispatch_event` (`src/core/stream_renderer.cpp:789-859`) has no `case` for
`ToolExecutionUpdateEvent` — it is silently dropped.

- In `src/core/stream_renderer.h`, add to `Renderer` (near `on_tool_start`/
  `on_tool_end`, line 60-64):
  ```cpp
  virtual void on_tool_update(std::string_view call_id,
                              std::string_view tool_name,
                              std::string_view partial_result) {}
  ```
- In `dispatch_event` (`stream_renderer.cpp:789-859`), add a branch:
  ```cpp
  } else if constexpr (std::is_same_v<T, ToolExecutionUpdateEvent>) {
    r.on_tool_update(e.tool_call_id, e.tool_name, e.partial_result);
  }
  ```
- Forward it in `VerboseRenderer` (`src/main.cpp:497-662`): add an
  `on_tool_update` override that just calls `base_.on_tool_update(...)`
  (no `std::cout` side effect — existing renderers ignore it via the
  default no-op, so this is a no-behavior-change addition for them).

### P2. Give the `Renderer` interface a "owns tool output" capability flag

`VerboseRenderer::on_tool_start`/`on_tool_end` (`src/main.cpp:531-609`)
unconditionally print tool-call/tool-result text straight to `std::cout`
(lines 565-567, 606-608), *regardless of which renderer is active*. This
already garbles `ViewportRenderer`'s alt-screen output today (tool text
lands on the real screen buffer, not the alt-screen content region) — worth
fixing as its own small bug, and it's a hard prerequisite for
`RegionRenderer`, which needs tool text routed into its region model instead
of `stdout`.

- Add to `Renderer` (`stream_renderer.h`, near `owns_status_line()`):
  ```cpp
  // True if this renderer draws its own tool call/result presentation
  // (e.g. inside a region) and VerboseRenderer must not also print to
  // stdout for tool start/end/format-hook output.
  virtual bool owns_tool_output() const { return false; }

  // Custom-formatted tool text from a Lua format_tool_call/format_tool_result
  // hook, routed here instead of stdout when owns_tool_output() is true.
  virtual void on_tool_output_text(std::string_view call_id,
                                   std::string_view text) {}
  ```
- In `VerboseRenderer::on_tool_start`/`on_tool_end` (`main.cpp:531-609`),
  **do not** guard with an early `return` sprinkled before the existing
  `std::cout` paths — the current code has bookkeeping *after* the
  `std::cout` write in the hook-success branch (`pending_tool_args_.erase`
  in `on_tool_end`'s hook branch, `main.cpp:584`) and an *entirely separate
  code path* when there's no hook (`format_tool_result` never runs at all
  today unless hooks are present). An early return the moment
  `owns_tool_output()` is true would skip `pending_tool_args_` cleanup
  (slow leak of the map over a long session) and skip ever computing the
  hook-formatted text for `RegionRenderer`. Instead, restructure both
  methods to *compute* the display string first (hook-formatted text if a
  hook is registered and returns one, else the existing default-formatted
  string), run the existing bookkeeping (arg caching/erasing) unconditionally,
  and only branch on `owns_tool_output()` at the single point where the
  computed string is emitted — either `base_.on_tool_output_text(call_id, text)`
  or the existing `std::cout << ... << std::flush`.
- `ViewportRenderer::owns_tool_output()` stays `false` for now (out of scope
  to fix its tool-output routing here — it already relies on the current
  bypass-to-stdout behavior and changing that is a separate task). Only
  `RegionRenderer` returns `true`.
- Note for awareness, not required to fix in this pass:
  `VerboseRenderer::on_message_end`'s verbose usage line and `on_error`
  (`main.cpp:617-625, 637-641`) write unconditionally to `std::cerr`, which
  also punches through any alt-screen renderer's content region (it's a
  separate fd/stream from the `write(fd_, ...)` calls the renderer makes on
  `fd_`, typically stdout). This predates this plan and affects
  `ViewportRenderer` today too. If it's visually disruptive under
  `RegionRenderer` during manual testing, route it the same way as
  `on_tool_output_text` rather than leaving it on `std::cerr`.

### P3. Expose `split_lines` and `render_visible_markdown` as reusable helpers

`RegionRenderer` needs the same ANSI-aware line wrapping and markdown
rendering `ViewportRenderer` uses, to avoid duplicating ~100 lines of
wrapping logic:

- `split_lines` is currently a private `static` method on `ViewportRenderer`
  (`stream_renderer.cpp:670-709`). Move it to `terminal.h`/`terminal.cpp` as
  a free function `std::vector<std::string> split_lines(std::string_view s, int width)`
  in `pi::core` (it only uses already-public helpers: `skip_ansi_sequence`,
  `codepoint_width`, `advance_utf8`, all already declared in `terminal.h`).
  Update `ViewportRenderer::repaint()` call sites to the free function.
- `render_visible_markdown` (`stream_renderer.cpp:51-56`, currently in the
  anonymous namespace) is a 5-line wrapper around the already-public
  `render_markdown_ansi` (`core/markdown.h`). Move it out of the anonymous
  namespace into `pi::core` proper (declare in `stream_renderer.h` or a
  shared header) so `RegionRenderer` (in its own `.cpp`) can call it too.

With P3 done, `RegionRenderer` does not need to live in `stream_renderer.cpp`
— it can be a clean new file.

## New files

- `src/core/region_renderer.h` — declares `RegionRenderer` (or just the
  `make_region_renderer(int fd)` factory, matching the existing pattern where
  concrete renderer classes are file-private and only factories are exported;
  see `make_viewport_renderer` at `stream_renderer.h:116`). Prefer putting
  the factory declaration in `stream_renderer.h` next to the others and
  keeping the class itself file-local in `region_renderer.cpp`, for
  consistency with `RawStreamRenderer`/`DiffMarkdownRenderer`/`ViewportRenderer`.
- `src/core/region_renderer.cpp` — implementation.
- `test/test_region_renderer.cpp` — unit tests (see Testing section).
- Add both to `CMakeLists.txt` (mirror however `stream_renderer.cpp` /
  `test_stream_renderer.cpp`, if one exists, are wired — check current
  `CMakeLists.txt` tool/test target lists before adding).

## Core design

### Data model

Tool execution genuinely runs on multiple threads
(`execute_tool_calls_parallel`, `src/core/agent_loop.cpp:797`, `std::async`
at line 847), but **`RegionRenderer`'s `on_*` overrides are never called
concurrently with each other**. Every `emit()` call — including the ones
from tool worker threads (`on_update` lambda, `agent_loop.cpp:396-404`) —
pushes into `EventStream`'s mutex-guarded queue (`src/core/stream.h:74-139`);
a single consumer thread (the `for (const auto &ev : agent.prompt(...))`
loop in `run_turn`, `main.cpp:679-681`) pulls events one at a time via
`next()` and calls `dispatch_event` → the renderer synchronously. So the
worker threads race each other only to *enqueue*, not to call into the
renderer — `RegionRenderer`'s own state is only ever touched by that one
consumer thread, **except** for reads from the separate paint thread this
design adds. Say this explicitly in code comments when implementing, so a
future maintainer doesn't add unnecessary locking around callback-to-callback
races that can't happen — the mutex below exists solely to guard the
consumer-thread-writes vs. paint-thread-reads boundary.

A flat `transcript_raw` string with tool regions appended after it is **not
sufficient**: a turn is not "text, then tools" — it's text → tools → more
text → more tools, repeating, all within one `on_turn_start`/`on_turn_end`
pair (the assistant can call tools, see results, and keep talking before the
turn ends). A single trailing tool-region list would paint round-2 text
*above* round-1's tool output, which is exactly the "doesn't line up"
problem this renderer exists to fix. Use an ordered sequence of blocks
instead:

```cpp
struct TextBlock {
  std::string raw; // markdown source accumulated for this run of text
};

struct ToolRegion {
  std::string tool_name;
  std::string args_json;      // for the header line
  std::vector<std::string> body_lines; // wrapped, already-rendered physical lines
  bool running{true};
  bool is_error{false};
};

using Block = std::variant<TextBlock, ToolRegion>;

struct RegionState {           // all fields guarded by mutex_
  std::vector<Block> blocks;                        // append-ordered
  std::unordered_map<std::string, std::size_t> tool_index; // call_id -> index into blocks
  std::string thinking_buf;
  bool in_thinking{false};
  std::optional<std::string> custom_status_line;
  std::string status_text;
  TokenUsage last_usage;
  int scroll_offset_rows{0};
  int max_scroll_rows{0};      // written back by the paint thread each frame
  bool dirty{true};
};
```

- `on_text_delta`: if `blocks` is empty or `blocks.back()` is not a
  `TextBlock` (i.e. the last thing appended was a tool region), push a new
  `TextBlock`; otherwise append to the existing trailing `TextBlock::raw`.
- `on_tool_start`: push a new `ToolRegion` onto `blocks`, record its index in
  `tool_index[call_id]`.
- `on_tool_update`/`on_tool_end`: look up `tool_index[call_id]`, mutate that
  `ToolRegion` in place — this is what keeps a tool's live output pinned to
  its original position in `blocks` regardless of arrival order relative to
  other tools or interleaved text.

Appending blocks in call/text arrival order (not completion order) is what
makes concurrent tool output "line up": tools are always painted where their
`on_tool_start` put them, each in its own fixed block, regardless of which
one's `on_tool_update`/`on_tool_end` fires next.

### Threading: one paint thread, mutex-guarded state, dirty flag

Because callbacks are serialized on the `EventStream` consumer thread (see
above), the mutex's only job is to guard `RegionState` between that consumer
thread (writer) and the paint thread (reader) — every `on_*` override still
must only take the lock, mutate `RegionState`, set `dirty = true`, and
notify a condition variable; no terminal I/O and no markdown rendering
happens on the calling thread, both because that thread is also the one
driving the whole agent loop (blocking it stalls the agent) and to keep the
locked region cheap.

```cpp
class RegionRenderer final : public Renderer {
public:
  explicit RegionRenderer(int fd);
  ~RegionRenderer() override; // request_stop() + join() the paint thread
                              // FIRST, then restore terminal — see shutdown
                              // note below. Do not reorder.

  void on_turn_start() override;          // lock; retain blocks, reset transient state/scroll; dirty=true
  void on_text_delta(std::string_view d) override;   // lock; append/extend trailing TextBlock; dirty=true
  void on_thinking_start() override;
  void on_thinking_delta(std::string_view d) override;
  void on_thinking_end() override;
  void on_tool_start(std::string_view call_id, std::string_view name,
                     std::string_view args_json) override;
  void on_tool_update(std::string_view call_id, std::string_view name,
                      std::string_view partial_result) override;
  void on_tool_end(std::string_view call_id, std::string_view name,
                   const ToolResult &result, bool is_error) override;
  void on_message_end(const TokenUsage &u) override;
  void on_turn_end() override;   // lock; paint one final synchronous frame; dirty=false; pause paint thread
  void on_command_output(std::string_view text) override;
  void on_error(RendererErrorKind kind, std::string_view msg) override;
  void on_scroll(RendererScrollCommand command) override;
  bool owns_status_line() const override { return true; }
  void set_status_line(const std::optional<std::string> &text) override;
  bool owns_tool_output() const override { return true; }
  void on_tool_output_text(std::string_view call_id, std::string_view text) override;

private:
  void mark_dirty();                 // lock, state_.dirty = true, cv_.notify_one()
  void paint_loop(std::stop_token);  // runs on paint_thread_; only active during a turn — see below
  void render_frame(const RegionState &snapshot); // builds lines, diffs, writes; wrapped in try/catch

  int fd_;
  std::mutex mutex_;
  RegionState state_;                // guarded by mutex_
  std::condition_variable cv_;
  bool turn_active_{false};          // guarded by mutex_; gates whether paint_loop paints (see quiescing note)
  AltScreenSession alt_screen_;      // from P0 — declared before paint_thread_
  std::vector<std::string> last_frame_lines_; // paint-thread-only, no lock needed
  int last_width_{0};
  int last_height_{0};
  std::jthread paint_thread_;        // MUST be the LAST member: destroyed
                                     // (and joined, via jthread's dtor calling
                                     // request_stop()+join()) before any other
                                     // member it references is torn down
};
```

`std::jthread` gives cooperative cancellation for free (matches existing use
in `terminal.h`'s `TerminalTitleController::worker_` at `terminal.h:80` and
`run_turn`'s `interrupt_watcher` in `main.cpp:670`). Declaring it last is not
a style nit: members are destroyed in reverse declaration order, and the
paint thread reads `last_frame_lines_`/writes through `alt_screen_`'s `fd_`
on every frame — if it's declared (and thus destroyed) *before* those, the
implicit destructor sequence tears them out from under a thread that may
still be running. `~RegionRenderer()`'s body should still start with an
explicit `paint_thread_.request_stop(); if (paint_thread_.joinable())
paint_thread_.join();` before touching `alt_screen_.leave()`, rather than
relying only on member-destruction order, so the shutdown sequence is
visible at the call site.

`render_frame` (and anything it calls — markdown rendering goes through
cmark, `render_visible_markdown`/`render_markdown_ansi`, which can throw on
malformed input) must not let an exception escape the body of `paint_loop`.
An uncaught exception on a `std::jthread` calls `std::terminate` — with the
terminal left in the alternate screen and raw mode, which is a broken
terminal for the user's whole shell session. Wrap each iteration's
`render_frame` call in `try { ... } catch (...) { /* skip this frame, keep
looping */ }`.

### Paint loop: throttled, coalesced, frame-budgeted, quiesced outside a turn

Two corrections to the naive version:

1. **The throttle must gate on time, not just dirtiness.** `wait_until`
   evaluates its predicate immediately when called; if `dirty` is already
   `true` (e.g. set again while the previous `render_frame` was running),
   `wait_until` returns instantly regardless of `next_frame`, so a burst of
   updates produces back-to-back paints with no 16ms floor between them —
   the opposite of throttling. The predicate must require *both* dirty and
   time-elapsed.
2. **The paint thread must not fight readline.** `cli::readline`'s
   `InputRenderer::redraw` (`src/cli/readline.cpp:234-330`, see the class at
   127-247) positions the input row with *relative* cursor moves (`\033[A`,
   `\033[D`, `\r`) on `std::cout`, assuming nothing else is concurrently
   moving the cursor. `RegionRenderer`'s `on_scroll`/`set_status_line` can
   be called *while the user is typing at the prompt* (the scroll keys are
   wired straight from readline's key handling,
   `readline.cpp:324-354` → `main.cpp:1640-1660`). If a background paint
   thread issues absolute `CUP` (`\033[N;1H`) writes at an arbitrary moment
   relative to readline's relative moves, the two cursor models desync and
   the input line gets corrupted on screen (not just cosmetically — readline
   would then compute wrong clear ranges on its next redraw). **The paint
   thread must only run during an active turn** (`on_turn_start` through
   `on_turn_end`, when readline is not reading input). Outside a turn,
   `on_scroll`/`set_status_line`/`on_error` must paint *synchronously on the
   calling thread*, wrapped in save/restore cursor (`\0337`...`\0338`, DECSC/
   DECRC) so it can't permanently perturb whatever cursor position readline
   left behind.

```cpp
void RegionRenderer::paint_loop(std::stop_token st) {
  using namespace std::chrono_literals;
  constexpr auto kFrameBudget = 16ms; // ~60fps
  auto next_frame = std::chrono::steady_clock::now() + kFrameBudget;
  while (!st.stop_requested()) {
    RegionState snapshot;
    {
      std::unique_lock lock(mutex_);
      cv_.wait_until(lock, next_frame, [&] {
        return st.stop_requested() ||
               (turn_active_ && state_.dirty &&
                std::chrono::steady_clock::now() >= next_frame);
      });
      if (st.stop_requested())
        return;
      if (!turn_active_ || !state_.dirty) {
        // Not time yet, or paused between turns: re-wait without a spin.
        // wait_until with no deadline change is fine; the predicate above
        // already re-checks time on every spurious wakeup.
        continue;
      }
      state_.dirty = false;
      snapshot = state_; // one copy per frame, off the hot callback path
    }
    try {
      render_frame(snapshot);
    } catch (...) {
      // Drop this frame; do not let a render exception kill the thread
      // with the terminal stuck in the alternate screen.
    }
    next_frame = std::chrono::steady_clock::now() + kFrameBudget;
  }
}
```

`on_turn_start` sets `turn_active_ = true` under the lock and notifies;
`on_turn_end` paints one last synchronous frame (so the final state of the
turn — e.g. a tool result that arrived and was coalesced away without ever
being painted — is guaranteed visible), then sets `turn_active_ = false`
under the lock. Any number of `on_text_delta`/`on_tool_update` calls
arriving within one 16ms window while a turn is active collapse into a
single repaint — this is the coalescing that fixes the "one syscall + one
full markdown re-render per token" problem in `ViewportRenderer::repaint()`.

### Frame construction: ordered blocks → physical lines

`build_frame_lines(state, width, content_rows)` — a free function or static
method, deliberately separate from any fd/terminal access so it's unit
testable (see Testing) — walks `state.blocks` in order and produces one flat
`std::vector<std::string>` of physical, already-wrapped terminal lines:

1. For each `TextBlock`: `render_visible_markdown(block.raw)` →
   `split_lines(rendered, width)`, appended to the output. Reuse the
   existing `BlockBoundaryScanner`/finalized-prefix caching idea
   (`terminal.h:156-178`, `ViewportRenderer::FinCache` at
   `stream_renderer.cpp:751-757`) if profiling shows markdown re-render is
   the bottleneck — **do not build this caching on day one**; ship the
   straightforward "re-render each `TextBlock` every frame" version first
   and only add caching if perf numbers show it's needed. Re-render is now
   bounded to once per 16ms instead of once per token, which is most of the
   win already.
2. For each `ToolRegion`: one header line (`"[tool_name] args_json"`,
   truncated to width, colored to match `VerboseRenderer`'s existing scheme
   — the `\033[38;5;214m` args color and `\033[38;5;245m` result color at
   `main.cpp:565-567,606-608`) followed by up to `kMaxToolBodyLines` (start
   with 4) body lines. Use `core::truncate_tool_result`
   (`terminal.h:135-138`, already public) on `partial_result`/final result
   content to get the "first 2 / last 2 + fold marker" body — this reuses
   the exact truncation semantics users already see in non-region
   renderers, so behavior is familiar.
3. **Every emitted line must be self-contained w.r.t. SGR (color) state** —
   see the SGR note below; this is a correctness requirement of the diff
   step, not a formatting nicety.
4. **Degenerate sizing.** If `content_rows < 1`, paint nothing (matches
   `ViewportRenderer::repaint()`'s early return at `stream_renderer.cpp:502-503`
   and `set_scroll_region()`'s `h < 3` guard at `stream_renderer.cpp:490-491`).
   If a single `ToolRegion`'s header line alone doesn't fit in
   `content_rows` (a very short/narrow terminal, or many concurrent tools
   each claiming their minimum), skip rendering that region's body entirely
   and show only as much of the header as fits — never let one region's
   layout push subsequent blocks off using more than their fair share.
   Cap the number of *concurrently expanded* tool regions rendered with a
   body at a fixed number (start with 6): once a turn has more than that
   many tools running/recently-finished, collapse the oldest running/
   finished ones in `blocks` to a single one-line summary
   (`"[name] done"`/`"[name] running…"`) so a burst of parallel tool calls
   can't evict the transcript text entirely. This mirrors why
   `active_tools_` in `ViewportRenderer` is already just a name summary
   (`stream_renderer.cpp:765`) rather than full output — full-fidelity
   concurrent output for an unbounded number of regions isn't a coherent UI
   regardless of rendering approach.
5. If total lines > `content_rows` (`term_height(fd) - 2`, same row budget
   as `ViewportRenderer` — rows 1..h-2 content, h-1 status, h reserved for
   readline, per the layout comment at `stream_renderer.cpp:286-294`),
   apply `scroll_offset_rows` exactly like
   `ViewportRenderer::on_scroll`/`repaint()` do (`stream_renderer.cpp:336-362,
   589-592`) — tail-anchored by default, scrollable via the same
   `RendererScrollCommand`s. `build_frame_lines` should also return the
   computed `max_scroll_rows` for that frame (total lines minus
   `content_rows`, clamped to ≥0); `render_frame` writes it back into
   `state_.max_scroll_rows`/clamps `state_.scroll_offset_rows` under the
   lock after computing it, since the *only* place that knows the real
   document length is frame construction — `on_scroll` itself only has the
   previous frame's cached max to clamp against otherwise, which would lag
   by one frame after a burst of new content changes the document length.

### Diff-based paint: only touch changed rows

```cpp
void RegionRenderer::render_frame(const RegionState &snapshot) {
  const int w = term_width(fd_);
  const int h = term_height(fd_);
  const int content_rows = std::max(0, h - 2);
  if (content_rows == 0) return;

  if (w != last_width_ || h != last_height_) {
    // Resized since the last frame: old row contents are meaningless,
    // and DECSTBM must be re-issued for the new height (only set once
    // today, in on_turn_start — see resize note below).
    last_frame_lines_.clear();
    last_width_ = w;
    last_height_ = h;
    set_scroll_region(fd_, h); // re-issue \033[1;{h-2}r
  }

  auto [lines, max_scroll] = build_frame_lines(snapshot, w, content_rows);
  lines.resize(content_rows); // pad with "" for rows that must be cleared
  {
    std::scoped_lock lock(mutex_);
    state_.max_scroll_rows = max_scroll;
    state_.scroll_offset_rows =
        std::clamp(state_.scroll_offset_rows, 0, max_scroll);
  }

  std::string out;
  for (int i = 0; i < content_rows; ++i) {
    if (i >= static_cast<int>(last_frame_lines_.size()) ||
        lines[i] != last_frame_lines_[i]) {
      out += "\033[" + std::to_string(i + 1) + ";1H\033[2K" + lines[i];
    }
  }
  if (!out.empty() && !write_all(fd_, out)) {
    // Partial/failed write: last_frame_lines_ would otherwise claim rows
    // are on-screen that never made it out. Force a full repaint next
    // frame rather than accumulating silent corruption.
    last_frame_lines_.clear();
    return;
  }
  last_frame_lines_ = std::move(lines);
  paint_status(snapshot); // separate small write, same status-bar approach
                          // as ViewportRenderer::paint_status (stream_renderer.cpp:607-665)
}
```

`write_all` is a small helper that loops on `::write`, retrying on `EINTR`
and short writes, returning `false` only on a hard error — `::write` on a
terminal fd is not guaranteed to consume the whole buffer in one call, and
the existing code's bare `::write(fd_, frame.data(), frame.size())` calls
(e.g. `stream_renderer.cpp:603`) already carry this same latent bug; don't
copy it forward into new code that specifically depends on
`last_frame_lines_` accurately reflecting what's on screen — under the old
full-clear-every-time design a short write self-heals on the next repaint,
but the diff design's whole premise is that `last_frame_lines_` is a
truthful record of on-screen state, so a partial write must be treated as a
cache-invalidation event, not ignored.

**SGR (color) state must not leak across diffed rows.** `split_lines`
(`stream_renderer.cpp:670-709`, moving to `terminal.h` per P3) passes ANSI
escapes through opaquely without tracking which SGR attributes are active at
each wrap point. Under the old full-repaint design this didn't matter — the
whole visible region was rewritten together every time, so a color set on
row N naturally carried into row N+1's terminal state exactly as intended.
Under row-level diffing, if only row N+1 changes and gets rewritten in
isolation, it starts from *whatever SGR state row N+1's cursor position
already had from the terminal's perspective* — which may now be stale or
wrong, and a color begun on row N with no closing `\033[0m` before the wrap
will not carry into a row N+1 that's rewritten alone later. Two required
fixes, both in `build_frame_lines`: (1) every emitted line must end with
`\033[0m` so it never leaves attributes bleeding into whatever is painted
next; (2) if a color run spans a wrap boundary (e.g. a long colored tool
result body line wrapping across N physical rows), each continuation row
must re-open the same SGR sequence at its start, not assume it's inherited.
This likely means `split_lines`/its caller needs to track "SGR state active
at the start of each output line" as it walks the source text, not just
byte-split on width. Add a unit test: a single logical line long enough to
wrap twice, entirely wrapped in one color, diffed such that only the middle
physical row changes — assert the middle row alone still renders in that
color.

### Wiring into the app

- `stream_renderer.h`: add `std::unique_ptr<Renderer> make_region_renderer(int fd = 1);`
  next to `make_viewport_renderer` (line 116).
- `stream_renderer.cpp` (or wherever the registry lives after P3): register
  `factories_["region"] = [](int fd) { return make_region_renderer(fd); };`
  in `StreamRendererRegistry::StreamRendererRegistry()` (`stream_renderer.cpp:880-885`).
  Do **not** add it to `make_auto_renderer` (`stream_renderer.cpp:873-878`) —
  stays opt-in via `--render region` (`cli::Args::render`, consumed in
  `make_renderer`, `main.cpp:478-489`).

## Phased task breakdown

1. **P0–P3 prerequisite fixes** (shared alt-screen/sigint session + interface
   additions + bug fix + helper extraction). Small, mechanical, testable
   independently, but do P0 before P2 — `RegionRenderer`'s constructor needs
   `AltScreenSession` to exist. Run `make test` after each.
2. **Scaffold `RegionRenderer`**: constructor/destructor using the shared
   `AltScreenSession` from P0, empty paint loop, registered as `"region"`.
   **Acceptance check for this phase, not deferred to later manual
   testing:** `pi --render region` enters/exits the alt screen cleanly, and
   Ctrl-C during a turn under `--render region` both restores the terminal
   *and* interrupts the in-flight turn (i.e. `core::consume_sigint()` is
   actually observed by `run_turn`'s `interrupt_watcher`,
   `main.cpp:670-678`) — this is the one thing P0 exists to guarantee and is
   easy to silently break if the extraction isn't done carefully.
3. **Transcript-only rendering**: wire `on_text_delta`/`on_thinking_*`/
   `on_turn_start`/`on_turn_end` to the paint loop with the diff-based
   `render_frame`, no tool regions yet. Should behave like a throttled
   `ViewportRenderer` for plain text streaming. Manually verify smoothness
   with a fast-streaming model or a synthetic delta generator.
4. **Tool regions**: wire `on_tool_start`/`on_tool_update`/`on_tool_end` to
   build `ToolRegion` blocks in `state.blocks`, keyed via `tool_index`, per
   the data model above. Manually verify with a
   multi-tool-call turn (e.g. ask the agent to run several `bash` commands
   in one turn that the model batches as parallel tool calls) that each
   tool's output stays in its own block and updates in place.
5. **Scrolling + status bar**: wire `on_scroll`/`set_status_line`/
   `on_error` to match `ViewportRenderer`'s behavior.
6. **Perf pass**: only if needed after manual testing — add the
   finalized-prefix markdown cache (mirroring `FinCache`) if transcript
   re-render is measurably the bottleneck at typical content lengths.

## Testing

- `test/test_region_renderer.cpp`: unit-test `RegionRenderer`'s pure logic
  without a real terminal where possible — e.g. extract `build_frame_lines`
  (frame construction from `RegionState`) as a free function or static
  method that takes `(state, width, content_rows)` and returns
  `vector<string>`, so it's testable without spawning the paint thread or
  writing to an fd. Cover:
  - Multiple concurrent tool regions stay in `call_order`, not
    completion order.
  - A tool's `on_tool_update` mutates only its own region's body lines.
  - Transcript + N tool regions exceeding `content_rows` triggers
    tail-anchored truncation / scroll clamping.
  - A color run spanning a wrapped line still renders correctly on the
    diffed row that didn't include the run's start (the SGR-leak case
    above).
  - Degenerate sizing: `content_rows < 1` paints nothing; a region whose
    header alone doesn't fit doesn't corrupt subsequent blocks; more than
    the concurrent-region cap collapses the oldest regions to one-line
    summaries.
  - Diff logic: given two consecutive `vector<string>` frames, only changed
    indices produce escape sequences (test the diff step directly, not via
    a real fd).
  - There is no existing `test/test_stream_renderer.cpp` to mirror — check
    `test/test_terminal.cpp` for the existing `split_lines`-adjacent
    helpers' test structure/fixtures instead (`split_lines` itself is
    currently untested; add coverage for it as part of the P3 move into
    `terminal.h`/`terminal.cpp`).
- Add a `test-region-renderer` target following the pattern used by the
  other `test-*` targets already declared in `CMakeLists.txt` (grep it for
  the existing `add_executable(test-...)`/`ctest` block before adding).
- Manual verification (this is a terminal-rendering feature — type checking
  won't catch visual regressions): run `pi --render region` interactively,
  confirm status line shows, confirm Ctrl-C restores the terminal cleanly,
  confirm resizing the terminal doesn't leave stale garbage rows.

## Explicit non-goals for this pass

- Not changing `make_auto_renderer`'s default renderer.
- Not adding side-by-side/column layout for tool regions — this plan is
  vertically-stacked ordered blocks, not a true multi-pane grid. Column
  layout is a plausible follow-up once vertical regions are proven out, but
  adds real width-budgeting complexity (splitting `term_width` across N
  concurrent tools) that isn't justified until vertical stacking is shipped
  and evaluated.
- Not fixing `ViewportRenderer`'s existing tool-output-bypasses-stdout bug
  beyond what P2 requires (`owns_tool_output()` defaults to `false` for it).
- Not building the `FinCache`-style markdown caching up front — see the
  perf-pass note in step 6.
