# Object taxonomy migration progress

This checkpoint is updated in every milestone commit. The authoritative plan
is [`plans/object-taxonomy-migration.md`](object-taxonomy-migration.md).

Branch: `trunk`

Completed milestones:

- Phase 0: `29c618b` — `docs: establish object taxonomy guidance`
- Phase 1: `383b5a2` — `test: characterize model catalog boundaries`
- Phase 2: `777ea9e` — `refactor: establish model catalog taxonomy`
- Phase 3: `669df17` — `feat: add provider model discovery bindings`
- Phase 4: `1de80bb` — `feat: make Authentication the process aggregate`
- Phase 5: `cfa2872` — `feat: introduce PiciProcess as the composition root`
- Phase 6: `04762ac` — `refactor: narrow SessionRuntime and centralize model-switch auth checks`
- Phase 7: complete in the commit containing this progress update, planned
  subject `refactor: move TaskTree under Agent`

Next milestone: Phase 8 — make `Mailbox` process-owned and sessions attach to
it.

Phase 4 verification: full `make test` passed 44/44 tests; `make format` and
`git diff --check` passed. Full-repo `make lint` is too slow to complete
interactively on this machine (multi-file `run-clang-tidy` run exceeded a
10-minute background budget without finishing); verification instead used
targeted `clang-tidy` invocations per changed translation unit, each compared
against the same file linted at the pre-Phase-4 commit (`669df17`, via
`git stash`) to isolate genuinely new diagnostics from pre-existing debt.
Result: `authentication.cpp`, `authentication.h`, `authentication_adapter.h`,
`auth_resolver.h`, `handlers.cpp`, `rpc_mode.cpp`, `main.cpp` (both), and
`test_authentication.cpp` are fully clean; `openai_codex_oauth.cpp`,
`models.cpp`, `session_runtime.cpp`, and `session_runtime_bundle.cpp` report
only pre-existing diagnostics (`login_device`/`resolve`/`find_model`/
`open_session_runtime` complexity and long-standing `misc-include-cleaner`
gaps), each verified to have identical warning counts before and after the
phase's diff. No new diagnostic was introduced. The stale
`src/core/auth/auth_resolver.cpp` compatibility translation unit (now
content-free after `AuthResolver` became a type alias) was deleted rather than
patched around, and `CMakeLists.txt`'s `pi-http` source list was updated to
match. A leftover `src/core/models.cpp.orig` backup file from a prior
in-progress edit was also removed.

Phase 5 verification: implemented by a delegated subagent against a detailed
scoped brief, then independently rebuilt/retested/lint-diffed by the
orchestrating session before commit. Full `make test` passed 45/45 (new
`test-pici_process`, 4 cases); `make format` and `git diff --check` passed.
Same targeted-`clang-tidy`-vs-pre-phase-commit method as Phase 4 (full
`make lint` still impractical here). New files `src/core/process/pici_process.{h,cpp}`
and `test/test_pici_process.cpp` are fully clean. `src/acp/main.cpp` matches
its pre-phase baseline exactly (one pre-existing `main` complexity warning).
`src/cli/session_runtime_bundle.cpp`'s pre-existing `open_runtime_bundle`
(née `open_session_runtime`) cognitive-complexity warning was at risk of
growing from 52 to 54 because of the added session-store injection branch;
extracting a small `resolve_session_store()` helper brought it down to 51
instead, so the phase left that pre-existing debt slightly better than it
found it rather than slightly worse. All other touched files matched their
pre-phase `misc-include-cleaner` baselines exactly.

One real deviation from the subagent's initial pass was caught and fixed
during review, worth flagging for future phases: wiring
`cfg.session_store = process.session_store()` unconditionally in
`src/acp/main.cpp` would have silently changed ACP's default session storage
from a per-run ephemeral temp directory (`src/acp/server.cpp`'s
`run_server()` fallback) to the CLI's persistent, shared
`SessionStore::default_sessions_dir()` -- a real behavior change outside
Phase 5's stated scope, of exactly the kind the plan's "stop rather than
guessing" rule is about. Fixed by only injecting the process's session store
when `--session-dir` is explicitly set, preserving ACP's ephemeral-by-default
behavior byte-for-byte while still sharing the process-owned store when a
directory is actually named.

`PiciProcess` (`src/core/process/pici_process.h`) owns process-lifetime
`ModelCatalog`, `Authentication` (sharing one process-owned
`auth::CredentialStore`, via `Authentication`'s two-argument constructor
rather than letting it silently build its own), and `SessionStore`. Mailbox
ownership is deliberately NOT here: `MailboxCoordinatorOptions` requires
`root_agent_id`/`initial_session_id`, known only once a specific session is
opening, not at bare process construction -- moving it in now would require
Phase 8's multi-attachment redesign ahead of schedule. `cli::SessionRuntimeConfig`/
`SessionRuntimeBundle`/`open_session_runtime`/`activate_session_runtime` were
renamed to `RuntimeBuildConfig`/`RuntimeBundle`/`open_runtime_bundle`/
`activate_runtime_bundle` (file `src/cli/session_runtime.h` itself was not
renamed) so they read as clearly distinct from the real `core::SessionRuntime`
object once Phase 6 renames `agent_session.{h,cpp}` to `session_runtime.{h,cpp}`.
`cli::cmd_run()`/`CmdRunSession` gained optional injected
`authentication`/`session_store` parameters (default null, so every existing
2-arg call site is unaffected); `pi-cli`'s `main.cpp` now passes its
`PiciProcess`'s facilities through. `pi-cli auth login/status/logout`
(`cmd_auth()`) deliberately still builds its own throwaway empty catalog/store
-- forcing it through a full `PiciProcess` (which also resolves a session
directory) buys nothing for a command that never opens a session.

Phase 6 verification: implemented by a delegated subagent, then independently
rebuilt/retested/lint-diffed by the orchestrating session before commit (same
process as Phases 4/5). Full `make test` passed 45/45 (no new test binary this
phase); `make format` and `git diff --check` passed. Same targeted-`clang-tidy`-
vs-pre-phase-commit method as prior phases; every touched/renamed file matched
its pre-phase baseline exactly except one genuinely new diagnostic
(`acp/handlers.cpp`'s `apply_requested_run_model` lost its `cfg` parameter once
the parameter went unused, fixed by dropping it and updating its one call
site). `core/session/agent_session.{h,cpp}` renamed to
`core/session/session_runtime.{h,cpp}` (19 include sites updated); the file's
old top-of-file comment explaining why it was kept at its "historical" name to
avoid colliding with the CLI's `SessionRuntimeConfig`/`Bundle` was rewritten,
since Phase 5's rename of those CLI types already removed that collision.

`SessionRuntime::Config` gained an `auth_availability` field so
`set_model()` can reject a switch to a provider with missing authentication
itself, instead of three frontends (RPC, ACP, the CLI REPL) each duplicating
`Authentication::availability(...)` before ever calling in -- exactly the
"must not duplicate domain validation" problem Phase 6 names. One real,
verified deviation from the plan text: this field is a
`std::function<AuthAvailability(std::string_view)>` callback, not a
`shared_ptr<auth::Authentication>` reference. Reason: `SessionRuntime` lives
in `pi-core`, but `Authentication`'s implementation lives in `pi-http` (it
needs CURL/OpenSSL for the OpenAI Codex OAuth adapter) -- `pi-core` is linked
*by* `pi-http`, not the reverse, so calling `Authentication::availability()`
directly from `pi-core` would force every `pi-core`-only test binary to link
`pi-http` just to resolve a symbol most of them never call. This mirrors an
idiom the codebase already uses for the identical problem:
`ModelCatalog::set_request_auth_resolver` (Phase 3) is the same kind of
callback seam for the same layering reason. `ModelSwitchResult` gained an
`error` field (`Agent::set_model`/`restore_session` never populate it; only
`SessionRuntime::set_model()` does, before ever calling into `Agent`). Also
added `SessionRuntime::cancel(TurnAbortReason)` as a one-line forwarder over
`agent_.interrupt(...)`, and moved 5 direct `session.agent().interrupt(...)`
call sites (CLI REPL, RPC, the faux-control test harness) onto it; the
*task*-scoped `task_manager_->interrupt(...)` call sites were deliberately
left untouched (`AgentTaskManager`/`TaskTree` is Phase 7's concern).

Explicitly deferred, not overlooked: injecting an inference-adapter
collection into `SessionRuntime::Config` (the plan's Phase 6 text lists it
alongside catalog/authentication/session-store/sandbox, but
`LLMClientRegistry::instance()` remains the only inference-adapter mechanism
that exists today; Phase 3 already deferred building an injectable seam for
it to sometime before the singleton's eventual removal, which is Phase 10's
job, not a Phase 6 prerequisite). The `RequestPresentation`/`InputProvenance`/
`RendererRequest` split the plan also mentions was already completed by an
earlier, unrelated migration (`plans/session-runtime-migration.md` Phase 7,
commit `c041218`); confirmed zero remaining references. `task_manager()`/
`mailbox_runtime()` accessors and `TaskTree`/`Mailbox` ownership are
untouched, per the plan (Phases 7/8).

Phase 7 implemented directly by the orchestrating session rather than
delegated (the destruction-order and root-vs-child dual-code-path analysis
required reading nearly all of `agent_task.cpp` first; the risk of a subagent
getting thread/lifetime ordering subtly wrong outweighed the delegation
savings). `AgentTaskManager` is renamed to `TaskTree` (compat alias kept,
Phase-10-tagged) in `core/agent_task.{h,cpp}` -- the file itself keeps its
name, only the class moved. It now takes `Agent &owner` instead of
`SessionRuntime &root`, since only two of its `root_` uses needed anything
beyond `Agent` (`is_streaming()`, `steer_envelopes()`); child construction no
longer builds a `SessionRuntime::Config` itself -- it calls an injected
`ChildSessionFactory` (`std::function<unique_ptr<SessionRuntime>(ChildSessionSpec)>`)
that `SessionRuntime::activate()` supplies, mirroring exactly the
`agent_options`/`tools`/`session_store=null` shape `make_task()` built inline
before. `Agent` gained `task_tree()`/`set_task_tree()` (a `shared_ptr<TaskTree>`,
forward-declared in `agent.h` -- shared_ptr, not unique_ptr, both because it
tolerates an incomplete type in the header without `agent.h` including
`agent_task.h` back, avoiding a circular include, and because it preserves
`MailboxRuntime::connect()`'s existing `weak_ptr<AgentTaskManager>` safety
mechanism for delivery callbacks firing after teardown starts, unchanged);
`task_tree_` is declared last in `Agent` so it destructs first, before
`state_`/`workers_`, matching `SessionRuntime::mailbox_runtime_`'s existing
declaration-order pattern one level up.

The root pseudo-task inside `TaskTree` has no `SessionRuntime` to point
`Task::session` at any more (`Agent` doesn't hold a back-reference to its
owning `SessionRuntime`), so `task->session` is null for "root" and non-null
(`== owned_session.get()`) for every real child; a new private
`task_agent(const Task&)` helper resolves either case to the right `Agent&`
and replaced every `task->session->agent()...` callsite that is genuinely
reachable for "root" -- `snapshot()` (now a non-static member instead of
static, since it needs `owner_`), `composition_report[s]()`,
`drop_mailbox_envelopes()`, `interrupt()`, and `make_task()`/`reserve_spawn()`'s
parent-context reads. Sites confirmed reachable only for real children
(`execute_work`, `run_task`, `close_tasks`, the post-root-check branch of
`steer_envelopes`) were left calling `task->session->` directly, unchanged.
Both `composition_report()` and `composition_reports()` were silently
returning zeroed reports for the root task before this fix would have made
that regression externally visible -- worth flagging: the pre-existing
`if (task->session != nullptr)` guards in those two functions and in
`drop_mailbox_envelopes()` were dead code protecting against a condition that
could never actually happen before this phase (root's `session` was always
`&root_`), which is exactly why nothing caught it as a latent bug until this
refactor would have turned it into a real, silent one.

A found-and-fixed hazard worth flagging for future phases: `SessionRuntime`'s
old member-order-only destructor (`= default`) relied on `task_manager_`
being declared after `mailbox_runtime_` to guarantee child tasks closed
before the mailbox observer detached. Moving the task tree inside `agent_`
(declared *before* `mailbox_runtime_`) silently inverts that guarantee if
nothing else changes, since `agent_`'s entire destructor -- task tree
included -- now runs *after* `mailbox_runtime_`'s. Fixed by giving
`~SessionRuntime()` an explicit body that calls `agent_.task_tree()->shutdown()`
(idempotent, so `agent_`'s own later teardown redundantly re-shutting it down
is harmless) before any member destructor runs at all. `test-mailbox-runtime`'s
`TeardownOrder` test exercises exactly this property and was run 5x
standalone post-change with no flakes, alongside 5x shuffled runs of the full
`test-agent_tasks` suite.

14 direct `TaskTree`/`AgentTaskManager` constructor call sites across
`test_agent_tasks.cpp`, `test_mailbox_coordinator.cpp`, `test_mailbox_runtime.cpp`,
and `test_memory_stats.cpp` needed a `ChildSessionFactory` argument added;
each file got its own small `default_child_factory()` test helper that
mirrors production's factory exactly, so test behavior is unchanged.

Phase 7 verification: full `make test` 45/45 (no new test binary); 5x
shuffled `test-agent_tasks` and 5x `test-mailbox-runtime` runs, no flakes;
`make format`/`git diff --check` clean. Targeted-`clang-tidy`-vs-pre-phase-commit
(`04762ac`) diff across all 13 touched files, normalized to ignore line-number
shifts: one genuinely new diagnostic (`test_mailbox_coordinator.cpp` missing a
direct `<memory>` include for its new `default_child_factory()`'s
`std::make_unique`), fixed. Three pre-existing diagnostics disappeared as an
incidental improvement (`agent_task.h`'s and `session_runtime.h`'s
`ToolDefinition`-not-directly-included warnings, resolved by the new
`class ToolDefinition;` forward declaration `ChildSessionSpec` needed;
`test_memory_stats.cpp`'s unused-`<memory>`-include warning, resolved by the
new helper actually using it). No other diagnostic list changed in content,
only in line number.

Current compatibility seams are `ModelRegistry` -> `ModelCatalog`,
`ProviderDefinition` -> `Provider`, pointer-returning catalog search, the
`LLMClientRegistry` singleton/inference collection bridge, and
`AuthResolver` -> `Authentication` (`auth_resolver.h` now aliases
`Authentication` directly; `openai_codex_oauth` implements
`AuthenticationAdapter` and is registered under the `openai-codex-oauth`
adapter id rather than reached as a free-standing object). Each migration
alias carries its Phase 10 removal TODO; the singleton remains transitional.

Preserve these unrelated untracked files: `.claude/`, `Testing/`,
`googletest_productivity_presentation.html`, `lint-final.txt`,
`lint-output.txt`, `scripts/failed_tool_calls.py`, `scripts/tool_error_rates.py`,
and `tidy-fix-output.txt`.

To resume, run `git status`, inspect `git log`, read `AGENTS.md`,
`docs/object-taxonomy.md`, `docs/architecture-lexicon.md`, and the
authoritative plan through the next phase, then run the baseline tests before
continuing. Given how long a full `make lint` run takes on this machine
(observed >10 minutes without finishing across the whole tree), prefer
targeted `clang-tidy` invocations on just the phase's changed files, compared
against the pre-phase commit the same way, over waiting on a full-repo run.
