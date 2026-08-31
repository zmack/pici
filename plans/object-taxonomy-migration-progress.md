# Object taxonomy migration progress

This checkpoint is updated in every milestone commit. The authoritative plan
is [`plans/object-taxonomy-migration.md`](object-taxonomy-migration.md).

Branch: `trunk`

Completed milestones:

- Phase 0: `29c618b` — `docs: establish object taxonomy guidance`
- Phase 1: `383b5a2` — `test: characterize model catalog boundaries`
- Phase 2: `777ea9e` — `refactor: establish model catalog taxonomy`
- Phase 3: complete in the commit containing this progress update, planned
  subject `feat: add provider model discovery bindings`

Next milestone: Phase 4 — make `Authentication` the process aggregate.

Phase 3 verification: full `make test` passed 43/43 tests; `make format` and
`git diff --check` passed; full `make lint` passed with the known 198 actionable
diagnostic baseline. The policy is that lint must pass with no increase above
198. Focused lint of the changed catalog after final corrections reports only
the pre-existing `resolve` and `find_model` complexity diagnostics.

Current compatibility seams are `ModelRegistry` -> `ModelCatalog`,
`ProviderDefinition` -> `Provider`, pointer-returning catalog search, and the
`LLMClientRegistry` singleton/inference collection bridge. Each migration
alias carries its Phase 10 removal TODO; the singleton remains transitional.

Preserve these unrelated untracked files: `.claude/`, `Testing/`,
`googletest_productivity_presentation.html`, `lint-final.txt`,
`lint-output.txt`, `scripts/failed_tool_calls.py`, `scripts/tool_error_rates.py`,
and `tidy-fix-output.txt`.

To resume, run `git status`, inspect `git log`, read `AGENTS.md`,
`docs/object-taxonomy.md`, `docs/architecture-lexicon.md`, and the
authoritative plan through the next phase, then run the baseline tests before
continuing.
