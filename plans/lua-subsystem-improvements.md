# Lua Add-on Subsystem: State & Extensibility Review

## Current State

The Lua subsystem (`src/core/lua_tool.h` / `lua_tool.cpp`) supports two kinds
of .lua files:

| Kind | Entry point | Purpose |
|------|-------------|---------|
| **Tool** | Returns `{name, description, schema, execute}` | Registers one tool definition the LLM can call |
| **Hooks add-on** | Returns `{before_tool_call, on_event, ..., status_line}` | Observes/modifies agent loop behavior |

Add-ons are loaded via `--hooks-file`, `--hooks-dir`, or `config.toml
[addons]`. Multiple add-ons are composed using `compose_hooks()`.

### Currently wired hooks

| Hook | When it fires | Purpose |
|------|---------------|---------|
| `before_tool_call` | Before each tool execution | Block/modify tool calls (e.g., permissions add-on) |
| `after_tool_call` | After tool result available | Rewrite/reroute tool results |
| `should_stop_after_turn` | After each LLM turn | Decide whether to end the loop early |
| `on_event` | Every agent event (start/end/delta) | Observational — log, track, meter |
| `prepare_context` | Before LLM request is built | Prune/compact message history |
| `on_command` | User types `/command` in REPL | Custom slash commands |
| `status_line` | Before each readline prompt | Custom status line (e.g., costline) |
| `tab_title` | Before each readline prompt | Custom terminal tab title |
| `prompt_line` | Before each REPL input | Custom prompt string |
| `format_tool_call` | Tool call is printed | Custom tool call display text |
| `format_tool_result` | Tool result is printed | Custom tool result display text |
| `complete` | Tab-completion in REPL | Custom argument completions |

### State mechanism: `pici.storage`

```lua
pici.storage.get("key")    -- read from shared JSON file
pici.storage.set("key", v) -- write & sync to disk
pici.storage.clear()       -- reset to empty object
pici.storage.path          -- filesystem path string
```

Backed by a single `nlohmann::json::object()` written as pretty-printed JSON
on every `set()`.

---

## Gap 1: Storage persistence broken for `--hooks-dir` and `config.toml`

**Severity:** Bug

**What happens:** `main.cpp:992-995` only sets `storage_path` when
`args.hooks_files` is non-empty:

```cpp
std::filesystem::path storage_path;
if (!args.hooks_files.empty())
    storage_path = std::filesystem::path(args.hooks_files[0]).string()
                   + ".storage.json";
```

If you load add-ons via `--hooks-dir` or `config.toml [addons].dirs`,
`storage_path` stays empty → `LuaHooksImpl::load_storage()` returns
immediately → `pici.storage.get`/`set` works in-memory but **nothing is
persisted to disk**. Restarting pici loses all add-on state.

**Fix:** Set `storage_path` to a sane default directory
(e.g., `~/.config/pici/addon-storage/`) regardless of how add-ons are loaded.
Or derive it from the composed add-on set via a hash.

## Gap 2: No per-add-on storage isolation

**Severity:** Design defect

All add-ons share one `nlohmann::json::object()` via a single `storage_path`.
Two add-ons calling `pici.storage.set("counter", ...)` silently clobber each
other.

**Fix options (in increasing complexity):**

1. **Naive prefixing** — Each `LuaHooksImpl` prepends a namespace derived from
   its source path (e.g., `costline/counter`). No filesystem changes. Collides
   if two add-ons load from the same file (unlikely).

2. **Per-add-on files** — Each `LuaHooksImpl` owns its own storage file,
   derived from its `.lua` path (e.g., `addons/costline.lua` →
   `addons/costline.lua.storage.json`). Requires `AgentInfo` to carry a list of
   per-add-on paths rather than a single path, or have each `LuaHooksImpl`
   compute its own path from a base directory.

3. **Key-value store per add-on** — The `configure` hook passes an opaque
   `pici.storage` instance that is scoped to that add-on's file. Internal to
   C++ it's still one JSON file but with an internal namespace key.

## Gap 3: No lifecycle hooks

**Severity:** Missing feature

There is no `on_load`, `on_unload`, `on_session_start`, `on_session_end`, or
`on_shutdown`. Add-ons that need to initialize counters, open file handles, or
flush state have no hook to do so.

Specific missing lifecycle events:

| Event | When | Use case |
|-------|------|----------|
| `on_load` | After `configure()` runs | Read config, validate deps |
| `on_session_start` | Agent begins a new session (or resumes one) | Reset per-session counters |
| `on_session_end` | Agent session ends / closes | Flush metrics, close handles |
| `on_shutdown` | Process exits (SIGINT, normal exit) | Final flush of `pici.storage` |
| `on_reload` | User runs `/reload-addons` | Teardown and re-init state |

## Gap 4: `on_event` is the only observation hook and it fires constantly

**Severity:** Performance concern

A single turn generates 50-200+ events (message start → N text deltas →
tool_call_start → tool_call_end → turn_end → ...). The `on_event` hook fires
for every one and receives the full canonical JSON event envelope. An add-on
that only cares about `TurnEndEvent` or `ToolCallStartEvent` must deserialize
and pattern-match every event.

**Fix:** Add typed per-event subscription hooks alongside the catch-all:

```lua
-- Proposed new hooks (all optional, like existing ones):
on_turn_start(ctx)      → nil
on_turn_end(ctx)        -- usage, error info
on_tool_start(ctx)      -- {tool_name, call_id, args}
on_tool_end(ctx)        -- {tool_name, call_id, result}
on_message_delta(ctx)   -- streaming text delta
```

Each fires only when its event type occurs. Reduces Lua pcall overhead and
makes add-on code cleaner. The `on_event` catch-all stays for add-ons that
truly need every event.

## Gap 5: Synchronous file I/O on every `pici.storage.set()`

**Severity:** Performance concern inside hot hooks

`pici.storage.set()` calls `save_storage()` which serializes the full JSON
object and writes it to disk synchronously. If called from inside
`format_tool_call` or `on_event` (which already hold the Lua mutex), this
stalls the agent loop.

**Fix:** Debounce writes — buffer in memory and persist on a timer or on
process exit. Or use a background thread with a work queue. For small
add-ons the latency is negligible; for a logging add-on that writes every
event, it adds up.

## Gap 6: No cross-add-on communication

**Severity:** Missing feature

Each `.lua` file gets its own `lua_State*` (own `LuaHooksImpl`). They share
nothing except the implicit `compose_hooks` dispatch in C++ and the global
`pici.storage`. One add-on cannot call another add-on's Lua functions.

Use case: A `metering.lua` add-on tracks per-tool latencies and emits them
via a shared bus. A `status_line.lua` add-on reads from that bus and displays
latencies in the prompt. Currently not possible without the metering add-on
writing to `pici.storage` and the status add-on polling it.

**Fix options:**

1. **Event bus** — A lightweight in-process event bus in Lua where add-ons
   can `pici.bus.subscribe("event_name", handler)` and
   `pici.bus.emit("event_name", data)`. The C++ side creates one shared bus
   table and injects it into each Lua state.

2. **Shared Lua state** — All add-ons share one `lua_State*` but load into
   separate tables. More complex, risk of cross-contamination.

3. **Lua require path** — Let add-ons `require("other_addon")` by configuring
   package.path to include peer add-on directories. Simple but fragile with
   composed hooks ordering.

## Gap 7: No timer / scheduled hooks

**Severity:** Missing feature

No add-on can schedule deferred work. Use cases:

- Auto-save `pici.storage` every 30s (belt-and-suspenders for crash safety)
- Show a spinner refresh every 200ms during a long tool execution
- Auto-summarize after 5 minutes of inactivity

**Fix:** Expose `pici.set_interval(seconds, callback)` and
`pici.set_timeout(seconds, callback)`, backed by `libuv` or a simple
event-loop tick from the main REPL loop.

## Gap 8: Error handling granularity

**Severity:** Minor

| Hook | On Lua error |
|------|-------------|
| `format_tool_call` | Silently falls back to default formatting |
| `format_tool_result` | Silently falls back to default formatting |
| `before_tool_call` | Returns block with error message as reason |
| `after_tool_call` | Returns nullopt (ignored) |
| `on_event` | Error silently swallowed |
| `on_command` | Returns unhandled (falls through) |
| `prepare_context` | Returns nullopt (no pruning done) |

There is no per-add-on error handler, no way to log stack traces, and no way
for an add-on to say "disable me if I error more than 3 times."

**Fix:** Add `on_error(err, source)` hook. Log Lua errors to stderr by default
with source file and line. Optionally disable an add-on after N errors.

## Gap 9: Add-on dependency ordering

**Severity:** Minor

`compose_hooks()` loads add-ons in filesystem or CLI argument order. No add-on
can declare dependencies. `permissions.lua` might want to run
`before_tool_call` first, and `audit_log.lua` wants to run `after_tool_call`
last.

**Fix:** Add a `depends_on = {"other_addon"}` field in the add-on's return
table. `compose_hooks()` topologically sorts before wiring.

---

## Proposal: Generic SQLite Interface?

**You asked:** Would a generic SQLite interface be useful as an extensibility
mechanism?

**Short answer:** Yes, but only for a specific class of add-ons. Not as a
replacement for `pici.storage`.

**What SQLite would be excellent for:**

| Use case | Example add-on |
|----------|---------------|
| **Session journal** — record every turn, token usage, tool result with timestamps | `session_logger.lua` |
| **Cost tracking across sessions** — aggregate across restarts | `cost_history.lua` |
| **Tool result caching** — keyed by tool_name + args hash, evicted by time | `tool_cache.lua` |
| **Performance profiling** — per-tool latency percentiles | `profiler.lua` |
| **User feedback DB** — store model responses user thumbs-up/down'd | `feedback.lua` |

**What SQLite would be *worse* at compared to `pici.storage`:**

| Concern | `pici.storage` (JSON) | SQLite |
|---------|----------------------|--------|
| Setup complexity | None | Schema management, migrations |
| Read overhead for simple state | O(1) key lookup in parsed JSON | Query compilation + B-tree traversal |
| Write overhead for simple state | Rewrite entire file on every set() | Single row insert/update |
| Concurrent write safety | Mutex + rewrite entire file | WAL mode, row-level locking |
| Schema coupling | None (schemaless) | Must define tables upfront |
| Portability | Zero dependencies | Requires vendoring sqlite3 |

**Recommendation:** Ship `pici.db` as an optional API alongside
`pici.storage`, not replacing it. `pici.storage` stays as the simple
key-value convenience. `pici.db` would expose:

```lua
pici.db.exec("CREATE TABLE IF NOT EXISTS log (ts TEXT, event TEXT)")
pici.db.exec("INSERT INTO log VALUES (datetime('now'), ?)", {event_type})
pici.db.query("SELECT * FROM log WHERE event = ?", {event_type})
  -- returns array of row objects
```

Backed by a single `pici_addons.db` SQLite file per user, with the add-on's
source path used as a schema prefix or table name convention.

**Implementation notes:**

- Vendor `sqlite3` (public domain, already in many C++ projects via FetchContent)
- Open DB once in `configure_info()`, close on process exit
- All SQLite calls through the same `LuaHooksImpl::mutex_` (or a separate
  dedicated mutex to avoid blocking the agent loop during long queries)
- Danger: arbitrary SQL from Lua. Mitigate by only exposing prepared statement
  bindings (no raw `exec` with user-provided SQL strings), or sandbox with
  authorizer callback.
- The `pici.storage` file should remain JSON for human readability and
  debugging. `pici.db` is for structured queryable data.

## Summary of priority order

| # | Improvement | Effort | Type |
|---|-------------|--------|------|
| 1 | Fix `--hooks-dir` / config.toml storage persistence | ~1 hour | Bug |
| 2 | Lifecycle hooks (`on_session_start`, `on_session_end`, `on_shutdown`) | ~1 day | Feature |
| 3 | Per-add-on storage namespacing | ~1 day | Design |
| 4 | `pici.db` SQLite interface | ~2-3 days | Feature |
| 5 | Typed event subscriptions (replacing `on_event` pattern matching) | ~1 day | Performance |
| 6 | Cross-add-on event bus | ~2-3 days | Feature |
| 7 | Debounced `pici.storage` writes | ~half day | Performance |
| 8 | Timer/scheduled hooks | ~1-2 days | Feature |
| 9 | Error handling granularity | ~half day | Robustness |
| 10 | Add-on dependency ordering | ~1 day | UX |
