# Plan V — Write-enabled subagents + child context-size exposure

## Goal

Two related upgrades to the child-agent system (`src/core/agent_task.{h,cpp}`,
`addons/agents.lua`):

1. **Write-enabled children.** Allow a spawned child to receive mutating tools
   (`edit`, `write`; later `apply_patch`) behind an explicit, config-gated
   opt-in, instead of today's hard-coded read-only restriction.
2. **Context observability.** Expose each child's current context size (message
   bytes, last-turn token usage, model context window) to the parent in
   `get`/`list` snapshots, so the parent can decide when a child has grown too
   fat and should be closed/discarded.

Both are opt-in, additive changes; default behavior is byte-for-byte unchanged
unless configuration enables it.

## Current state (verified)

- Read-only enforcement lives in **core**, not the addon:
  - `AgentTaskManager::inherit_tools()` (`src/core/agent_task.cpp:341`) filters
    the parent toolset to `capabilities().child_safe == true`; requesting any
    other tool by name throws `invalid_tool`
    ("tool is not available to child agents").
  - Child-safe builtins are exactly `read`, `grep`, `find`, `ls`
    (`BuiltinTool` ctor `child_safe=true` in `builtin_tools.cpp`: lines ~425,
    ~855, ~915, ~990). `bash`, `edit`, `write` default to `false`.
  - Lua tools are never child-safe except the six hardcoded mailbox tools,
    and only when loaded from a file literally named `mailbox.lua` with the
    bundled flag (`lua_tool.cpp:1772-1778`, `main.cpp:1114`,
    `lua_tool.cpp:2145`). `pici.add_tool` has no way to set it.
  - `make_task` also strips all hook callbacks from `child_options_`
    (`agent_task.cpp:~430`), with the comment: *"Children are restricted to
    C++ read-only tools in the in-memory MVP."*
- Snapshots: `AgentTaskManager::snapshot()` (`agent_task.cpp:298`) fills
  `AgentTaskSnapshot` (id/path/status/result/child_count/queued/generation);
  `snapshot_json` in `main.cpp:1449` serializes it. Last-turn token usage is
  captured in `execute_work` (`result.usage = final_message->usage`,
  `agent_task.cpp:707`) but only survives inside `task->result` after a turn
  completes — it is not visible while a child is mid-flight.
- Context size primitives exist: `message_bytes(Message)`
  (`agent_task.cpp:68`), `Limits::max_context_bytes` (2 MiB,
  `agent_task.h:190`), `Agent::context_snapshot()` (`agent.h:131`),
  `Model::context_window` (provider catalog).
- Config: `config.toml.example` has no `[agents]` section yet.

## Design decisions

### D1 — Eligibility vs. grant (two different gates)

Keep `child_safe` meaning *"eligible to be granted to a child"* and add the
real trust decision at **spawn time**, config-gated:

- `bash`/`edit`/`write` become eligible (`child_safe = true`).
- A child only actually receives them when its spawn request names them AND
  the request is permitted by config (D2).

This avoids silently changing the default child toolset (today: the four
read-only tools) for every existing caller, including `/delegate`.

### D2 — Trust boundary: config gate, not addon self-declaration

An addon must never be able to escalate itself. Two independent gates:

1. **Spawn gate.** `SpawnAgentRequest` gains
   `bool allow_write_tools{false};`. `inherit_tools()` honors non-child-safe
   eligible tools only when it is set. The Lua→C++ binding in `main.cpp`
   sets it from the spawn JSON, but only if config allows (below); otherwise
   it throws `permission_denied`.
2. **Config gate.** New `[agents]` section:
   ```toml
   [agents]
   # Tools children may be granted beyond the read-only set.
   # "none" (default) preserves current behavior.
   write_tools = "none"   # none | core | all
   # "core"  → edit, write (workspace-confined file mutations)
   # "all"   → core + bash (requires an active sandbox policy)
   ```
   `"all"` additionally requires `sandbox.mode != disabled` unless the user
   passes an explicit CLI escape hatch, mirroring the existing sandbox
   philosophy in `config.toml.example`.

### D3 — Hooks stay stripped; compensate with tool-level confinement

The reason children strip `before_tool_call` is that Lua-hook authority
(permissions.lua etc.) belongs to the root. Propagating Lua callbacks into
children risks re-entrancy deadlocks (hooks calling `pici.agents.*` from
inside a child's tool call) — out of scope here. Instead:

- Phase 1 grants only `edit`/`write`, which are inherently workspace-confined
  (`resolve_workspace_path`) and produce auditable diffs in the transcript.
- `bash` eligibility ("all") is additionally justified because `BashTool`
  carries its own `SandboxPolicyPtr` and children share the same policy
  object; document plainly in the config comment that hook-based permission
  addons do **not** run inside children.
- Follow-up (non-blocking): a dedicated C++-level permission callback that is
  deadlock-free could later replace this caveat.

### D4 — Lua `add_tool` child-safe opt-in (trusted files only)

Extend `pici.add_tool{ ... , child_safe = true }`:
- `PendingTool` gains `bool child_safe`; `finalize_inline_tools()`
  (`lua_tool.cpp:1765`) uses `spec.child_safe || <mailbox allowlist>`.
- To keep the trust boundary intact, honor `child_safe` only when
  `bundled_child_safe_tools_` is true (i.e. the bundled `mailbox.lua`) — OR
  introduce a second load-flag `trusted_addon_tools` set only for files passed
  explicitly via `--hooks-file`/`[addons].files` (never for `[addons].dir` /
  `--hooks-dir`, where arbitrary drop-ins land).
- This lets power users author write-capable worker tools in Lua later; it is
  not required for Phase 1–2 and can land as Phase 3.

### D5 — Context exposure: bytes + tokens + window

Add an optional `context_info` to `AgentTaskSnapshot`:

```cpp
struct AgentTaskContextInfo {
  std::size_t message_count{0};
  std::size_t context_bytes{0};        // sum of message_bytes()
  std::uint64_t last_input_tokens{0};  // last assistant MessageEnd usage
  std::uint64_t last_output_tokens{0};
  std::uint64_t total_tokens{0};
  std::optional<std::size_t> context_window; // from child's Model
  bool truncated{false};               // hit Limits::max_context_bytes path
};
std::optional<AgentTaskContextInfo> context;
```

Populated in `snapshot()` while holding `task->mutex`:
- bytes/count: `context_snapshot()` on the task's agent (state has its own
  lock; manager `mutex_` is **not** held in `snapshot()` — verify lock
  ordering stays task->mutex → agent-state mutex everywhere);
- usage: cache `final_message->usage` into `Task` (new field, e.g.
  `TokenUsage last_usage`) in `execute_work`'s callback path
  (`MessageEndEvent`, `agent_task.cpp:703`) so it updates *during* the turn,
  not only at completion;
- window: resolved once at `make_task` time from `options.model`.

Serialize in `snapshot_json` (`main.cpp:1449`) as an additive `"context"`
object. Parents (and humans via `/agents`) can now apply a discard heuristic:
`last_input_tokens ≈ 0.8 × context_window` → close and respawn fresh.

### D6 — Surface area in `addons/agents.lua`

- `spawn_agent` schema gains `allow_write_tools` (boolean) and keeps
  `tools` (array) — description updated to explain the config gate.
- `list_agents`/`get` pass through the new `context` object (free).
- `/agents` command output gains a `ctx=` column
  (`bytes` and `%window` when known).
- No mailbox protocol changes; snapshots ride the existing structures.

## Phases

### Phase 1 — Context exposure (no behavior change, ship first)

1. `agent_task.h`: add `AgentTaskContextInfo` + `std::optional<context>` to
   `AgentTaskSnapshot`; add `TokenUsage last_usage` to `Task`.
2. `agent_task.cpp`:
   - record usage on every assistant `MessageEndEvent` in `execute_work`;
   - populate `context` in `snapshot()`; resolve `context_window` in
     `make_task`.
3. `main.cpp`: extend `snapshot_json` (additive keys only).
4. `addons/agents.lua`: `/agents` column; docs string updates.
5. Tests (`test/test_agent_tasks.cpp`):
   - idle child reports exact seeded-context bytes;
   - `last_usage` updates after first completed turn and again mid-life on a
     follow-up turn (`follow_up`);
   - `context_window` present iff model declares one;
   - lock-order stress: `get()` during an active streaming turn (TSan build
     exists: `build-tsan/`).

### Phase 2 — Write-enabled children

1. `config.toml.example` + config parsing: `[agents].write_tools`.
2. `builtin_tools.cpp`: mark `EditTool`, `WriteTool` (`BashTool` under
   `"all"`) as `child_safe = true`.
3. `agent_task.h`: `SpawnAgentRequest::allow_write_tools`;
   `agent_task.cpp:inherit_tools()`: when set, permit requested tools whose
   `child_safe` flipped in step 2; keep the `invalid_tool` error for anything
   else (error text updated to name the config knob).
4. `main.cpp` spawn binding: parse `allow_write_tools`; enforce config gate →
   `permission_denied` with actionable message otherwise.
5. Wire-through in `make_task`: nothing else changes — hooks remain stripped
   (D3), limits identical.
6. Tests:
   - default (`write_tools="none"`): spawn with `tools=["edit"]` still errors
     exactly as today;
   - `write_tools="core"` + `allow_write_tools=true`: child given `edit`
     actually mutates a workspace file (tmpdir workspace fixture);
   - sibling isolation: requesting `bash` under `"core"` errors;
   - `"all"` + `sandbox.mode=disabled` → denied; with sandbox → allowed;
   - read-only defaults unchanged for `/delegate` and mailbox flows
     (`test_mailbox_bindings.cpp` regression).

### Phase 3 — Lua-authored child-safe tools (optional)

Implement D4 (`add_tool{child_safe=...}` + trusted-source flag plumbing in
`load_lua_hooks`/`load_lua_hooks_dir` signatures, `main.cpp:1114/2145` callers).
Tests in `test_lua_tool.cpp`: flag honored only for trusted loads; dir-loaded
files silently demoted with a startup warning.

### Phase 4 — Parent-side ergonomics (follow-up, separate plan)

- Auto-budget: `[agents] max_child_context_fraction = 0.85` → warning event to
  parent (or auto-close) when crossed; needs an emit path from
  `MessageEndEvent` handling.
- Documented discard-and-respawn pattern for long-running workers using
  `context` + `close_agent` + `spawn` with `recent_messages` inheritance.

## Risks / notes

- **Permission bypass:** children do not run permissions.lua-style hooks (D3).
  Mitigated by config gating, `edit`/`write` confinement, sandbox requirement
  for `bash`, and explicit documentation.
- **Lock ordering:** `snapshot()` will newly touch agent state. Manager
  `mutex_` is not held there today; keep it that way and add the TSan test.
- **Shared tool instances:** inherited tools are the parent's instances; all
  builtins are stateless/const-execute — verified for the four existing
  child-safe tools and true of `Edit`/`Write`/`Bash` as well.
- **Additive JSON:** new snapshot keys only; mailbox/faux-control consumers
  ignore unknown fields.
- **Compat:** `write_tools="none"` default means every existing call site
  (including `run_agent` compat path in `main.cpp:1512`) behaves identically.

## Build / verification

```sh
cmake --build build && ctest --test-dir build            # unit
cmake --build build-tsan && ctest --test-dir build-tsan  # concurrency (Phase 1)
```

Manual: run pici with `write_tools="core"`, spawn a writer child via
`/delegate`, watch `/agents` `ctx=` climb across follow-ups, close at ~80%
window, respawn with `recent_messages` context and confirm continuity.
