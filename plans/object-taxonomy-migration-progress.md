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
- Phase 5: complete in the commit containing this progress update, planned
  subject `feat: introduce PiciProcess as the composition root`

Next milestone: Phase 6 — narrow `SessionRuntime` and establish the session
contract.

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
