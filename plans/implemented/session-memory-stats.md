# Per-session memory accounting for pici

## Status

Implementation plan. All four phases implemented; Phases 1–2 verified and
committed (`7da239d`), Phases 3–4 verified 2026-08-23, uncommitted.
Targets the current tree as of 2026-08-22
(`src/core/agent_task.{h,cpp}` `AgentTaskManager`, `src/core/session/agent_session.{h,cpp}`,
`src/core/agent.cpp`, `src/core/stream.h`, `src/main.cpp` `cmd_run`).

Reviewed by Opus (2026-08-22) before implementation start. That review found
the original binding strategy in §Design 2 didn't match this codebase's
threading model (it would have measured almost nothing) and two jemalloc API
names were wrong. This revision folds in the required fixes; superseded
text is not reproduced.

### Implementation status (2026-08-22)

- **Phase 1 — done.** `composition_report_for_messages()` and
  `AgentTaskManager::composition_report[s]()` in `agent_task.{h,cpp}`;
  `/memory` in the dispatch chain (`main.cpp:2436`) and completion list
  (`main.cpp:2020`); composition panel rendered by `format_memory_composition`
  (`main.cpp:978`). Verified live: fresh session shows root 0 B; after 25
  scripted faux-control turns, root 38.5 KB all-text (matches expectation).
  Helper kept at `scripts/faux_grow_session.py` for future verification runs.
- **Phase 2 — done.** `src/core/memory_stats.{h,cpp}` (the internal impl
  header was folded into memory_stats.h after an include-order breakage);
  CMake option + TSan FATAL_ERROR guard (`CMakeLists.txt:506-523`);
  `test/test_memory_stats.cpp` registered as `test-memory_stats`. Full suite
  green (35/35) in default build AND with `-DPI_CPP_MEMSTATS=ON`; TSan+
  memstats configure refuses with the intended fatal error; arena
  round-trip test passes under `LD_PRELOAD=libjemalloc.so.2` on the memstats
  build. Fix found during verification: jemalloc caches every `stats.*`
  value per-epoch — reads must write the `epoch` mallctl first or they
  return process-start values forever (canary always failed without it).
  The startup canary now passes under real preload and correctly rejects
  non-jemalloc runs (glibc build stays "unavailable", no crash).
  Note: `test_arena_round_trip` exercises the memory_stats module directly;
  nothing in the agent/task path uses the module until Phase 3.
- **Phase 3 — done (2026-08-23).** All three spawn points wrapped with
  `inherit_arena()`: `Agent::launch_worker_locked`, `EventStream::start_worker`,
  and `AgentTaskManager::spawn`'s runner jthread. Child arenas are acquired in
  `make_task()` on the spawning thread immediately before `owned_session`
  construction (guard restores the spawning thread's context on unwind) and
  released in `close_tasks()` strictly after all runner threads join; the
  spawn-failure path also releases. Root arena acquired exactly once by
  `bind_root_arena()` (main thread, before the interactive loop) or lazily by
  `heap_reports()` under the manager mutex. Acceptance test
  (`test_child_task_arena_attribution`): a child task whose client allocates a
  2 MiB response lands ≥ half of it in the child's arena while root stays flat
  (< quarter); passes under real `LD_PRELOAD=libjemalloc.so.2`. Full suites
  green in default AND memstats+preload builds (35/35 each).
- **Phase 4 — done (2026-08-23).** Heap panel added above the composition
  panel (`format_memory_heap`, main.cpp); shared is computed as
  `stats.allocated − Σ(session arenas)` per §Design 4's consistency identity,
  clamped at zero against cross-arena epoch drift. README gained a `/memory`
  section (two-panel model, enable recipes, accuracy caveats). Verified live
  in tmux under jemalloc preload: fresh session shows root 0 B / shared
  1.6 MB / resident 5.2 MB / RSS 25.6 MB; repeated `/memory` stable (no
  double-acquire). Default build prints only composition + the enable hint.

### Remaining verification notes (post-implementation)

1. Phase 3 acceptance (child-task arena attribution) is covered by the
   automated test above, which runs in CI whenever the memstats build is
   exercised under preload. A manual interactive pass with a real child task
   spawned mid-session has not been driven end-to-end from tmux — the REPL
   currently exposes no interactive task-spawn command, so the automated
   path (same wiring, same spawn chain) is the evidence.
2. Default-renderer TTY check of `/memory`: **done** (2026-08-23, tmux +
   snapshot harness, jemalloc-preloaded memstats build). Both panels render
   correctly and repeatedly; the previously suspected input-focus quirk did
   not reproduce. One caveat observed live: after binding at startup, an
   idle session's root reads exactly 0 B — pre-bound startup allocations
   were served from jemalloc's default pool before the bind, and tcache
   serves subsequent small frees/allocs without touching the arena. This is
   plan §Risks 2 working as documented, not a wiring bug: attribution moves
   to the right arena under genuine allocation churn (see the acceptance
   test), and shared absorbs the startup tail.

## Goal

Answer, from inside a running pici process: **how much memory is in use, per
session, and what is it spent on** — surfaced via a new `/memory` REPL
command. "Per session" is meaningful because a single pici process can host
more than one session concurrently: the root `AgentSession` plus up to
`AgentTaskManager::Limits::max_resident_tasks` (default 8, `agent_task.h:199`)
child agent-task sessions, each a real in-process `AgentSession` object
(`agent_task.cpp:482-489`) running its own turn loop. These are not
subprocesses — no `fork`/`exec` anywhere in `agent_task.cpp` or `mailbox/*` —
so their memory is genuinely commingled in one process's heap today, and `ps`/
`/proc/<pid>/smaps` (see chat transcript, 2026-08-21) cannot tell them apart.

This plan chose **true allocator-level accounting** over cheaper
JSON-transcript-size estimation: real malloc bytes attributed to whichever
session's arena requested them, covering Lua VM state, HTTP/TLS buffers, and
other overhead a transcript-size estimate would miss — not just message
payloads. The transcript-size estimate is still built (§Design 3), but as an
independent, differently-labeled panel, not a sub-breakdown of the arena
number — see §Design 3 for why those two numbers must not be nested.

## Non-goals for v1

- **Call-stack / heap-profiling ("by function") breakdown.** jemalloc's
  `--enable-prof` + `jeprof` gives sampled allocation call sites, but it's a
  sampling, offline-analysis-shaped feature that doesn't map cleanly onto
  "session" boundaries anyway. Out of scope.
- **Precise attribution of shared/background threads** (region renderer's
  `paint_loop`, mailbox coordinator, SQLite's single shared connection —
  `session_store.cpp:241-409`). These stay on jemalloc's default arena pool
  and are reported as a single "shared" bucket, not broken down further.
- **Continuous/live monitoring.** `/memory` is a point-in-time snapshot
  command, matching `/tree` and `/model`.
- **Windows.** No `WIN32` branch exists in `CMakeLists.txt` today.
- **Exact accounting.** tcache lag (§Risks 3) and the shared single-
  connection SQLite cache (§Design 3) both mean session numbers are good
  attribution, not audited-accurate ones. The plan optimizes for "which
  session is heavy and roughly why," not forensic precision.

## Existing infrastructure this builds on

| Piece | Location | Reuse |
|-------|----------|-------|
| Per-message JSON byte size | `message_bytes()`, `agent_task.cpp:68-70` | Content-composition panel (§Design 3) |
| Child-task transcript byte summation | `AgentTaskManager::snapshot()`, `agent_task.cpp:327-341` building `AgentTaskContextInfo::context_bytes` (`agent_task.h:96-103`) | Extend the same summation to the root session, which currently has no equivalent; compute the content-type split in the same pass (don't re-serialize) |
| Turn-spawn call chain | `Agent::launch_worker_locked` (`agent.cpp:471`) → `run_agent_loop_envelopes` (`agent_loop.cpp:1274`) → `EventStream::start_worker` (`stream.h:49`) | The three points arena-context inheritance must wrap (§Design 2) |
| Per-turn transcript deep copy | `Agent::create_context_snapshot()`, `agent.cpp:143`, `494-503` — copies `state_.messages()` in full every turn | Explains why arena bytes can run well above the transcript-JSON estimate; call this out in the UI, don't hide it |
| Child task lifecycle / runner thread | Runner jthread spawn `agent_task.cpp:638`; task spawn/`owned_session` construction `agent_task.cpp:482-489`; `run_task()` `agent_task.cpp:754`; close/join `agent_task.cpp:1141-1145` | Arena bind-before-construct and recycle-on-close hook points |
| pkg-config for a native dependency | `pkg_check_modules(LUA REQUIRED IMPORTED_TARGET lua5.4)`, `CMakeLists.txt:426-429` | Same shape for jemalloc's fallback link path (§Design 5) |
| Optional heavy dependency gated behind a CMake option | `PI_CPP_OTEL_API`/`PI_CPP_OTEL_SDK` (`CMakeLists.txt:10-11`), `PI_CPP_OTEL_ENABLED` compile define (`CMakeLists.txt:527-539`) | Same pattern for a new `PI_CPP_MEMSTATS` option |
| TSan build | `TSAN_CONFIGURE_FLAGS`, `Makefile:15-17` | `PI_CPP_MEMSTATS` must refuse to combine with `-fsanitize=` (§Design 5, §Risks 6) |
| Slash-command dispatch | `main.cpp:2207-2450` (`/tools` at 2209, `/skills` at 2213, `/model` at 2279, `/tree` at 2370) — **not** `main.cpp:1829`, which is inside `reload_addons` and was a stale citation inherited from `plans/agent-skills.md:59` | `/memory` joins this chain |
| Slash-command tab completion | `main.cpp:1939-1946` | `/memory` must be added here too or it won't complete like its siblings |

## Design

### 1. Allocator: jemalloc, resolved at runtime, not linked at build time

jemalloc gives per-arena allocation stats and lets a thread bind to an
explicit arena via the `thread.arena` mallctl — the mechanism this plan
needs to attribute allocations to a session, which glibc malloc has no
equivalent for.

**Do not link jemalloc into the binary.** Resolve `mallctl` via
`dlsym(RTLD_DEFAULT, "mallctl")` at startup; `memory_stats_available()` is
true iff that resolves *and* a startup canary (allocate a few MB through the
normal `new`/`malloc` path, confirm `stats.allocated` moved by roughly that
much) confirms the process's global allocator is actually jemalloc — a
resolved symbol alone doesn't prove the override took effect (weak symbol
resolution, link order, or `--as-needed` can all leave glibc malloc in
charge even with jemalloc present in the process). The user opts in per-run
via `LD_PRELOAD=libjemalloc.so.2 pi-cli` (macOS: `DYLD_INSERT_LIBRARIES`).

This eliminates the build-time risk entirely: there is no separate
"memstats build" to regress the default build, no vendoring, and the exact
same release binary reports real numbers whenever preloaded. See §Design 5
for the CMake-level fallback (linking against a system jemalloc via
pkg-config) kept only for the case where `LD_PRELOAD` isn't practical (e.g.
a locked-down deployment); it's the secondary path, not the primary one.

### 2. Arena-context inheritance — new module `src/core/memory_stats.{h,cpp}`

```cpp
namespace pi::core {

// True only when dlsym resolved mallctl AND the startup canary confirmed
// jemalloc is the active global allocator. Every other function below is a
// safe no-op/nullopt when this is false, so call sites never need their own
// #ifdef or availability check.
bool memory_stats_available();

struct SessionArena {
  unsigned index;
};

// Creates a new arena, or reuses one from the recycle pool (see "no
// destroy" below). Never destroyed for the life of the process; recycled
// between sessions instead.
std::optional<SessionArena> acquire_session_arena();

// Unbinds any thread still bound to it (there shouldn't be one — call this
// only after the session's threads have all joined), purges its pages back
// to the OS, and returns the index to the recycle pool. Does NOT call
// arena.<i>.destroy — see §Risks 4 for why that call is unsafe here.
void release_session_arena(SessionArena);

// Reads/sets the calling thread's arena-context TLS slot. bind_current_
// thread also issues the real jemalloc thread.arena mallctl; the TLS slot
// additionally lets a spawning thread capture "what arena am I in" and pass
// it to a child thread it's about to create, which is the actual mechanism
// this plan depends on (see below).
std::optional<SessionArena> current_arena();
void bind_current_thread(SessionArena);
void unbind_current_thread();                    // back to jemalloc's default pool

// Wraps a callable so that, when actually invoked (on whatever thread that
// turns out to be), it first binds that thread to the arena captured from
// the calling thread's TLS at wrap time, runs the callable, then restores
// the previous binding. A no-op passthrough when memory_stats_available()
// is false. This is the piece that makes arena attribution survive the
// jthread-per-turn spawn chain (see call sites below) instead of only
// covering whichever thread happened to call acquire_session_arena().
template <typename F> auto inherit_arena(F &&f);

struct ArenaStats {
  std::uint64_t allocated_bytes; // stats.arenas.<i>.small.allocated +
                                  // stats.arenas.<i>.large.allocated —
                                  // there is no stats.arenas.<i>.allocated
};
std::optional<ArenaStats> read_arena_stats(SessionArena);

struct ProcessMemorySnapshot {
  std::uint64_t rss_bytes;                     // getrusage/proc; always available
  std::optional<std::uint64_t> allocator_allocated_bytes; // stats.allocated (live malloc bytes, process-wide)
  std::optional<std::uint64_t> allocator_resident_bytes;  // stats.resident (allocator-owned physical pages)
};
ProcessMemorySnapshot read_process_snapshot();

}
```

**Why binding two threads (the original design) doesn't work.** A turn does
not execute on the thread that requested it. Tracing the actual call chain:
`AgentSession::run_messages` calls `Agent::prompt`, which spawns a fresh
`std::jthread` per call (`launch_worker_locked`, `agent.cpp:471`, pushed
into `workers_` at `agent.cpp:479`). That worker calls
`run_agent_loop_envelopes` (`agent_loop.cpp:1274`), which calls
`EventStream::start_worker` (`stream.h:49`) — **another** fresh jthread. It
is that innermost thread that does essentially all the real allocation:
`client->stream(...)` (`agent_loop.cpp:670`), the HTTP/TLS round trip, SSE
and JSON parsing, and assistant-message accumulation. Binding only the main
thread (root) and only `run_task()`'s runner thread (children) — the
original plan — binds neither of the threads that actually allocate; nearly
everything would land in the shared bucket regardless of which session
triggered it.

**The fix: inherit arena context across every thread-spawn point in the
turn path**, not just bind two fixed threads:

- `Agent::launch_worker_locked` (`agent.cpp:471`) — wrap the worker's
  callable with `inherit_arena(...)` before `workers_.emplace_back(...)`.
- `EventStream::start_worker` (`stream.h:49`) — same wrap around whatever
  callable it hands to its own `jthread`.
- `AgentTaskManager`'s runner jthread spawn (`agent_task.cpp:638`) — same
  wrap, so a child task's whole call chain (which re-enters the same
  `launch_worker_locked`/`start_worker` path via its own `owned_session`)
  inherits *that task's* arena rather than the spawning thread's.

Each wrap reads `current_arena()` at spawn time (on the parent thread, where
it's already correct) and re-binds it as the very first action once the
wrapped callable actually starts running on the new thread. Since
`agent.cpp`/`stream.h` are hot, generic infrastructure shared by every
caller, `inherit_arena()` must be a zero-cost passthrough
(`return std::forward<F>(f);`) when `memory_stats_available()` is false at
compile/link time — this is a diagnostic feature and must not add overhead
to the default build's threading path.

**Bind before construction, not "at the top of `run_task()`".** A child
`AgentSession` is constructed on the *spawning* thread
(`agent_task.cpp:482`), before `run_task()`'s runner thread even exists —
system prompt assembly, tool-definition copies, and config all happen there.
`acquire_session_arena()` + `bind_current_thread()` must run immediately
before that construction, on the spawning thread, so the arena is already
current in TLS by the time `inherit_arena()` captures it for the runner
jthread and everything downstream.

**No `destroy`, only purge-and-recycle.** The original plan called
`arena.<i>.destroy` when a task closed. Per jemalloc's own documented
contract (shared with `arena.<i>.reset`): *"None of the arena's
discarded/cached allocations may be accessed afterward... all thread caches
which were used to allocate/deallocate in conjunction with the arena must be
flushed beforehand."* In this codebase that precondition is not
satisfiable — a closed task's `AgentTaskResult::text`
(`agent_task.cpp:808-826`) and any `ChildAgentEvent` payloads it emitted are
read by the *parent* after the child's arena would be destroyed, and
short-lived per-turn threads routinely free each other's allocations.
Destroying under those conditions is a use-after-free generator, not a
"refuse or leak" as originally (wrongly) written. `release_session_arena()`
instead unbinds, calls `arena.<i>.purge` (always safe — only returns
currently-free pages to the OS), and returns the index to a small recycle
pool that `acquire_session_arena()` draws from for the next task. Accept
that a recycled arena carries a stale tail of the previous occupant's freed-
but-not-yet-reused allocations as a known, documented accuracy cost — the
alternative is unsafe.

### 3. The content-composition panel: independent, not a sub-breakdown

Reuse `message_bytes()` (`agent_task.cpp:68-70`), applied per content block
instead of per whole message, to get a JSON-wire-size estimate split by
block kind (`text` / `tool_use` / `tool_result` / images), for the root
session and every live child. Extend `AgentTaskManager::snapshot()`'s
existing per-message summation (`agent_task.cpp:334-335`) to compute the
per-content-type split **in the same pass** it already serializes each
message — don't add a second full-transcript JSON pass, since `snapshot()`
is already on paths like `/tasks` that get called often.

**This must be presented as an independent panel with its own units, not
nested inside the arena number as "of which."** `message_bytes()` is
escaped-JSON wire size, not heap size, and the two diverge in both
directions:

- Base64-encoded images inflate JSON to ~1.33× the decoded bytes actually on
  the heap.
- The transcript exists in **multiple simultaneous heap copies**:
  `Agent::create_context_snapshot()` (`agent.cpp:143`, `494-503`) deep-copies
  the *entire* message vector every turn before it's moved into the loop
  worker and serialized again into the request body. Real per-session heap
  for a transcript is plausibly 3-5× the JSON-size estimate at peak, and can
  legitimately exceed it in bursts — not a bug, just a different quantity.

So: two panels, two units, no containment claim between them.

```
=== heap (real bytes, requires -DPI_CPP_MEMSTATS / LD_PRELOAD jemalloc) ===
session              arena allocated
root                 12.4 MB
task #3 "luna"        2.1 MB
task #7 "echo"        0.9 MB
------------------------------------------
shared (unattributed)24.3 MB   [stats.allocated − Σ(arenas above)]
allocator resident   41.0 MB   [stats.resident]
process RSS          43.1 MB

=== context composition (JSON wire bytes — what you pay tokens for) ===
session              transcript    text    tool_result   tool_use
root                    9.8 MB    6.1 MB     3.0 MB       0.7 MB
task #3 "luna"          1.6 MB    1.0 MB     0.5 MB       0.1 MB
task #7 "echo"          0.4 MB    0.3 MB     0.1 MB       0.0 MB
```

Without `-DPI_CPP_MEMSTATS`/an active `LD_PRELOAD`, only the second panel
prints, with a one-line note explaining how to get the first.

### 4. Reporting structs and the `/memory` command

```cpp
struct SessionHeapReport {          // §Design 2's panel
  std::string label;                // "root" or task name/path
  std::optional<ArenaStats> arena;  // nullopt if memory_stats_available() is false
};

struct SessionCompositionReport {   // §Design 3's panel
  std::string label;
  std::size_t transcript_bytes{0};
  std::size_t text_bytes{0};
  std::size_t tool_use_bytes{0};
  std::size_t tool_result_bytes{0};
};
```

`AgentTaskManager` gains a method building both reports per live task in one
pass; the root session's equivalent is built in `main.cpp`, since the root
`AgentSession` lives there. The `/memory` command:

- Joins the slash-command dispatch chain at `main.cpp:2207-2450` (alongside
  `/tools` at 2209, `/skills` at 2213) — **not** `main.cpp:1829`, which is
  inside `reload_addons`.
- Is added to the tab-completion list at `main.cpp:1939-1946` so it
  completes like its siblings.
- Collects root + all live child reports, `read_process_snapshot()`, and
  prints both panels per §Design 3's layout.

### 5. Build integration

Primary path is runtime-only (§Design 1: `dlsym` + `LD_PRELOAD`, no CMake
changes at all beyond the `PI_CPP_MEMSTATS` option gating whether
`memory_stats.cpp`'s real implementation or its stub compiles in).

Secondary/fallback path, for environments where `LD_PRELOAD` isn't
practical — link a system jemalloc via pkg-config, matching this repo's
existing convention for exactly this dependency shape
(`pkg_check_modules(LUA REQUIRED IMPORTED_TARGET lua5.4)`,
`CMakeLists.txt:426-429`):

```cmake
option(PI_CPP_MEMSTATS "Compile real per-session memory accounting (/memory command)" OFF)

if(PI_CPP_MEMSTATS)
    if(CMAKE_CXX_FLAGS MATCHES "-fsanitize=" OR TSAN_CONFIGURE_FLAGS)
        message(FATAL_ERROR
            "PI_CPP_MEMSTATS is incompatible with sanitizer builds: "
            "jemalloc's malloc override and TSan's malloc interceptors conflict.")
    endif()
    target_compile_definitions(pi-core PUBLIC PI_CPP_MEMSTATS_ENABLED)
    # No link step for the dlsym/LD_PRELOAD path. Only if the fallback
    # link-time path is explicitly requested:
    if(PI_CPP_MEMSTATS_LINK_JEMALLOC)
        pkg_check_modules(JEMALLOC REQUIRED IMPORTED_TARGET jemalloc)
        target_link_libraries(pi-core PUBLIC PkgConfig::JEMALLOC)
    endif()
endif()
```

`memory_stats.cpp` compiles unconditionally; internally `#ifdef
PI_CPP_MEMSTATS_ENABLED` gates the real dlsym-based implementation, stub
otherwise, so no call site anywhere needs its own `#ifdef`.

Never build `PI_CPP_MEMSTATS` alongside `make check`'s TSan build
(`Makefile:15-17`) — guarded above with a hard `FATAL_ERROR`, not just a
doc note, since a silent conflict here would surface as a confusing crash
rather than a configure-time error.

## Phases

Reordered from the original draft: the transcript/composition panel has no
jemalloc dependency and is independently useful, so it ships first. The
arena-inheritance mechanism (§Design 2) is the phase most likely to need
iteration — it lands once its foundation (the command + report structs)
already exists and is visibly useful on its own.

### Phase 1 — content-composition panel + `/memory` command skeleton

- Extend `message_bytes()`-based summation to the root session's
  `agent.state().messages()`, computing the per-content-block-kind split in
  one pass (§Design 3); do the same in `AgentTaskManager::snapshot()`'s
  existing loop for child tasks, without adding a second serialization pass.
- Add `/memory` to the dispatch chain (`main.cpp:2207-2450`) and completion
  list (`main.cpp:1939-1946`), printing only the composition panel (no heap
  panel yet — `memory_stats_available()` doesn't exist until Phase 2/3).
- **Acceptance**: `/memory` in a live TTY session shows a plausible,
  growing breakdown for the root session and any live child tasks.

### Phase 2 — `memory_stats` module + build integration (stub-first)

- Add `src/core/memory_stats.{h,cpp}` per §Design 2's API, with the
  dlsym-based real implementation behind `PI_CPP_MEMSTATS_ENABLED` and a
  safe stub otherwise (§Design 5).
- Startup canary: allocate through the normal path, confirm
  `stats.allocated` moved, before `memory_stats_available()` ever returns
  true — this is what actually protects against link order/`--as-needed`
  silently leaving glibc malloc in charge.
- Add `test/test_memory_stats.cpp`: with `-DPI_CPP_MEMSTATS=ON` and
  `LD_PRELOAD`'d jemalloc, arena acquire/bind/read/release round-trips;
  without it, `memory_stats_available() == false` and every call a safe
  no-op. Register `test-memory-stats` in the CMake `foreach` block
  (`CMakeLists.txt:584`).
- **Acceptance**: full test suite green both under
  `LD_PRELOAD=libjemalloc.so.2 ... -DPI_CPP_MEMSTATS=ON` and the default
  build with the flag off; the TSan build (`make check`) refuses to
  configure with `PI_CPP_MEMSTATS=ON` (hard error, not silent skip).

### Phase 3 — arena-context inheritance + wiring

- Implement `inherit_arena()` and wrap the three spawn points identified in
  §Design 2: `agent.cpp:471`, `stream.h:49`, `agent_task.cpp:638`.
- Bind a root arena on the main thread once, before the interactive loop
  starts. Bind a child task's arena *before* `owned_session` construction
  (`agent_task.cpp:482`), not at `run_task()` entry.
- Wire `release_session_arena()` into the task-close path, strictly after
  the runner thread is joined (`agent_task.cpp:1141-1145`).
- **Acceptance** (this is the test that would have caught the original
  design's flaw): with jemalloc preloaded, spawning a child agent task and
  running turns on it measurably moves *that task's* arena bytes — not the
  root's, not "shared" — confirmed by running a turn heavy on tool output
  and checking the delta lands in the expected arena.

### Phase 4 — full `/memory` table + docs

- Add the heap panel (§Design 3's first table) alongside the existing
  composition panel; wire `read_process_snapshot()`'s `rss_bytes`,
  `stats.allocated`, `stats.resident`.
- Verify `Σ(session arenas) + shared == stats.allocated` exactly (both are
  live-malloc-byte quantities in the same units — this is the meaningful
  internal-consistency check; process RSS agreeing with `ps` "within noise"
  is not, since RSS includes fragmentation/text/mmap and was never expected
  to equal allocator bytes).
- README section: `/memory`, the two-panel model and why they're not
  nested, how to enable (`LD_PRELOAD` vs. `-DPI_CPP_MEMSTATS_LINK_JEMALLOC`),
  and the known accuracy caveats (tcache lag, recycled-arena stale tail,
  shared SQLite connection).

### Future (not in this plan's milestones)

- `LuaUiContext`/status-line exposure for live memory (same shape as the
  costline live-update work).
- jemalloc heap-profiling (`--enable-prof` + `jeprof`) for true call-site
  attribution, if arena-level numbers turn out to be too coarse in practice.
- `MALLOC_CONF=tcache:false` as a documented "accurate mode" run recipe, if
  tcache lag (§Risks 3) proves confusing in practice.

## Risks and mitigations

1. **Arena-context inheritance correctness.** The mechanism now depends on
   wrapping every thread-spawn point in the turn path, not just two fixed
   threads — missing a spawn site (e.g. a future tool-execution thread pool)
   silently drops that work into "shared" rather than erroring. Mitigated
   by Phase 3's acceptance test being specifically designed to catch
   misattribution, and by keeping the wrap points to the three call sites
   enumerated in §Design 2 rather than something more implicit.
2. **tcache lag.** Freed regions sitting in a thread's tcache still count as
   allocated by the owning arena until flushed; cross-thread frees are
   common here (main thread freeing strings a loop worker allocated), so a
   session's number can be transiently inflated. Bounded and self-correcting
   at thread exit. Document it; `MALLOC_CONF=tcache:false` available as an
   accurate-but-slower mode if needed (§Future).
3. **`stats.allocated`/`stats.resident` require jemalloc's stats support**,
   which is the default in jemalloc 5.x (`--disable-stats` is the opt-out,
   not the reverse) — lower risk than originally estimated, but
   `memory_stats_available()`'s startup canary must still fail gracefully
   (not crash) if a distro shipped a stats-disabled build.
4. **Recycled-arena stale tail.** Since arenas are never destroyed (§Design
   2), a recycled arena's reported bytes include whatever the previous
   occupant hadn't freed yet at recycle time until purge/reuse clears it.
   Documented as a known accuracy limit (§Non-goals), not silently hidden.
5. **Shared-resource smear.** SQLite's page cache lives on one connection
   shared by `SessionStore` across all sessions
   (`session_store.cpp:241-409`); OpenSSL/curl similarly have process- or
   thread- level shared state. Whichever session's thread first triggers
   growth in these gets charged for it. Not fixable without much heavier
   instrumentation; the README (Phase 4) must say so plainly so the numbers
   aren't read as more precise than they are.
6. **Sanitizer conflict.** jemalloc's malloc override and TSan's interceptors
   are mutually exclusive. Guarded by a hard `FATAL_ERROR` at configure time
   (§Design 5) rather than relying on the default-off option to keep it out
   of `make check` by convention alone.

## Open questions

- Should `/memory` be TTY-only (like `/tree`/`/model`) or also available via
  `--print-mode`/one-shot invocation for scripting? Default plan: TTY-only
  for v1.
- (Resolved by review) A system-package/`LD_PRELOAD`-time jemalloc is the
  accepted path — no vendoring, no build-time hard dependency. The
  `PI_CPP_MEMSTATS_LINK_JEMALLOC` fallback exists only for environments
  where `LD_PRELOAD` genuinely isn't an option.
