# Object taxonomy migration progress

This checkpoint is updated in every milestone commit. The authoritative plan
is [`plans/object-taxonomy-migration.md`](object-taxonomy-migration.md).

Branch: `trunk`

Completed milestones:

- Phase 0: `29c618b` — `docs: establish object taxonomy guidance`
- Phase 1: `383b5a2` — `test: characterize model catalog boundaries`
- Phase 2: `777ea9e` — `refactor: establish model catalog taxonomy`
- Phase 3: `669df17` — `feat: add provider model discovery bindings`
- Phase 4: complete in the commit containing this progress update, planned
  subject `feat: make Authentication the process aggregate`

Next milestone: Phase 5 — introduce `PiciProcess` as the composition root.

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
