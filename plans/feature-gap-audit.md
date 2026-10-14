# Pici feature and safety audit

Audit started 2026-10-12 and continued 2026-10-13 (America/Los_Angeles).
Compare the checked-out sources,
not upstream marketing or an assumed current release.

- Pici working tree: application code unchanged by this audit; build-tooling
  changes described below. Existing untracked files were preserved.
- Codex: `vendor/codex` at `f82e36592278a0a782fb0d0fc97e5fa10e832ac2`.
- Pi: `vendor/pi` at `7d75a0e3eb273dff753d49c066ea29fbe7b3b6f6`.
- Tracker: [Pici project](https://tasks.home.nope.space/#/projects/13).

Object names and ownership follow [object-taxonomy.md](../docs/object-taxonomy.md).
The detailed [Codex comparison](codex-feature-gap-audit.md) records reference
paths, legacy-versus-v2 differences, intentionally absent behavior and uncertainty.
This audit uses the target taxonomy rather than migration-era names. Older
`docs/pi-gap-analysis.md` findings about a missing shared runtime/catalog are
historical: those facilities now exist.

## Agent coordination

Pici already has native delegation mechanisms and a durable cross-session
mailbox. The largest gaps are in recovery, admitting completion work to a
parent, and policy propagation. Pi's README explicitly says subagents are
not a built-in default; its example extension is a separate comparison.

| Capability | Pici today | Reference / remaining work |
|---|---|---|
| Spawn, list, status, result | Implemented in `TaskTree`; exposed through Lua add-on, JSONL RPC and ACP task endpoints | Codex has both legacy and v2 coordination tools. Pi's optional `examples/extensions/subagent/index.ts` launches separate Pi processes. |
| Message / follow-up / interruption / close | Implemented; passive messages and turn-triggering follow-ups are distinct | Preserve this distinction in any compatibility adapter; Codex's message and follow-up APIs also have distinct semantics. |
| Context inheritance and model override | Implemented: none/full/through-message/recent modes, provider-aware model resolution | Child configuration remains narrower than the root runtime; no named delegation profiles. |
| Wait without losing an event | Generation-based wait with cancellation and bounded timeout | Pici waits on status **changes**, not necessarily terminal completion. Callers must advance the generation and inspect status. |
| Completion notifications | CLI activity bridge wakes and redraws UI; ACP task events stream; native status events exist | No automatic parent-agent completion input in the traced callback path. UI notification alone cannot continue synthesis after the parent settles. |
| Resident child continuation | Follow-up retains the same child transcript/identity | Child sessions are memory-only; close is permanent; no durable child resume/reconstruction. |
| Nested tasks | Host can spawn under a parent ID; depth is bounded | `allow_subagents` is parsed but unused by `TaskTree`; child runtimes are not activated with a child-owned tree and do not inherit Lua delegation tools. Recursive model-driven delegation is therefore incomplete. |
| Resource caps | Active/resident/direct-child/depth/mailbox/message/output/context byte limits | No aggregate delegated token/cost budget or automatic task deadline in `TaskTree`. Budget/timeout interruption enum values alone do not enforce either. |
| Child tool policy | Read-only default; write tools require configured upper bound, explicit opt-in and requested tool list | Root Lua permission/context hooks are removed from children; MCP and Lua tools are not automatically child-safe. Preserve restriction until explicit native policy exists. |
| Workspace isolation | Native file-tool workspace checks and bubblewrap for bash | Children share the inherited workspace/tool objects; no delegated worktree factory, conflict/merge workflow or per-child cwd in the spawn contract. |
| Cross-session coordination | Process-owned `Mailbox` / SQLite store / leases / claim-route-accept-ack | Strong existing feature. Do not conflate a durable mailbox entry with a durable child task or task completion. At-least-once delivery remains the contract. |
| Frontend parity | Shared session runtime and much shared task behavior | ACP `/tasks/*` uses a scratch runtime distinct from durable `/runs` sessions. This separation is deliberate and documented, but prevents session-scoped task integration. |

Primary Pici evidence:

- [Task requests, snapshots and limits](../src/core/agent_task.h:99).
- [Child creation and hook stripping](../src/core/agent_task.cpp:540), including
  `ChildSessionSpec.session_store = nullptr`.
- [Reservation and direct-child cap](../src/core/agent_task.cpp:630).
- [Child completion transition](../src/core/agent_task.cpp:971).
- [Wait implementation](../src/core/agent_task.cpp:1472).
- [Child runtime factory](../src/core/session/session_runtime.cpp:98): no
  child activation or automatic compaction configuration.
- [Activity bridge](../src/core/subagent_activity.cpp:70) and
  [REPL wake handling](../src/cli/cmd_run_session.h:2099): render activity,
  then process mailbox work, without injecting child completion input.
- [Agent add-on tools](../addons/agents.lua:25).
- [RPC dispatch](../src/cli/rpc_mode.cpp:580).
- [ACP scratch task runtime rationale](../src/acp/server.cpp:85).
- [Mailbox observation](../src/core/mailbox/mailbox_coordinator.cpp:525).

There is also a concrete misleading instruction to fix: both `agents.lua`
and the direct-child-limit error suggest `wait_agent` can free a resident
slot. `TaskTree::wait()` only observes; the parent child set remains until
close. A completed child can continue consuming residency and its name.

Status (task 174): fixed. Guidance and diagnostics now say `close_agent` frees
slots and `wait_agent` does not. `allow_subagents=true` is rejected by
`TaskTree::spawn()` (`invalid_state`) until child-owned trees exist (task 176).
Nonstreamed results share the streamed byte cap, inherited context is cut at
the first incomplete tool exchange, root `follow_up` is rejected, root status
derives from the owning `Agent`, and Lua `agents.wait` receives the tool's stop
token. `pici.run_agent`'s internal 1 s polling wait still ignores cancellation.

Additional source-derived correctness candidates from the Codex audit:

- Nonstreamed assistant text in `execute_work()` bypasses the output byte cap
  applied to streamed deltas (`agent_task.cpp:889`).
- Context truncation can leave an assistant's multiple tool calls with only
  some results: cleanup examines only a trailing assistant message, not every
  retained exchange (`agent_task.cpp:143–190`).
- Lua agent waits omit caller cancellation while RPC waits supply it
  (`cmd_run_session.h:1503`, `rpc_mode.cpp:261`).
- The synthetic root task has no runner; root follow-up can reserve work that
  nobody consumes, and root snapshots use initialized task status rather than
  the owning Agent's current state (`agent_task.cpp:330,377,1258`).

These need targeted reproductions before implementation. They are separate
from the observed terminal/readline test failures.

Typed, correlated user questions and approval replies are another missing
coordination contract. Pici has text conversation and Lua denial hooks; Codex
has request/result events and retained authorization evidence for child
reviewers (`tools/handlers/request_user_input*.rs`,
`agent/control/user_authorization.rs`). Any future implementation should use
request/response Values through the Session contract; Agent owns pending
execution state and native enforcement, and frontends own question presentation.
Also complete the process composition seam: `PiciProcess` owns shared facilities
but currently exposes no open-session factory or collection of live runtimes,
despite that target ownership in the taxonomy (`pici_process.h:59–117`).

## Broader comparison with Pi

| Area | Assessment | Evidence |
|---|---|---|
| Agent loop, streaming, cancellation, tool execution, steering and follow-up | Strong existing mechanisms; no need to port another loop | `src/core/agent*.{h,cpp}`, `vendor/pi/packages/agent/src/` |
| Durable sessions, forks and tree navigation | Implemented, with narrower automation surfaces | `src/core/session/session_store.h`, `src/cli/cmd_run_session.h` `/tree`; Pi `docs/sessions.md` and `docs/rpc-commands.md` |
| Compaction | Partial parity: Pici remote compaction is tied to `openai-codex-responses`; Pi can summarize with the selected model and preserve recent history | `src/core/compaction.cpp:40`, `src/core/llm_client.h:129`; Pi `docs/compaction.md:43` |
| MCP | Implemented stdio and streamable-HTTP (`McpHttpTransport`, libcurl) handshake/tool discovery/calls, bounded startup, cancellation, result truncation, reconnect of dead transports and `tools/list_changed` re-listing for new sessions | `src/core/mcp/mcp_client.h`, `mcp_config.h`, `mcp_tool.cpp`, `src/core/process/pici_process.cpp` |
| Rich MCP lifecycle | Missing from the present Pici contracts: server OAuth, resources, deferred tool discovery, standalone HTTP GET event stream, live tool-refresh UI for running sessions | Pi `docs/mcp.md`; Pici `McpClient` owns an `McpTransport` (stdio or HTTP) plus factory; `PiciProcess` owns the clients and `refresh_mcp_tools()` |
| Extension customization | Substantial Lua add-on system, skills, hooks, commands, tool formatting and reload exist | `src/core/lua_tool.h`, `addons/README.md`; Pi's TypeScript extension APIs are a different ecosystem, not a drop-in parity target |
| Extension distribution | Missing install/resource packaging and managed package distribution | `src/cli/session_runtime.cpp:17` explicitly uses a source-tree bundled path; Pi `docs/packages.md` supports npm/git packages |
| RPC automation | Partial parity: Pici has core prompt/model/session/task commands | Pi additionally documents queue clearing/modes, retry controls, standalone bash, stats, HTML export, entries/tree queries and command discovery in `docs/rpc-commands.md`; compare against Pici's explicit dispatch table |
| Provider coverage | Pici has OpenAI-compatible, Codex Responses and Muse Messages adapters, plus model/provider configuration | Pi documents additional distinct provider families (Bedrock, Vertex, Azure and others). OpenAI-compatible model names do not imply native protocol parity. |
| Prompt templates / named profiles / theme packages | No equivalent managed Pi resource system found in the inspected Pici configuration/CLI surfaces | Pi `docs/prompt-templates.md`, `docs/themes.md`, subagent `agents.ts`; Pici already has Lua formatting themes, which are useful but different |
| Project trust | No equivalent Pi project-trust workflow in Pici's traced explicit config/add-on loader | Pi `docs/security.md` protects project executable resources. Pici currently uses explicit config/file/directory loading; review trust before adding automatic project discovery. |

Do not mark plan mode, third-party UI compatibility, all Pi packages, or every
Codex product integration as mandatory Pici features. Preserve intentional
frontend exclusions and choose native contracts before copying wire formats.

Against Codex, Pici also lacks a model-facing resumable shell-session handle
(`unified_exec/write_stdin.rs` versus Pici's bounded Bash invocation), typed
plan/checklist updates, and native hosted web/image tools. External tools can
supply some of these capabilities. Managed worktree support exists in Codex,
but the audit does not establish that every child automatically gets one.
Codex v2 completion delivery is best effort and queue-only, and its unloaded
agent mailbox is in memory: do not claim guaranteed automatic parent execution
or crash-proof message delivery as Codex parity.

## C++ safety tooling

Local comparisons:

- `../agent-proxy/CMakeLists.txt:10` offers opt-in ASan/UBSan, required tidy,
  configurable tidy workers and `-Wall -Wextra -Wpedantic -Werror`.
  `../agent-proxy/tools/run_tidy.py` enumerates handwritten compile-database
  entries, runs independent processes and detects config-parser failures.
- `../llama.cpp/CMakeLists.txt:18` and
  `../whisper.cpp/CMakeLists.txt:43` offer separate address/thread/undefined
  sanitizer switches. Their warning lists include `-Wshadow`; Whisper also
  rejects C variable-length arrays. These checkouts are examples, not universal
  policies to copy wholesale.
- `prime-agent` is TypeScript and `capture` is Python/scripts; neither is a
  C++ safety-tooling reference in this checkout.

Before this change, Pici already had clang analyzer, bugprone, concurrency,
performance and core-guideline checks, plus a TSan target. Its tidy target
used `run-clang-tidy -j <CPU count>` when available but fell back to a serial
invocation. `cmake --build --parallel` did not parallelize that fallback.

This audit adds:

- A stdlib-only runner with one clang-tidy process per translation unit;
  automatic CPU/memory/job bounds, explicit `TIDY_JOBS`, timeout, nonzero
  failure propagation, optional strict diagnostics, and version-independent
  execution. It does not depend on `run-clang-tidy`.
- Runtime source discovery checked against the compilation database; the
  current build covers all 72 `src/**/*.cpp` translation units. Files excluded
  from a configured build are reported as skipped rather than silently lost.
- Serial `tidy-fix` by default to avoid races on shared-header edits. The
  existing `handlers.cpp` analyzer exclusion remains explicit; its historical
  rationale could not be established from the checked-out code.
- An opt-in ASan/UBSan build and Make target with TSan/jemalloc conflict checks.
  Instrumentation is applied to fetched dependencies too.
- Behavioral runner tests for actual overlapping processes, serial fixes,
  resource bounds, strict warnings (including paths with spaces), failure
  propagation, bad configuration and invalid options.

See [cpp-safety-tooling.md](../docs/cpp-safety-tooling.md) for commands and
limitations. Remaining safety work: automate gates in CI, add parser-focused
fuzzing, widen header analysis beyond the current core/http filter, document
the handlers analyzer exception, and introduce strict warnings incrementally.
Blanket `-Werror` or every hardening flag has not been added to an existing
warning-heavy codebase without measuring its impact.

## Proposed implementation order

Each item should be a bounded delegated task with a separate review. No new
runtime object is implied by this list: use existing owners and contracts.

Tracked backlog:

| Task | Scope |
|---|---|
| [174](https://tasks.home.nope.space/#/tasks/174) | Reproduce and fix delegation correctness issues |
| [175](https://tasks.home.nope.space/#/tasks/175) | Typed completion and RPC task events |
| [176](https://tasks.home.nope.space/#/tasks/176) | Recursive delegation and shared resource ceilings |
| [177](https://tasks.home.nope.space/#/tasks/177) | Durable child recovery and process composition |
| [178](https://tasks.home.nope.space/#/tasks/178) | Session-scoped ACP tasks and correlated human replies |
| [179](https://tasks.home.nope.space/#/tasks/179) | MCP transport and lifecycle expansion |
| [180](https://tasks.home.nope.space/#/tasks/180) | Repeatable safety gates and baseline terminal failures |

1. **Correct delegation policy and slot feedback.** Make `allow_subagents`
   truthful: enforce or explicitly reject unsupported recursive delegation;
   expose consistent schemas. Correct wait-versus-close guidance. Owner:
   agent-owned `TaskTree` Aggregate, activation lifetime, sole live task-status
   authority, consuming the Delegation contract. Characterize lifecycle and
   permission behavior before extending child activation.
   Include bounded nonstreamed output, complete tool-exchange context,
   cancellable Lua waits and truthful root pseudo-task behavior in this first
   correctness pass, using focused reproductions for each.
2. **Admit completion work to the parent.** Opt-in typed completion Value
   through session input admission, with task ID/run generation correlation,
   deduplication, bounded result content and cancellation handling. Parent
   `Agent` owns its input/run state; `TaskTree` owns child status. Never append
   directly to the parent's transcript from a renderer callback. Verify busy,
   idle, interrupted and shutdown parents, draft preservation and no wake loops.
3. **Bound delegated spend and time.** Agent/tree-scoped budget Values and
   inherited upper bounds, request-boundary admission, aggregate usage and
   deadline cancellation. Define what can overshoot while calls are in flight;
   do not label usage telemetry as budget enforcement.
4. **Persist child sessions and task relationships.** `SessionStore` remains
   the Store/durable authority; the owning `TaskTree` remains the live
   Aggregate. Journal spawn/terminal transitions before installing promised
   live state, recover interrupted activations explicitly, and distinguish
   reopening a durable session from resurrecting a closed activation.
5. **Make ACP tasks session-addressed.** Translate an explicit session ID at
   the frontend Adapter; route to that `SessionRuntime` and its Agent-owned
   tree. Preserve task-ID scope and existing clients through a deliberate wire
   compatibility design. Do not search all trees for `agent_1`.
6. **Improve write-enabled delegation.** Explicit ChildSessionSpec capability,
   sandbox/cwd and policy values; optional worktree operation owned by the
   process/session composition path. Establish cleanup/conflict semantics and
   native permission limits before granting Lua/MCP tools to children.
7. **Expand MCP in phases.** Phase 1 (transport Adapter, reconnect, tool-list
   refresh) is done (task 179); remaining:
   resources/deferred discovery and server-bound authentication. Process-owned
   reusable implementations and credential isolation must stay explicit.
8. **Provider-neutral compaction and packaging.** Reuse the compaction
   Operation/SessionStore transition; keep summary DTOs out of transcript
   authority. Install executable/resources together rather than assuming a
   checkout path. RPC convenience commands can follow shared domain contracts.

## Validation and execution record

- Work was fanned out in the `pici-review` tmux session using tracker tasks
  [170](https://tasks.home.nope.space/#/tasks/170),
  [171](https://tasks.home.nope.space/#/tasks/171),
  [172](https://tasks.home.nope.space/#/tasks/172), and
  [173](https://tasks.home.nope.space/#/tasks/173).
- Claude completed the bounded tooling implementation. One Codex audit worker
  hit model capacity; the primary agent completed the Pi/safety review. Task
  events and tmux completion signals were used to observe worker completion.
- Existing focused coordination/mailbox/RPC/frontend tests: 9/9 suites passed.
- Existing full CTest run: 48/50 suites passed. Failures:
  `Terminal.TerminalTitleHelpers` (`tmp` instead of the requested nonexistent
  directory basename) and `ReadlineEnvironment.test_word_motion` (PTY output
  mismatch). This audit changes no application code. These were observed
  before rebuilding and must not be represented as a clean full gate.
- Final full build succeeded with two build workers. After rebuilding and
  reconfiguring the final patch, CTest passed **50/51 suites**, including
  `test-tidy-runner`. `Terminal.TerminalTitleHelpers` still failed; readline
  passed on this run, so its earlier PTY failure needs reproducibility work.
  The first build attempt hit the sandbox's read-only default ccache location;
  using ccache read-only with a writable `/tmp` temporary directory succeeded.
- Runner regression tests: 7 passed and are registered as `test-tidy-runner`
  when Python and repository tests are enabled. CMake configured successfully; generated
  tidy wiring and 72-file coverage were checked.
- Claude ran one real clang-tidy translation unit (`src/acp/types.cpp`) with
  LLVM 18 successfully; the complete real lint has not been run. Fixture
  concurrency is functional evidence, not a whole-repository timing benchmark.
- ASan/UBSan configure/build/execution smoke and incompatible-option checks
  (C-only TSan, combined/configuration-specific TSan and jemalloc) passed;
  a complete sanitized build/test run remains outstanding.
- No commit, deployment, journal/schema migration or new application feature
  was performed. Runtime features above are proposed work, not shipped claims.
