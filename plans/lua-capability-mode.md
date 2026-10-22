# Lua as Pici’s only tool

Proposal · 17 October 2026 · Sandboxed Lua worker · Initial release implemented

## Recommendation

Add an explicit Lua-only mode whose single tool runs code in a **separate Lua worker inside Pici’s OS sandbox**. Extend the existing bubblewrap launcher rather than build a new filesystem API. Use familiar Lua libraries: `io`, `os`, and LuaFileSystem (`lfs`). Keep a per-call `requires` argument so users can see which library interfaces the script requests.

**Separate library selection from permissions.** `requires` controls which modules the worker exposes. User-configured sandbox policy controls filesystem mounts, writes, network access, environment variables, and resources. Selecting `io` does not grant access to the host filesystem, and leaving out `io` does not prove a script cannot read files through a command. The OS envelope is the authority boundary, including for subprocesses.

Start with one worker per invocation, Linux/bubblewrap enforcement, standard libraries plus bundled `lfs`, no host RPC bridge, and a stable tool schema. Do not require custom `fs.read.v1`/`fs.write.v1` APIs or version suffixes in imports. Pin runtime and package versions in the shipped worker and record them in metadata.

Every model-initiated action passes through the Lua tool. Pici’s own inference, transcript persistence, frontend input, and trusted administrative add-ons remain host operations. “Lua-only” describes the agent’s entry point: a script may use shell commands or other interpreters inside the sandbox when those interfaces are available.

## Implemented first release

The repository now has `--lua-only`, a dedicated `pi-lua-worker` executable, standard Lua utilities and JSON, and per-call selection of `io`, `os`, and bundled LuaFileSystem 1.8.0. Library ceilings and workspace roots are configurable through CLI/TOML. Read-only workspace access is the default; writes need explicit roots. Admission is pinned to the worker tool across registration/reload, including dispatch. The old in-process evaluator remains separate.

The launcher uses pinned directory/file descriptors, a sealed process-lifetime worker image, argv execution, hard limits installed through util-linux prlimit before worker execution, a cleared environment, read-only runtime mounts, isolated networking, and a disposable worker/process tree. The result channel is a private temporary file mounted into the sandbox, separate from captured stdout/stderr. JSON round trips preserve empty arrays; json.array() constructs one explicitly. Results are untrusted and bounded, and cannot request host actions.

Practical limits are documented in `docs/lua-worker-mode.md`: a configurable per-process address-space limit (1024 MiB default; 0 explicitly disables it), 256 MiB per-file writes, 64 MiB temporary storage, independent deadlines, and bounded output/results. This does not claim aggregate process-tree quotas, per-file secret exclusions, or complete syscall auditing. Runtime and package versions are reported with results; Lua comes from the existing system Lua 5.4 dependency.

The initial policy is fixed for an activation. Its compact snapshot is appended to the stable system instructions rather than changing the tool schema; mutable mid-session grants are not implemented. Import selections vary only in tool-call arguments. This preserves a stable prefix within the session while keeping its permission profile visible.

Validation: full build and all 53 CTest targets passed, including 24 new worker tests. Targeted ASan/UBSan instrumentation of the new worker and host adapter passed 23 tests; the address-space exhaustion case was skipped under sanitizers and passed in the ordinary build. The broader test run also exposed a warm-up/idle race in an existing agent-task test; synchronizing the two affected warm-up sites with the idle state stabilized those tests; the initial failing case passed 50 repetitions. Changed-source formatting and the CLI single-tool smoke check passed. Focused clang-tidy completed for all six checked components; the new runtime files were warning-free, with existing advisories in legacy CLI code. A repository-wide analyzer run was stopped after more than 20 minutes while it was still working through unrelated code; this is not a claim of a full-repository lint pass.

LPeg and argv-based process convenience bindings remain follow-ups after this first usable runtime. Networking libraries, extra format parsers, persistent workers, and a host coordination bridge remain optional later work. No provider traffic or paid live-model cache experiment was needed for these deterministic checks.

## Existing foundation and required changes

| Existing code | Reuse | Gap to close |
|---|---|---|
| `src/core/sandbox.{h,cpp}` | Bubblewrap namespaces, workspace mounts, command launching | Currently executes `/bin/sh -c`, exposes a writable workspace and broad system directories, and does not clear inherited environment. Add argv launching, explicit mounts, controlled runtime dependencies, and environment/descriptor isolation. |
| `src/core/lua_tool.cpp` | Evaluator concepts: source validation, captured output, JSON conversion, compile-only mode, budgets | Existing evaluator executes inside Pici and lacks `io`, standard imports, and shell access. Add a separate worker backend; do not load unrestricted libraries into the host evaluator. |
| `src/core/lua_eval_tool.h` | Existing session lifetime and bounded scratch concepts | Live C++ closures for agents/session/mailbox cannot be passed into another process. Keep host coordination out of the initial worker. |
| `src/cli/cmd_run_session.h` | Tool configuration and evaluator opt-in | Add-on tools are appended after the allowlist. Enforce Lua-only selection after every registration/reload and at dispatch. |
| Provider serializers | Existing tool schemas, append-only tool history, cache usage reporting | Keep definitions stable across changing manifests; validate actual payloads. |

Pici already has `--sandbox auto|required|disabled` and `--no-sandbox`, but this currently confines shell commands, not the host process or embedded Lua. Reuse the mechanism, not an assumption that current flags already sandbox the evaluator. In the new constrained mode, require sandbox availability. Reject a conflicting `--sandbox disabled` rather than silently launching unconfined code. Preserve ordinary bash and legacy evaluator behavior outside the new mode.

## Proposed tool contract

A file read:

```json
{
  "requires": ["io"],
  "script": "local io = require('io'); local f = assert(io.open('README.md', 'r')); local text = f:read('*a'); f:close(); return text",
  "mode": "run",
  "timeout_ms": 30000
}
```

A directory operation:

```json
{
  "requires": ["lfs"],
  "script": "local lfs = require('lfs'); assert(lfs.chdir('src')); local names = {}; for name in lfs.dir('.') do names[#names+1] = name end; return names"
}
```

A command:

```json
{
  "requires": ["os"],
  "script": "local os = require('os'); local ok, kind, status = os.execute('git status --short'); return {ok=ok, kind=kind, status=status}"
}
```

Configuration and examples in this document are proposals, not existing CLI/API support. Keep `script`, `mode`, and `timeout_ms`; add required `requires` as an array of unique module-name strings, including `[]`. Optionally add `cwd` for a workspace-relative initial directory. Enforce type, field, size, and value limits before starting a worker. Avoid an enum generated from session grants; version the worker protocol without making imports awkward.

Pure base/string/table/math/utf8 facilities and JSON serialization can be fixed bootstrap facilities, documented once. Effectful libraries are exposed only when selected. Approved `io`/`os` may retain their usual globals as well as support `require()`; globals and preloaded tables for unselected libraries must be absent. This preserves familiar behavior without requiring two import conventions.

`mode: "check"` validates the envelope and available library names, then compiles text-only source without loading effectful modules or executing code. It does not prove imports, file access, or commands will succeed at runtime. Use a restricted compile worker with no writable workspace; do not initialize `lfs` merely to check syntax.

## Standard libraries and module selection

| Library | Familiar interfaces | Permission implications |
|---|---|---|
| `io` | `io.open`, file reads/writes, `io.popen` | Full library includes command execution through `popen`; do not describe it as filesystem-only. File access remains limited by sandbox mounts. |
| `os` | `os.rename`, `os.remove`, `os.execute`, time, environment access, exit | Full library includes commands, mutations, and worker exit. `os.exit` ends the disposable worker, not Pici; environment access sees only sanitized variables. |
| `lfs` | Directory iteration, attributes, mkdir, `chdir` | Bundle LuaFileSystem, an existing C extension. Directory changes affect the worker and its children, never the Pici host. |
| Pure Lua modules | User-reviewed utilities and dependencies | Load from approved packages staged read-only; they inherit the worker’s sandbox and selected interfaces. |
| Additional native modules, later | Established third-party bindings | Explicit installation and review; native code can bypass Lua-level module restrictions, but still runs inside the OS envelope. |

[Lua’s reference manual](https://www.lua.org/manual/5.4/manual.html#6.8) documents `io` and `os`; [LuaFileSystem’s manual](https://lunarmodules.github.io/luafilesystem/manual.html) documents `lfs`. There is no need to make Pici’s own file-read/write vocabulary the primary user API.

Validate the full `requires` list against the user’s enabled-library ceiling before source execution or package initialization. Unknown/unavailable entries fail rather than disappear silently. Resolve approved transitive dependencies in advance and show effectful dependencies in metadata. Unused declarations are allowed. Do not infer imports with source-text searches.

Use a host-selected loader inside the worker: approved built-ins/preloads and pinned package files only, with no ambient working-directory search or user-controlled `LUA_PATH`/`LUA_CPATH`. Do not expose unrestricted `package.loadlib`, debug/registry access, or alternate loaders as a shortcut around selection. Keep `loadfile`/`dofile` out of the bootstrap unless explicitly supported. This is a small loader policy, not a replacement filesystem API. Native libraries and full command execution mean the selection mechanism is not a separate security boundary against arbitrary code; the sandbox remains authoritative.

Library versions are pinned at build/install time. The model cannot install packages or enable a new host module by adding its name to `requires`. A command-enabled worker may install or run things inside its allowed workspace when the network/runtime permits; this does not modify Pici’s approved module catalog or expand mounts.

## Runtime library bundle and command environment

Ship standard `string`, `table`, `math`, `utf8`, and `coroutine` utilities as bootstrap facilities. Restore normal string patterns inside the isolated worker, whose parent can terminate blocked native computation. Provide bounded JSON encode/decode using the existing JSON dependency and bundle `lfs` for filesystem metadata and directories. Select `io`, `os`, and `lfs` through `requires`.

The first usable release is standard Lua + JSON + LuaFileSystem. Follow with argv-based process execution and LPeg when the basic worker is validated. Evaluate existing luaposix/process bindings before adding a convenience API for argument arrays, captured stdout/stderr, and exit status. LPeg is the preferred optional parsing addition; avoid requiring it for ordinary string edits. These follow-up packages are not prerequisites for the initial release.

Defer LuaSocket/HTTP until an explicitly enabled network profile exists. Add YAML/TOML, SQLite, and archive libraries when real workloads justify them. Package versions are pinned and approved by the user/host; runtime imports do not install libraries.

Treat available commands separately from Lua modules. The development sandbox should expose configured read-only system/toolchain locations containing `rg`, `git`, build tools, and test runners where installed, without promising host HOME/configuration or network access. Libraries are selected per call; installed commands remain subject to the same sandbox envelope. Document missing commands and runtime dependencies rather than silently invoking host tools outside the sandbox.

## Permissions users can reason about

Use two independent displays:

```text
Libraries requested: io, lfs
Sandbox: read workspace; write src/ and scratch/; network off
Working directory: /workspace
```

The first answers “which APIs are directly exposed?” The second answers “what resources can this invocation reach?” Both matter. Never equate a library name to a read/write grant.

Illustrative configuration:

```toml
[tools]
mode = "lua-only"

[lua]
libraries = ["io", "os", "lfs"]

[lua.sandbox]
read_roots = ["."]
write_roots = ["./src", "./scratch"]
network = false
address_space_mb = 1024
```

Use a single resolved policy shared by execution and presentation. Default profiles: compute-only (no workspace), workspace read (read-only mount), and workspace edit (explicit writable roots). Runtime/toolchain mounts are additional declared infrastructure, not invisible host access. Temporary storage is writable even in workspace-read mode; describe that distinction. Do not introduce per-call approval dialogs by default; use existing policy/admission behavior where applicable.

Keep the session sandbox envelope fixed across calls initially. `requires` narrows available interfaces, not mount permissions. A later optional per-call sandbox argument could attenuate paths/budgets without expanding the session ceiling. Avoid coupling “io selected” to “mount writable”: full `io` supports both read and write, and commands/native code can use the same mounts without it.

## Worker lifecycle and process boundary

```text
Agent invokes the one lua tool
    → host validates envelope, selected libraries, and current policy
    → reusable launcher starts a disposable worker inside bubblewrap
    → worker loads approved libraries and runs the script
    → worker and its children operate within the same OS envelope
    → parent collects bounded output/results and waits for teardown
    → tool result returns to the Agent
```

Ship a small dedicated worker executable linked to the project’s chosen Lua version and bundled `lfs`, instead of depending on an arbitrary `lua` on PATH. Keep it independent of provider credentials, session stores, plugin discovery, and the full CLI startup path. Re-exec into that minimal executable rather than executing a Lua VM in a forked copy of Pici’s multithreaded host. Audit process spawning so only safe setup happens between fork and exec.

Extend the launcher to accept executable plus argv; the worker launch must not concatenate Lua source into a shell command. Send source/options over a bounded input pipe. Separate worker protocol frames from stdout/stderr, since scripts and child processes can write directly to those streams. Close all nonessential inherited descriptors and mark protocol/control descriptors close-on-exec for descendants. Treat every worker frame as untrusted: validate shape, lengths, nesting, and terminal states; never let a frame request new permissions or arbitrary host actions. A dedicated result channel prevents accidental mixing, not forgery by fully capable native worker code.

Define JSON-compatible return values, explicit errors for unsupported/cyclic values, bounded print/stdout/stderr, worker exit status, signal/crash/timeout outcomes, and partial-output handling. Distinguish syntax error, runtime error, sandbox setup failure, abrupt exit, and cancellation. A worker with `os.exit` can finish without a result frame; this must be a normal handled failure, not a host crash.

Fresh worker means fresh globals, handles, cwd, and Lua state. Workspace files persist; temporary storage is per invocation. Do not promise existing `pici.scratch` automatically works across processes. Initial worker mode has no scratch/coordination bridge; scripts can return data into conversation or deliberately store files within policy. Persistent workers and bounded host-backed scratch are later options with explicit protocols.

Keep execution sequential initially. The parent owns wall-clock deadlines independently of Lua hooks: blocking I/O, C extensions, pathological string operations, and child commands must all be interruptible by terminating the sandbox process tree. On normal completion as well as cancellation, stop remaining descendants before resolving the tool call. Do not allow background writers to survive an invocation.

## Reusing and tightening the sandbox

[Bubblewrap](https://github.com/containers/bubblewrap) supplies isolation primitives; effective policy depends on the arguments and resources the application supplies. Extend the existing launcher with:

- Explicit read-only/read-write mounts and an isolated root. Stage the worker, approved modules, required dynamic libraries, and optional toolchain read-only. Mount only configured workspace roots; use a stable `/workspace` view and validated initial cwd.
- A minimal environment built from scratch. Do not pass API tokens, cloud credentials, agent socket paths, `LD_PRELOAD`, Lua search-path overrides, or arbitrary host HOME. Provide controlled PATH/HOME/TMPDIR and only explicitly approved variables. Keep credentials owned by Authentication in the host.
- An audited mount list rather than broad default `/etc`/home exposure. Remove inherited host descriptors, sockets, and access to host `/proc`; preserve the existing private namespaces and network-off baseline. Namespace isolation alone is not a disk/memory/process quota.
- Parent-enforced wall time and output limits, worker memory/CPU/file-size/fd/process limits, and a documented process-tree accounting/cleanup strategy. Per-process resource limits do not bound the sum of all descendants; use cgroups or equivalent when aggregate limits are promised. Plain tmpfs and per-file limits do not cap every write to a mounted workspace; document disk/quota limits accurately.
- A verified no-downgrade path: the constrained worker fails when bubblewrap or required host support is unavailable. Ordinary `auto` semantics elsewhere remain unchanged. An unrestricted alternative, if added later, needs an explicit separately named mode.

Build the sandbox from verified host-side root descriptors/mount sources, not script-controlled host paths. Test symlink escapes, swapped mount source paths, readonly boundaries, and hard-link aliases. A broad workspace mount can expose `.env`, `.git` configuration, credentials checked into the project, sockets, and other sensitive resources; explicitly exclude/mask resources required by policy and document remaining workspace visibility. Read-only exposure still allows reading secrets. The mount model protects the host outside exposed resources; it does not make all project contents harmless.

`lfs.chdir` is now ordinary and safe with respect to host state: only the disposable worker’s cwd changes. Relative paths continue from that directory during the invocation; the next worker starts from its explicit initial cwd.

## Limitations and tradeoffs

1. **Library selection is coarse.** Full `io` includes `popen`; full `os` includes execution and exit. A compute-only profile must not claim stronger guarantees just because names are omitted when native execution is available. For a strict “no subprocesses” profile, either add explicit small function restrictions plus suitable OS enforcement, or do not offer that promise in v1. The recommended workspace profiles allow processes within the envelope when their selected APIs provide them.
2. **An interpreter is not a language jail.** A script can launch Python or shell through available commands. This still satisfies a single Lua tool entry point, but cannot guarantee every executed instruction is Lua.
3. **No atomic script transaction.** Writes and commands before an error persist; automatic retries may repeat effects. Ordinary file APIs do not automatically provide guarded patches or atomic replacement. Users can implement those patterns in Lua, or add convenience libraries later.
4. **No complete effects audit from standard APIs.** Capturing stdout and return values does not capture every read/write or command effect. Display selected libraries, sandbox scope, execution status, and optional best-effort workspace diffs; do not label them a complete audit. Strong syscall/process auditing is separate work.
5. **Startup and deployment cost.** Each call creates a worker and namespaces; runtime/module binaries must be packaged and available inside mounts. Measure overhead before introducing pools. Linux is the initial enforced platform; do not silently emulate the sandbox elsewhere.
6. **Standard Lua behavior can block or allocate outside the VM.** Parent-side kill and OS accounting are essential; the old 64 MB Lua allocator ceiling alone is insufficient for native modules and descendants. Aggregate limits need real mechanisms.
7. **No direct Pici API closures in v1.** Host agents/session/mailbox access needs a later narrow RPC adapter with its own native authorization. Never give the worker a generic host tool dispatcher or credential-bearing socket.
8. **Writable shared workspace means shared effects.** Parallel sessions can conflict. Sandboxes do not supply branch isolation or undo. Per-call roots and cwd are isolated; shared mounted files are intentionally not.

Moving Lua outside Pici also allows reconsidering evaluator-specific restrictions such as removed string-pattern functions: worker deadlines can kill blocked native work. Restore ordinary behavior where practical and tested, rather than copying every in-process safeguard without a reason. Retain bounded protocol serialization and conservative module loading.

## Alternatives and paradigm mapping

| Approach | Tradeoff |
|---|---|
| **Sandboxed worker + standard libraries + per-call requires — chosen** | Familiar Lua, existing OS launcher, inspectable modules. Permissions are mount/network/resource policy rather than module-name effects. |
| Same worker, all user-approved modules always available | Simpler imports, less per-call visibility. Reasonable later if manifests cause measured friction. |
| Same worker, optional per-call narrower sandbox profile | Better per-call attenuation and review. More schema and policy complexity; keep session ceiling authoritative. |
| In-process restricted wrappers | More precise per-operation authorization and audit. Requires maintaining custom bindings and defending host state; not the selected filesystem approach. |
| Persistent sandboxed kernel | Lower startup overhead and notebook-like globals/cwd. Harder cleanup, revocation, concurrency, and resource attribution; defer. |

The selected design is closest to a containerized code interpreter with an explicit import manifest. OS sandbox policy resembles container permissions/mounts; `requires` resembles dependency declaration. It is less like an object-capability API or a statically checked effect system: APIs are familiar and authority is an environment boundary. A module declaration cannot describe every syscall or transitive command effect. Programmatic tool calling still applies: Lua composes many operations before returning one result, without exposing dozens of model tools.

## Prompt-cache strategy

Keep the `lua` tool name/schema/description and compact worker API reference stable. Put changing `requires`, `cwd`, source, and timeout in call arguments. Do not generate schema enums or tool descriptions from the last module selection. Import standard names (`io`, `os`, `lfs`), with versions in a session runtime snapshot rather than versioned names repeated in scripts.

Preserve this layout:

```text
Stable tool definition and concise Lua usage instructions
Stable behavioral instructions
Session snapshot: available libraries, versions, sandbox mounts/network/limits
Conversation with new calls/results appended
```

Publish policy changes as appended context at idle boundaries; native execution always uses current policy. Restore the current snapshot after compaction/resume. Do not rewrite historical calls or rotate cache keys for each `requires` list. A changed initial session snapshot can reduce cross-session prefix sharing after that point; this is acceptable and must not be hidden from users.

[OpenAI’s caching guide](https://developers.openai.com/api/docs/guides/prompt-caching) recommends stable tools and preserved conversation prefixes. [Anthropic’s guide](https://platform.claude.com/docs/en/build-with-claude/prompt-caching) describes tools → system → messages caching. The design implication is a stable prefix with varying call arguments, not a guarantee of hits across every model or gateway. Keep existing provider routing/cache keys unchanged unless measurements justify a separate change.

Use a compact fixed library reference initially. Optional documentation lookup can run through a pure bootstrap function inside the same tool without loading effectful modules. Avoid inserting newly selected manuals into the system prompt each turn. Measure actual request prefixes, cache-read tokens, total tokens, latency, worker startup overhead, and task success. Do not assume one tool is cheaper if its instructions become much larger.

## Ownership and architectural contracts

Follow `docs/object-taxonomy.md`, the lexicon, and migration ordering. Current host-owned evaluator execution becomes a session-selected tool adapter invoking a process-owned launcher; live Lua state moves from the host into a call-scoped OS child. Domain ownership does not move into that child.

| Proposed noun | Archetype and owner | Lifetime, authority, and contract |
|---|---|---|
| `LuaWorkerPolicy` | Value, held by SessionRuntime | Immutable resolved revision of library ceiling and sandbox settings for the activation; one source for effective policy. Process config constrains it. |
| `LuaWorkerAdapter` | Adapter implementation owned by PiciProcess | Process lifetime, reusable launch/protocol implementation; consumes bounded execution requests, owns no transcript/session facts. Session tool bindings reference it. |
| `LuaExecution` | Operation owned by the active tool invocation | One invocation; owns worker process, pipes, deadline, teardown, and result assembly. No durable identity and no extra unqualified Runtime. |
| `LuaWorkerRequest` / result | Values crossing the execution boundary | One call; typed policy-derived execution request and validated result. Protocol JSON and Lua values remain adapter details. |

A helper executable is a packaging detail, not a new domain service. SessionRuntime holds policy/bindings, AgentState owns the admitted single tool, TaskTree remains Agent-owned, and stores remain process-owned. Do not put policy authority into worker globals. If policy metadata is persisted, preserve durable-write-before-live-install transitions and idle-only replacement; resume intersects with current configuration rather than restoring obsolete grants.

Leave existing in-process `--lua-eval` behavior intact in legacy mode. The explicit Lua-only mode selects the worker backend and excludes the legacy host evaluator. ACP does not currently gain the evaluator; reject unsupported mode requests clearly rather than widening its feature scope incidentally.

Coordination is deferred. Any later worker RPC must implement specific session/delegation/mailbox contracts with actor and policy checks; it cannot become a host filesystem/process escape. Preserve Mailbox’s at-least-once delivery and acknowledge timing. If child agents are later enabled, apply Lua-only admission and attenuated policy recursively through their SessionRuntimes; do not merely mark the current evaluator child-safe.

## Implementation phases and acceptance gates

### Phase 1 — Specify mode and enforce final tool admission

Add Lua-only config and resolved worker policy. Audit `set_tools`/`add_tool`, reload, MCP, and add-on paths; enforce both provider advertisement and dispatch. Validate incompatible sandbox flags before executing a prompt. Preserve default/legacy behavior and ACP exclusions.

Files: `src/cli/{args,config}.{h,cpp}`, `src/cli/cmd_run_session.h`, shared Agent/tool dispatch, SessionRuntime and process composition as needed.

Gate: exactly one worker-backed Lua tool remains after registration/reload; hidden tools cannot be invoked; default mode is unchanged.

### Phase 2 — Generalize the existing launcher and package the worker

Add argv execution and explicit sandbox/environment specifications while retaining a shell convenience path for existing bash callers. Build a minimal worker executable and vendor/pin `lfs` using repository dependency conventions. Introduce bounded request/result framing, isolated output streams, and parent-owned child lifecycle. Worker runs only after sandbox setup; no unrestricted fallback.

Files: `src/core/sandbox.{h,cpp}`, new worker entry/protocol files, process adapter wiring, `CMakeLists.txt` and packaging/install rules. Avoid broad changes to unrelated frontend/domain code.

Gate: disposable worker starts with expected mounts/environment, returns structured values/errors, and exits cleanly. Setup failures never execute source. Existing bash sandbox behavior passes regression tests.

### Phase 3 — Select libraries and use ordinary filesystem APIs

Implement `requires` validation, pinned loader/preloads, standard `io`/`os`, and bundled `lfs`. Complete read-only/read-write roots, network-off mode, cwd validation, and controlled toolchain/runtime mounts. Verify text-only compile checks with no module initializers. Remove ambient host bindings from this backend; keep legacy evaluator separate.

Gate: familiar Lua reads/writes/lists/chdir and runs commands inside allowed mounts; read-only boundaries and unauthorized host paths remain enforced even through shell/native code. Unknown libraries and denied manifests execute no script effects.

### Phase 4 — Bound resources and handle failure honestly

Implement independent parent deadlines, process-tree teardown, output/protocol caps, platform resource limits, and the chosen aggregate accounting strategy. Handle `os.exit`, crashes, hangs, flooding, subprocess fan-out, and missing terminal frames. Show libraries, actual sandbox profile, output, and failure/partial-effect status without claiming full effects auditing.

Gate: no worker/descendants outlive a resolved tool call; parent survives worker corruption/crashes; no credential/descriptor leakage; limits claimed in UI are tested. Unsupported aggregate limits are documented rather than advertised.

### Phase 5 — Usability, caching, and release documentation

Add compact standard-library instructions and session snapshots. Test end-to-end inspect/edit/build workflows, error recovery, fresh cwd/state, and old evaluator compatibility. Compare payload prefixes across manifests and measure optional live cache behavior. Document platform support, installed library versions, missing host scratch/coordination, and shared-workspace effects. Defer persistent kernels and generic native-module loading.

Gate: a real coding task completes through Lua alone; manifests vary without tool-schema churn; startup overhead and failure rates are measured. New mode remains an explicit user choice.

## Verification plan

- Worker protocol tests: malformed/oversized requests and results, invalid UTF-8/binary output, cyclic/non-JSON return values, missing frame, source passed without shell interpolation, and separate stdout/stderr handling.
- Module tests: only selected libraries exposed through globals/preloads, blocked alternative loaders, denied dependency closure, initializer purity for checks, pinned package resolution, and fresh VM per call. Native-code selection limits are documented accurately.
- Sandbox integration tests exercising real effects: `io` and shell both denied writes to readonly mounts; traversal/symlink/mount-source swaps cannot reach unexposed host files; no credentials, ambient descriptors, host sockets or denied networks. Include workspace secret visibility/masking tests.
- Lifecycle tests: long native library call, blocked read, runaway command, worker exit/crash, huge output, and descendants started in new process groups. Verify complete teardown on normal completion, timeout, cancellation, and host shutdown.
- Configuration/admission tests: add-on reload, MCP discovery, hidden-tool dispatch, incompatible flags, missing bubblewrap, unsupported platforms, legacy behavior, and ACP exclusion.
- Provider serialization tests: varying `requires` and `cwd` preserves stable tool definitions/instructions. Live cache experiments remain optional, separate from deterministic CI.
- Use focused C++ targets during implementation. Before merging run repository build/CTest, formatting, lint, and appropriate sanitizer/concurrency checks. This document update alone does not need a C++ rebuild.

## Decisions retained for implementation

- **Chosen:** worker process inside the existing extended sandbox, standard libraries, explicit per-call `requires`, separate OS permission policy, disposable state, no initial host RPC.
- **Starting profiles:** workspace-read and workspace-edit with network off; compute-only available when policy and mounted runtime match the name.
- **Initial support:** Linux/bubblewrap with a hard failure when confinement cannot be established.
- **To resolve during implementation:** exact runtime/toolchain packaging, process-tree accounting mechanism and quotas, module/global bootstrap details, and optional initial `cwd` field. These do not change the core execution model.

## Sources and provenance

Repository inspected 17 October 2026; user-owned unrelated files remain untouched. Normative references: `docs/object-taxonomy.md`, `docs/architecture-lexicon.md`, `plans/object-taxonomy-migration.md`; build/test guidance: `claude.md` and `docs/testing-with-googletest.md`.

Primary external references: [Lua standard libraries and imports](https://www.lua.org/manual/5.4/manual.html), [LuaFileSystem](https://lunarmodules.github.io/luafilesystem/manual.html), [bubblewrap](https://github.com/containers/bubblewrap), [OpenAI prompt caching](https://developers.openai.com/api/docs/guides/prompt-caching), and [Anthropic prompt caching](https://platform.claude.com/docs/en/build-with-claude/prompt-caching). Implementation choices and rollout gates are proposals, not claims that these features already exist in Pici.
