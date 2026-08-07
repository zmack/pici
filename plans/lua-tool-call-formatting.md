# Customizable tool-call formatting via Lua

## Context

No existing plan covers this. `plans/lua-context-access.md` is about giving
Lua hooks *read* access to conversation context (raw/effective message
history) — unrelated to how tool calls are *displayed*. This is a new,
separate, much smaller surface.

### How tool-call text is actually produced today

There is exactly one place that renders the human-readable
`[tool: name(args)]` / `  [name] -> result` text blocks seen in a normal CLI
session: `VerboseRenderer` (`src/main.cpp:420-469`), which unconditionally
wraps whatever base renderer `make_renderer()` selected (`main.cpp:526`,
`VerboseRenderer vr(renderer, verbose, std::move(diagnostics));`). Despite the
class name, its `on_tool_start`/`on_tool_end` overrides print this
unconditionally — `verbose_` is not checked in either method — so this is not
an opt-in `--verbose` feature, it is the default tool-call display for every
interactive CLI session:

```cpp
// on_tool_start (main.cpp:452-460)
std::cout << "\n[tool: " << name << "(" << "\033[38;5;214m" << args_json
          << "\033[0m" << ")]\n" << std::flush;

// on_tool_end (main.cpp:461-469)
std::cout << "\033[38;5;245m" << "  [" << name << "] "
          << format_tool_result(result.content()) << "\033[0m\n" << std::flush;
```

`format_tool_result` (`main.cpp:377-416`) does the line-count truncation:
strips trailing newlines, and if there are more than 5 lines, keeps the first
2 and last 2 with a `"… +N lines omitted"` marker in between.

Verified during planning — the other three renderer implementations do **not**
independently format tool calls as text:

- `RawStreamRenderer` (`stream_renderer.cpp:84-105`) has no
  `on_tool_start`/`on_tool_end` override at all (inherits `Renderer`'s
  no-op default).
- `DiffMarkdownRenderer` (`stream_renderer.cpp:125-296`) likewise has no
  override.
- `ViewportRenderer` (`stream_renderer.cpp:296-...`) overrides both
  (`stream_renderer.cpp:394-404`), but only to track an `active_tools_` map
  for its own status-line indicator (`paint_status()`) — it does not print a
  detailed call/result block. This is a different, already-Lua-customizable
  concern (the existing `status_line` hook can already override whatever a
  renderer puts on that line).

So `VerboseRenderer` is the single choke point for exactly the feature being
asked for. `AcpSseRenderer` and `SyncRenderer` (`src/acp/handlers.cpp:39,175`)
are structured JSON/SSE wire formats for programmatic consumers, not
human-facing text — out of scope, see Non-goals.

### Existing Lua hook conventions to follow, not invent

`LuaHooks` (`src/core/lua_tool.h:76-227`) already has two families of hook
with exactly the semantics this feature needs, and the implementation must
reuse their patterns rather than inventing new ones:

- **Lifecycle-paired hooks** (`before_tool_call`/`after_tool_call`,
  `lua_tool.h:84-90`): separate start/end callbacks keyed by tool call
  lifecycle. `call_before`/`call_after` (`lua_tool.cpp:931-1004`) build a Lua
  context table with `tool_name`, `call_id`, `args` (via `json_to_lua` on the
  decoded arguments — **not** the raw JSON string), locking `mutex_` for the
  whole call since the Lua state is not reentrant/thread-safe. Composition
  (`lua_tool.cpp:1855-1870`-ish) runs every add-on's hook, first
  non-default-result wins.
- **"Return string or nil for default" UI hooks** (`status_line`/`tab_title`,
  `lua_tool.h:179-180`): `call_ui_line` (`lua_tool.cpp:847-863`) is the
  precedent to copy exactly. On a Lua error (`lua_pcall` fails), it silently
  returns `std::nullopt` — no error surfaced, no crash, caller falls back to
  its own default. Composition is "last non-nil wins"
  (`lua_tool.cpp:1987` comment).

The new hooks are structurally identical to the second family (formatting is
inherently a single-winner, string-or-default concern — there's nothing to
"merge" between two different renderings), just fired from a different call
site (`VerboseRenderer` instead of the readline-prompt loop).

## Executive design decision

Add two new optional hooks to `LuaHooks`, named to mirror the existing C++
concepts they replace (`format_tool_result` already exists as a free function
in `main.cpp`) and to avoid any confusion with `before_tool_call`/
`after_tool_call` (which control *execution* — blocking calls, rewriting
results — not *display*):

```lua
format_tool_call(ctx) → string | nil
  -- ctx: {tool_name, call_id, args}
  -- Called when a tool call starts streaming in. Return the full text block
  -- to print (including any ANSI codes and trailing newline handling); nil
  -- falls back to today's built-in "[tool: name(args)]" formatting.

format_tool_result(ctx) → string | nil
  -- ctx: {tool_name, call_id, args, content, is_error}
  -- Called when a tool call's result is available. `content` is the full,
  -- untruncated result text — Lua owns any truncation/formatting decision
  -- itself; nil falls back to today's built-in truncated "  [name] -> ..."
  -- formatting (including the 5-line/first-2-last-2 truncation).
```

`ctx.args` is the decoded arguments table (via `json_to_lua`), matching
`before_tool_call`/`after_tool_call`'s `ctx.args` shape exactly — not the raw
JSON string `VerboseRenderer` currently receives. This lets a hook branch on
`ctx.args.command` for the `bash` tool, `ctx.args.file_path` for `read`, etc.,
the same way existing hooks already do, rather than requiring every add-on to
re-parse JSON.

No `turn` field, unlike `before_tool_call`'s context: that hook derives it
from `count_turns(ctx.context.messages)` (`lua_tool.cpp:945`) using an
`AgentContext` the renderer's call site does not have access to
(`Renderer::on_tool_start`/`on_tool_end`, `stream_renderer.h:60-64`, take
only `call_id`/`tool_name`/`args_json`/`ToolResult`/`is_error` — no
context). Not worth plumbing a counter through for this.

`format_tool_result`'s `ctx.args` has a real wrinkle: `Renderer::on_tool_end`
(`stream_renderer.h:63`) is not handed the original arguments at all, only
`call_id`/`tool_name`/`ToolResult`/`is_error` — confirmed at the
`dispatch_event` call site (`stream_renderer.cpp:842-843`,
`r.on_tool_end(e.tool_call_id, e.tool_name, *e.result, e.is_error)`, no
`args`). `VerboseRenderer` must cache parsed args by `call_id` in
`on_tool_start` and look them up (then erase) in `on_tool_end`. Tool calls
can interleave under parallel execution (`stream_renderer.h:57-58`'s own
comment on why `call_id` exists), so this must be a real
`call_id → args` map, not a single last-seen value.

Composition rule: **last non-nil wins**, identical to `status_line`/
`tab_title`. Document this explicitly in the `compose_hooks` doc comment
alongside the existing composition table (`lua_tool.h:267-273`).

Error handling: identical to `call_ui_line` — a Lua error in either hook is
swallowed, the call falls back to the built-in formatting, nothing crashes or
blocks tool execution. This is a deliberate divergence from
`before_tool_call`'s error behavior (which surfaces the error as a blocking
reason) — a broken formatter must never break the agent loop, since it's a
pure display concern.

## Implementation

### `src/core/lua_tool.h`

- Add `format_tool_call` and `format_tool_result` fields to `LuaHooks`,
  typed like `status_line`/`tab_title` but taking a context struct instead of
  `LuaUiContext` (new small structs, or reuse `BeforeToolCallContext`/
  `AfterToolCallContext` if their shape already matches closely enough —
  check before adding new ones; `AfterToolCallContext` already carries
  `tool_call`/`result`/`is_error`, which may be sufficient for
  `format_tool_result` directly).
- Update the doc comment block (`lua_tool.h:46-74`) with the two new hook
  signatures, matching the existing terse per-hook documentation style.
- Update `compose_hooks`'s doc comment (`lua_tool.h:267-273`) to list both
  new hooks under "last non-nil wins."

### `src/core/lua_tool.cpp`

- `LuaHooksImpl` (or whatever the concrete loader class is named around
  `call_before`/`call_status_line`): add `format_tool_call_ref_`/
  `format_tool_result_ref_`, extracted the same way as
  `before_ref_`/`status_line_ref_` (`lua_tool.cpp:680,688`).
- Add `call_format_tool_call`/`call_format_tool_result`, copying
  `call_ui_line`'s exact shape (lock `mutex_`, push context table via
  `json_to_lua`-style field construction like `call_before` does, `lua_pcall`,
  swallow errors to `std::nullopt`, extract only if the return value is a
  string).
- Wire both into `compose_hooks` under the "last non-nil wins" group
  alongside `status_line`/`tab_title` (`lua_tool.cpp:1987-2000` area).

### `src/main.cpp`

- **Not a plain `shared_ptr` capture.** `VerboseRenderer vr(...)` is
  constructed inside the free function `run_turn` (`main.cpp:523-526`), not
  `main()`, and the live hooks object is not a stable local — it lives in
  `HookRuntime{std::mutex mutex; std::shared_ptr<core::LuaHooks> hooks;}`
  (`main.cpp:71-74`) precisely because `/reload`-style commands replace
  `hooks` under `mutex` at every other call site (`main.cpp:1185-1192` and
  the `HookRuntime`-guarded reads elsewhere). `run_turn` must take a
  `HookRuntime &` (or equivalent shared handle) and `VerboseRenderer` must
  snapshot `std::shared_ptr<core::LuaHooks>` under `mutex` **once per
  tool-call event**, not once at construction — otherwise a mid-session
  `/reload` never takes effect for tool-call formatting until the next
  process restart, while every other hook already picks it up immediately.
- Add a `std::unordered_map<std::string, nlohmann::json> pending_tool_args_`
  member to `VerboseRenderer` to bridge the gap described above: populate it
  in `on_tool_start` (keyed by `call_id`), read + erase it in `on_tool_end`.
  If a `call_id` is somehow missing at `on_tool_end` (should not happen, but
  don't crash on it), pass an empty table to `format_tool_result`.
- Args parsing: `on_tool_start`/`on_tool_end` receive `args_json` as a raw
  string (`Renderer::on_tool_start`'s `args_json` parameter,
  `stream_renderer.h:60-62`). Parse it with
  `nlohmann::json::parse(args_json, nullptr, false)` (the four-argument,
  non-throwing form — malformed model-generated arguments must never throw
  inside a renderer callback) and check `.is_discarded()` before use; pass
  the resulting `nlohmann::json` into the new context struct. `lua_tool.cpp`
  then calls the existing file-static `json_to_lua` (`lua_tool.cpp:43`) on
  it — the same function `call_before` already uses on
  `ctx.tool_call.arguments` (`lua_tool.cpp:942`). Do not hand-roll a second
  JSON→Lua conversion.
- In `on_tool_start`: if a hook is configured, call it and print the
  returned string **after sanitizing it** (see Sanitization below); nil
  falls through to the existing hardcoded print.
- In `on_tool_end`: same shape for `format_tool_result`, passing the *full*
  `result.content()` (not pre-truncated) so Lua can make its own truncation
  decision; fall through to the existing `format_tool_result(...)` call if
  the hook returns nil.
- Keep `base_.on_tool_start(...)`/`base_.on_tool_end(...)` forwarding
  unconditional and first, exactly as today — the underlying renderer (e.g.
  `ViewportRenderer`'s active-tool tracking) must keep working regardless of
  whether a Lua hook customizes the printed text.

### Sanitization (required, not optional)

A Lua-returned string is about to be written directly to `std::cout` and,
under `ViewportRenderer`, into an alternate-screen viewport that repaints
itself via cursor-position control sequences
(`stream_renderer.cpp:296-330`-ish). Every other Lua-sourced string that
reaches the terminal already goes through a sanitizer for exactly this
reason: `status_line` output is clipped via `truncate_ansi_line`
(`terminal.cpp`), and `tab_title` output goes through
`sanitize_terminal_title` — the same function fixed in commit `6ecda63` for
letting an invalid UTF-8 byte through unstripped. A hook-returned tool-call
string needs the equivalent treatment before printing: strip invalid UTF-8
(reuse the fixed logic's approach, not a third reimplementation) and
disallow cursor-movement/screen-clearing control sequences while still
permitting SGR (color) codes, since the whole point of this feature is
letting hooks colorize output. Add this as its own small helper rather than
inlining it into `VerboseRenderer` — it's a second consumer of "sanitize
before terminal write" and should not duplicate `sanitize_terminal_title`'s
byte-handling bug class.

### Truncation reuse

Expose the existing line-truncation behavior (`format_tool_result`,
`main.cpp:377-416`) to Lua as a callable, e.g. `pici.truncate_tool_result(content)`,
so an add-on that only wants different colors — not different truncation
— can call the built-in truncation and wrap the result, instead of
reimplementing the 5-line/first-2-last-2 rule from scratch.

### Threading

`on_tool_start`/`on_tool_end` fire on whatever thread drives the
`EventStream` iterator — normally the agent's worker thread
(`Renderer`'s own documented contract, `stream_renderer.h:36-39`; confirmed
call site `stream_renderer.cpp:839-843`). The new hook calls take the same
Lua `mutex_` that `before_tool_call`/`after_tool_call` already take
(`lua_tool.cpp:933,973`) — there is no reentrancy/deadlock risk since
formatting hooks don't nest inside execution hooks, but a slow or
misbehaving `format_tool_call`/`format_tool_result` implementation will
stall the whole turn's streaming output for as long as it runs, exactly
like a slow `before_tool_call` already can. Document this in the hook's
doc comment; do not add a timeout in this pass (none of the existing
formatting hooks have one either).

## Non-goals

- ACP (`AcpSseRenderer`) and RPC (`SyncRenderer`, and RPC mode's own output
  path) tool-call event shapes. Those are structured wire formats for
  programmatic clients — "formatting" doesn't apply, and changing them would
  be a protocol compatibility break, not a display customization.
- `RawStreamRenderer`/`DiffMarkdownRenderer`/`ViewportRenderer` gaining their
  own independent tool-call text rendering. They don't have any today
  (confirmed above); this plan hooks the one place that does, it doesn't add
  new rendering surfaces to the others.
- Changing `ViewportRenderer`'s status-line tool-activity indicator
  (`active_tools_`/`paint_status()`). Already covered by the existing
  `status_line` hook if a user wants to customize it; conflating the two
  would duplicate an existing customization point.
- Streaming/incremental formatting (e.g. a hook called per output chunk
  while a long-running tool is still executing). `format_tool_call` fires
  once at start, `format_tool_result` fires once when the result is fully
  available — matching `on_tool_start`/`on_tool_end`'s existing granularity.
  No `on_tool_progress`-style hook exists today and adding one is a separate,
  larger change.
- Giving the formatter hooks the ability to block or modify the tool call
  itself (that's what `before_tool_call`/`after_tool_call` already do — do
  not duplicate or blend those semantics into the new hooks).
- A `format_tool_call`/`format_tool_result` example add-on shipped by
  default. Document the hook; don't ship an opinionated default theme beyond
  what already exists.
- Child/subagent tool calls (`AgentTaskManager`, `src/core/agent_task.cpp`).
  `make_task` explicitly nulls every Lua hook for children, including
  `before_tool_call`/`after_tool_call` (`agent_task.cpp:412-413`, comment:
  "Children are restricted to C++ read-only tools in the in-memory MVP. This
  lets us remove Lua hooks without silently bypassing their authority.") —
  the new formatting hooks must follow the same restriction, not be added to
  the child `Agent::Options` construction. Child tool-call events reach only
  `ChildAgentEvent`/`src/acp/task_events.cpp`, never the CLI's
  `VerboseRenderer`, so there is nothing to wire up there regardless.

## Test plan

Follow the existing `test/test_lua_tool.cpp` pattern (Lua-side
`pici.test.run`/`pici.test.eq` harness per `lua_tool.h:229-244`):

- `format_tool_call` returning a string is used verbatim; returning `nil`
  falls back to default; hook absent falls back to default (no behavior
  change for add-ons that don't define it — regression coverage).
- `format_tool_result` receives the *untruncated* content (assert against
  content longer than the 5-line built-in truncation threshold) and its
  return value is used verbatim, un-truncated by pici itself.
- `ctx.args` in both hooks is a decoded table, not a JSON string — assert a
  specific field is directly indexable (e.g. `ctx.args.command` for a `bash`
  call).
- A Lua error inside either hook falls back to default formatting and does
  not raise/propagate/block the tool call — assert the tool result is still
  delivered to the model and the process doesn't crash.
- `compose_hooks` composition: two add-ons both defining `format_tool_call`
  — the later-loaded one's non-nil result wins, matching `status_line`.
- Malformed `args_json` (not valid JSON) does not throw inside
  `on_tool_start`/`on_tool_end` — assert the hook still gets called (with an
  empty/discarded-fallback args table) and the turn completes normally.
- Parallel tool calls: two overlapping `on_tool_start`/`on_tool_end` pairs
  with different `call_id`s — assert `format_tool_result`'s `ctx.args`
  matches the *correct* call's original arguments, not whichever call
  started or ended most recently (exercises the `call_id → args` cache
  directly, including that entries are erased after use rather than
  leaking).
- A hook returning a string containing an invalid UTF-8 byte or a
  cursor-movement/screen-clearing control sequence gets sanitized before
  being written — assert the printed output contains neither, while SGR
  color codes in an otherwise-clean returned string survive intact.
- `/reload` (or whatever the existing hot-reload command is) with a new
  `format_tool_call` implementation takes effect on the *next* tool call in
  the same session, without restarting the process — exercises the
  `HookRuntime`-guarded snapshot-per-call requirement, not a
  snapshot-at-construction shortcut.
- Regression: existing `before_tool_call`/`after_tool_call`/`status_line`
  tests keep passing unmodified — these hooks must not interact with the new
  ones (they operate at different call sites and control different
  concerns).

Manual smoke test: load an add-on defining `format_tool_call` that returns a
custom string for the `bash` tool only (returning `nil` for everything else),
run a session mixing `bash` and `read` calls, confirm `bash` shows the custom
format and `read` shows the unchanged default.

## Completion criteria

- `format_tool_call`/`format_tool_result` are documented in `LuaHooks`'
  doc comment and `compose_hooks`' composition table, following the existing
  style exactly.
- `VerboseRenderer` calls both hooks when present, falls back to today's
  exact output when absent or when a hook returns `nil` — byte-for-byte
  unchanged default behavior is a hard requirement, not a nice-to-have.
- A Lua error in either hook never crashes the process or blocks tool
  execution.
- `ctx.args` is a decoded Lua table in both hooks, matching
  `before_tool_call`/`after_tool_call`'s existing convention, including for
  `format_tool_result` despite `Renderer::on_tool_end` not carrying args
  natively (via the `call_id → args` cache).
- A hook's returned string is sanitized (invalid UTF-8 stripped,
  cursor/screen control sequences disallowed, SGR color preserved) before
  ever reaching `std::cout` — no unsanitized Lua-sourced string reaches the
  terminal, matching the existing `status_line`/`tab_title` precedent.
- Hot-reloading add-ons picks up a changed `format_tool_call`/
  `format_tool_result` on the next tool call in the same session — no stale
  `shared_ptr<LuaHooks>` snapshot captured once at renderer construction.
- New/updated tests per the test plan pass; full `make test` stays green;
  `make format`/`make lint` clean on touched code.
