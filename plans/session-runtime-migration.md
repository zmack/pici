# Migrating to the architecture-lexicon target

## Status and intended reader

This is an implementation plan for bringing the current C++ implementation
into line with `docs/architecture-lexicon.md` (the normative target
architecture, reviewed 2026-08-29). It is written for whoever executes the
migration, one phase at a time, over several sessions/PRs. It is not a
flag-day patch — the lexicon doc says so explicitly, and the phases below are
ordered so each one lands independently, keeps the build/tests green, and
narrows the gap between the "Review findings against the target" section and
reality.

Read `docs/architecture-lexicon.md` first. This plan does not restate its
vocabulary; it sequences the work to get there and pins each phase to actual
files and line ranges as of commit `bdef9b9`.

Follow this repo's existing plan conventions while executing:
- Prefer narrow build/test targets during the edit loop; full `make test`
  before considering a phase done.
- Do not add decorative section-divider comments.
- Do not commit unless explicitly asked.
- Renames get a compatibility alias only where an external/wire boundary
  requires it (see lexicon "Usage rules" and the rename table's own note:
  "incremental direction-setting renames, not a flag-day patch").

## Why this order

The lexicon's own "Migration sequence" section (steps 1–7) is the spine of
this plan. It is expanded here because two of its steps hide most of the
risk:

- Step 3 ("introduce a shared `SessionRuntime` and factory") is the one
  structural change everything else depends on. **Correction from initial
  drafting**: the real duplication is not between `cmd_run()` and
  `src/acp/handlers.cpp:542`/`src/acp/server.cpp:56` — those two call sites
  build a 5-field `AgentSession::Config` (`agent_options`, `model_registry`,
  `tools`, `session_store`, `sandbox_policy`) with no mailbox, no Lua hooks,
  no skill catalog, no context files, no auto-compaction. The actual
  duplicated logic is model/auth/sandbox resolution, split between
  `cmd_run()` (src/main.cpp) and `src/acp/main.cpp` (its own
  `resolve_model`-equivalent, `AuthResolver` construction, `--api-key`
  handling, sandbox mode resolution — ~245 lines). See Phase 2 for what this
  means for scope.
- Step 5 (renaming `AgentSession`, `AgentMessageEnvelope`, `MailboxMessage`,
  etc.) has very different blast radii per type. Measured against the current
  tree: `AgentSession` appears in 27 files, `AgentMessageEnvelope` in 18,
  `RequestPresentation` in 14, `MailboxMessage` in 9, `MailboxBody` in 7.
  Low-blast-radius renames should land first, both to retire easy items and
  to prove the compatibility-alias pattern before the expensive ones — but
  see Phase 4: two of the "low blast radius" mailbox items are not low-risk
  despite touching few files, because they cross an on-disk/wire boundary.

So the plan below front-loads the shared-construction work (Phase 1), does
the mailbox-side renames while the mailbox code is already warm from that
work (Phase 2), then works up the blast-radius list, and finishes with the
two changes that are genuinely architectural rather than nominal: splitting
`RequestPresentation` and removing ACP's global run mutex (Phases 6–7).

## Phase 0 — Adopt the vocabulary now, at zero cost

Per lexicon "Migration sequence" step 1. No code changes; do this
immediately and keep doing it for the rest of the migration:

- Use the canonical terms (`SessionRuntime`, `AgentInput`, `MailboxEntry`,
  `ACP run`, etc.) in new comments, commit messages, plan docs, and PR
  descriptions, even before the underlying type is renamed.
- When touching a file for any other reason, prefer the canonical term in
  new/changed comments, but do not rename types as a drive-by inside an
  unrelated change — that's Phases 2–5's job, done deliberately with tests.
- Add the lexicon's "Code-review checklist" section to whatever review
  process this repo already uses (see `code-review` skill config) so new
  code is checked against it.

Definition of done: no code change; a norm, not a milestone.

## Phase 1 — Characterization tests before touching construction

Per lexicon step 2. Before extracting `SessionRuntime`, pin down current
behavior so the extraction is provably behavior-preserving. Existing
coverage is decent but split across `test_agent.cpp`, `test_agent_tasks.cpp`,
`test_mailbox_runtime.cpp`, `test_mailbox_coordinator.cpp`,
`test_mailbox_bindings.cpp`, `test_mailbox_store.cpp`, `test_rpc_mode.cpp`,
`test_faux_control_mode.cpp`, `test_compaction.cpp`, `test_config.cpp`,
`test_sandbox.cpp`, and `test_acp.cpp` (which already runs a real ACP server
in-process via `pi::acp::run_server(port, config)` on a random port and
drives it over HTTP — the right fixture to extend, not replace, for
anything ACP-related below) — none of it currently asserts that **`cmd_run()`
and `acp/main.cpp` resolve equivalent models/auth/sandbox from equivalent
inputs**, which is the actual duplication this migration closes (see "Why
this order" for the correction on where that duplication really lives).

Add (or extend) tests covering, per the lexicon's "Required flows":
1. **Model/auth/sandbox resolution parity**: given the same
   model/provider/api-key/sandbox-mode inputs, `cmd_run()`'s resolution logic
   (src/main.cpp) and `src/acp/main.cpp`'s resolution logic produce
   equivalent `Model`, `AuthResolver` configuration, and `SandboxPolicy`.
   This is the actual duplicated logic (see "Why this order" correction
   above) — do not write this test against `AgentSession::Config`
   construction in `handlers.cpp`/`server.cpp`, which is already small and
   already not duplicated with `cmd_run()`'s full stack. This test needs a
   seam — a pure function both `cmd_run()` and `acp/main.cpp` can call —
   which is the actual extraction target for Phase 2.
2. **Session switch / model switch / compaction** idle-only transitions
   (invariant 10): assert they're rejected or queue correctly while a run is
   active, on both frontends.
3. **Mailbox accept/ack timing**: claim → route → accept → acknowledge
   ordering from `test_mailbox_coordinator.cpp` / `test_mailbox_runtime.cpp`,
   asserted against the state diagram in the lexicon's "Claim, delivery,
   acknowledgement" section, so a later rename of `MailboxMessage` →
   `MailboxEntry` can't silently change delivery semantics.
4. **Task teardown order**: child tasks close before mailbox observer
   disconnects before coordinator dies (per "Destruction follows inverse
   dependency order"), using `test_agent_tasks.cpp` as the base.
5. **Frontend parity** for at least one full turn: CLI, JSONL RPC, and ACP
   each driving one prompt → tool call → response through the same
   underlying config and asserting equivalent transcript output. `ACP`
   already has an in-process fixture for this: `test_acp.cpp` calls
   `pi::acp::run_server(port, config)` on a random port and drives real HTTP
   requests against it — reuse that fixture rather than building a new one.
6. **Credentials never enter transcripts/mailbox payloads/presentation**
   (invariant 13): add a test asserting a resolved API key/bearer token never
   appears in a persisted transcript message, mailbox payload, or rendered
   event. This becomes load-bearing the moment Phase 2 gives ACP a
   shared, `AuthResolver`-backed factory it didn't have before.
7. **Delegated task results never silently splice into the parent
   transcript** (delegated-task flow step 6): extend
   `test_agent_tasks.cpp` to assert child-task results only arrive via task
   APIs or explicit mailbox entries.

Definition of done: the above run green against current code, with at least
one CI-visible failure demonstrated and reverted (to prove the test actually
catches a resolution-parity regression) before Phase 2 starts.

## Phase 2 — Extract a shared `SessionRuntime` factory

Per lexicon step 3. This is the highest-value, highest-risk phase — it's the
"missing `SessionRuntime`" the review findings call out directly.

Important framing correction: **this is not a pure dedup refactor for ACP.**
ACP today deliberately has no mailbox, no Lua hooks, no skill catalog, no
context files, no auto-compaction (its `AgentSession::Config` is 5 fields).
If the shared factory unconditionally wires all of those in, ACP gains
mailbox delivery, arbitrary Lua hook execution, and skill loading over HTTP
that it does not currently have — a security-relevant scope expansion, not
a behavior-preserving extraction. Two ways to resolve this, pick one before
writing code and record the decision in this plan:
(a) the factory takes an explicit capability-selection config (e.g.
`enable_mailbox`, `enable_hooks`, `enable_skills` flags) and ACP keeps
calling it with those off, so construction logic is shared but ACP's
current feature scope is preserved; or
(b) ACP's feature scope is deliberately widened as part of this migration,
in which case that's a separate, explicitly-reviewed change, not something
that falls out of "extract a factory."
Default to (a) unless someone actively decides otherwise — do not let the
scope expansion happen as a side effect.

Scope:
- New file(s), e.g. `src/core/session/session_runtime.h/.cpp` (naming can
  follow whatever this repo's convention turns out to be for factory-style
  code — check `src/cli/mailbox_runtime.h`'s comment "Declare it after
  AgentSession and before AgentTaskManager" for the intended construction
  order it already documents; see also the destruction-order note below).
- Pull out of `cmd_run()` (src/main.cpp, `int cmd_run(...)`, starting at line
  1095) everything that is **not** CLI-specific. Do not use a line-range cut
  point — the construction logic is interleaved with CLI-only code
  throughout the function body, including *after* the `AgentSession
  runtime(...)` call at line ~1441 (tool registration, Lua tool loading,
  hook-tool composition, system-prompt assembly, the `--list-tools`/
  `--list-addons` early returns, `AgentTaskManager` construction around line
  1534, and `mailbox_runtime.connect()` around line 1545 are all
  construction logic that comes after that line). Identify each piece by
  what it does, not by where it sits in the function:
  - model/auth resolution (matching what Phase 1's parity test now covers:
    the logic currently duplicated between `cmd_run()` and `src/acp/main.cpp`)
  - session store construction and session load/resume/continue resolution
  - sandbox mode resolution and `SandboxPolicy` construction
  - `AgentTaskManager::ChildWriteTools` resolution and `AgentTaskManager`
    construction itself
  - context file loading, skill catalog discovery (behind the capability
    flag from the framing correction above)
  - Lua hook composition (`load_hooks`, `load_presentation_hooks`,
    `HookRuntime`, and the `opts.before_tool_call`/`after_tool_call`/
    `should_stop_after_turn`/`on_event`/`prepare_context` closures that
    forward through it) (behind the capability flag)
  - mailbox launch (`cli::resolve_mailbox_launch_options` /
    `cli::start_mailbox`) and `MailboxRuntime` construction/connection
    (behind the capability flag)
  - the resulting `AgentSession::Config` construction
- What stays CLI-specific: argument parsing (`cli::Args`), renderer
  selection/wiring, REPL loop, terminal I/O, command dispatch — i.e.
  everything the lexicon's "Frontend / adapter" section says a frontend
  should own.
- Point `src/acp/main.cpp` at the shared model/auth/sandbox resolution
  (its actual duplication with `cmd_run()`), and point
  `src/acp/handlers.cpp:542` / `src/acp/server.cpp:56` at the same
  `AgentSession::Config`-building factory, called with the capability flags
  ACP currently implies (mailbox/hooks/skills off). This is where Phase 1's
  resolution-parity test earns its keep.
- `src/cli/rpc_mode.cpp` already takes a constructed `AgentSession &` — no
  change needed there beyond whatever `cmd_run()` needs to pass through
  after extraction.
- Preserve the existing construction/destruction order constraint that
  `src/cli/mailbox_runtime.h` already documents ("Declare it after
  AgentSession and before AgentTaskManager so teardown closes child tasks
  while the mailbox observer is still attached"). When this becomes a
  factory-local set of locals (and, in Phase 6, class members), the same
  ordering constraint applies — get it wrong and teardown order breaks
  silently, since C++ member destruction order is easy to get backwards
  without a compiler error.

Do not rename `AgentSession` itself in this phase. The factory can return
today's `AgentSession` + `AgentTaskManager` + `MailboxRuntime` bundle; Phase
6 folds/renames the type. Keeping extraction and renaming separate makes
each change reviewable on its own and matches "extract first, rename
second."

Definition of done: `cmd_run()` shrinks to arg-parsing + factory call +
REPL/renderer wiring; `acp/main.cpp` and ACP's two construction sites call
the same factory with ACP's current capability scope preserved (per the
framing correction above, decision recorded); Phase 1 tests stay green; no
behavior change in manual CLI/ACP smoke checks.

## Phase 3 — Relocate mailbox runtime wiring out of `cli::`

Per lexicon step 3's "move mailbox wiring there" and the review finding:
"`cli::MailboxRuntime` correctly isolates mailbox wiring, but root/task
delivery policy belongs in shared runtime code."

- Move `pi::cli::MailboxRuntime` (src/cli/mailbox_runtime.h/.cpp) into the
  shared runtime layer introduced in Phase 2 (e.g. `pi::core::session` or
  wherever the factory landed), since its job — connecting a coordinator to
  a root session, task manager, and wake callback — is product-runtime
  behavior per the lexicon's ownership table (`MailboxCoordinator` is a
  `SessionRuntime` member, not a frontend concern).
  Leave a thin `cli::` wrapper only if the REPL's wakeup mechanism
  (`wake_root` callback, terminal-specific) genuinely needs a CLI-side hook;
  otherwise fold it in outright.
- Update ACP to attach through the same relocated type instead of any
  ACP-specific mailbox wiring it currently has (check `src/acp/handlers.cpp`
  and `src/acp/task_events.cpp` for mailbox-adjacent code first).

Definition of done: `cli::MailboxRuntime` either no longer exists or is a
thin pass-through; CLI and ACP both attach mailbox delivery through the
shared type; `test_mailbox_runtime.cpp` moves or gets mirrored accordingly.

## Phase 4 — Mailbox renames, split by whether they cross a wire boundary

Per lexicon step 5 and the rename table. **Correction from initial
drafting**: these items do not have uniformly low risk just because they
touch few files. `mailbox_store.cpp` declares `message_id TEXT PRIMARY KEY`
and `reply_to_message_id` as real SQLite columns (with `PRAGMA user_version`
schema versioning that currently only *rejects* newer schemas, no
downgrade path), and `mailbox_bindings.cpp` emits/requires the literal JSON
keys `"message_id"` / `"reply_to"` as a public Lua add-on API. The mailbox
database is explicitly shared across concurrently-running pici processes
(that's its whole purpose), so a column/key rename is a cross-version
on-disk and cross-addon compatibility break, not a mechanical C++ rename.
Split accordingly:

### Phase 4a — C++-internal only, no wire/schema/Lua-key exposure

Verify each item against `mailbox_bindings.cpp` and `mailbox_store.cpp`
before starting — confirm it's a C++ type/identifier name only, not also a
literal string crossing the SQLite or Lua boundary — then rename freely:

1. `MailboxMessage` → `MailboxEntry`
2. `MailboxMessageKind` → `MailboxEntryKind`
3. `MailboxBody` → `MailboxPayload`
4. `SendRequest` → `EnqueueMailboxEntryRequest`
5. `SendReceipt` → `MailboxEnqueueReceipt`

Do each as its own mechanical commit (rename + call sites + tests), not one
combined patch — these are independent and independently revertable.

### Phase 4b — Crosses SQLite schema and the Lua JSON wire API

1. mailbox `message_id` column → `entry_id`
2. `reply_to_message_id` column → `reply_to_entry_id`

This needs a real migration design, not a rename commit — options to choose
between (record the decision, don't default silently):
- Bump `mailbox_store.cpp`'s `PRAGMA user_version` and add a real forward
  migration (`ALTER TABLE ... RENAME COLUMN`), accepting that mixed-version
  pici processes sharing one mailbox DB during a rollout window will see a
  schema mismatch — decide what old binaries do against a migrated DB
  (currently `migrate()` only rejects *newer* schemas; confirm that's still
  the right behavior or add a compatible-read path).
- Keep the SQLite column and Lua-facing JSON key as `message_id`/`reply_to`
  indefinitely (the lexicon's own "Usage rules" allow keeping the current
  name at a wire/protocol boundary) and only rename the in-memory C++
  field/accessor to `entry_id`/`reply_to_entry_id`, translating at the
  store/bindings layer. This avoids the migration entirely at the cost of
  the C++ name and the wire name permanently diverging.
- If the JSON key does change, dual-emit both keys for one deprecation
  window and accept either on read, since third-party Lua addons may depend
  on the current key name.

Keep `delivered_at_ms` as-is for now; it's handled explicitly in Phase 6
because the lexicon flags it as underspecified, not just misnamed ("has
schema/read representation but no defined write-side commit... do not
depend on it yet").

Definition of done (4a): no remaining references to the old names in C++;
`test_mailbox_coordinator.cpp`, `test_mailbox_store.cpp` pass, updated in
naming only.
Definition of done (4b): migration/compatibility approach chosen and
documented above the code, not just in this plan; `test_mailbox_bindings.cpp`
and `test_mailbox_store.cpp` updated to assert the *chosen* on-disk/wire
behavior (they will not simply "pass unmodified" — the whole point of this
sub-phase is that the wire behavior is changing or being deliberately
pinned).

## Phase 5 — Agent input renames

Per lexicon step 5, next blast-radius tier (18 files for the main type):

1. `AgentMessageEnvelope` → `AgentInput`
2. `MessageAcceptanceCallback` → `InputAcceptanceCallback`
3. `AgentMessageSource` → `InputSource`

These three are used together at most call sites (envelope + its acceptance
callback + its source), so do them as one coordinated commit rather than
three separate ones — splitting them would leave intermediate states where
`AgentInput` wraps a `MessageAcceptanceCallback`, which is more confusing
than the current all-old or all-new states.

Note the entanglement with Phase 7: `agent_loop.h` already declares a
`RequestSource` enum (`ordinary`/`mailbox`/`follow_up`) that is a near-exact
duplicate of `AgentMessageSource` — the same three-way split lives in
`request_presentation.h` too. Renaming `AgentMessageSource` → `InputSource`
here without also resolving `RequestSource` just gives the codebase two
canonical-looking source enums instead of one. Either fold `RequestSource`
into this rename now, or explicitly note here that Phase 7 owns collapsing
them and this phase's `InputSource` is deliberately provisional until then.
Also note `RequestPresentation::message_id` **is** the mailbox entry ID —
Phase 4b's chosen field name propagates into Phase 7's provenance split;
decide Phase 4b before designing Phase 7's `InputProvenance` fields.

Definition of done: grep clean for old names in non-wire code; mailbox
delivery's "Delivery converts a claimed actionable entry into agent input"
flow (lexicon "Claim, delivery, acknowledgement") still passes Phase 1's
mailbox-timing characterization test unmodified; the `RequestSource`/
`AgentMessageSource` duplication is either resolved or explicitly deferred
to Phase 7 in writing (not left implicit).

## Phase 6 — Fold `AgentSession` into `SessionRuntime`

Per lexicon step 5's `AgentSession` → "grow/fold into `SessionRuntime`" and
the target ownership tree. This is the largest *named* rename in the table
(27 files) — though `core::Message` → `TranscriptMessage`, parked in Phase 8,
is likely larger still by raw reference count; "largest rename" here means
largest among the items this plan treats as urgent, not largest overall. It
is also the one place where "rename" is really "restructure": the target
ownership tree has `SessionRuntime` own the active session identity, one
`Agent` activation, `AgentTaskManager`, mailbox attachment, and add-on
runtime/hooks — i.e. everything the Phase 2 factory currently *constructs
and returns as a bundle* should become members of one `SessionRuntime`
class.

- Rename `pi::core::AgentSession` to `pi::core::SessionRuntime` (or
  introduce `SessionRuntime` as the class and fold `AgentSession`'s current
  API into it — whichever keeps diffs smaller given what Phase 2 already
  built).
- Move `AgentTaskManager` and the relocated mailbox-runtime attachment
  (Phase 3) from "constructed alongside" to "owned by" `SessionRuntime`,
  matching the ownership table's `AgentTaskManager` row (`SessionRuntime` /
  root runtime) and `MailboxCoordinator` row (runtime/product service /
  attachment).
- Preserve the construction/destruction order constraint flagged in Phase 2
  as C++ member declaration order once these become members of one class —
  this is exactly the kind of thing that compiles fine in the wrong order
  and only breaks at teardown under specific timing.
- **Open decision, not yet resolved by this plan**: `server.cpp` currently
  builds **one process-wide** `task_root` `AgentSession` and one
  `AgentTaskManager` shared across all ACP runs; `handlers.cpp:542` builds a
  **new** `AgentSession` per `POST /runs`. Making `AgentTaskManager` an owned
  member of `SessionRuntime` (as this phase proposes) forces a choice for
  ACP that doesn't yet have an answer in this plan:
  (a) ACP moves to one long-lived `SessionRuntime` per durable session,
  looked up/reused across runs against that session, matching the lexicon's
  "a session can exist without an agent; successive agents can activate it"
  — this is probably the architecturally correct target, but changes
  `AgentTaskManager::Limits` scope from process-wide to per-session and is a
  real behavior change worth its own review; or
  (b) ACP keeps per-run `SessionRuntime` construction and accepts a
  per-run (not process-wide) task manager, changing today's task-tree scope
  the other direction.
  Resolve this explicitly before writing Phase 6 code — it changes what
  "root" means for ACP's task tree (invariant 15: "root/child relationships
  are task-tree-scoped, never process-global" already argues for (a), but
  say so on purpose rather than by accident of refactoring).
- Resolve `delivered_at_ms` here per lexicon guidance: either define its
  write-side commit point explicitly (name the exact transition, per
  "Persistence, presentation, and protocols" — "A commit point is the exact
  transition after which a named subsystem's promise is true") or remove
  the field. Don't leave it schema-present/semantics-absent through this
  phase.

This phase should be the first one genuinely gated on Phases 2–3 being
fully landed and stable — it's restructuring ownership, not just renaming a
token, so do it only once construction is unified and the mailbox runtime
already lives in the right place.

Definition of done: `SessionRuntime` exists and owns what the target tree
says it owns; CLI, RPC, and ACP all hold a `SessionRuntime` reference rather
than separately holding `AgentSession` + `AgentTaskManager` + mailbox
runtime; `delivered_at_ms` has a defined commit point or is gone; full
Phase 1 characterization suite green.

## Phase 7 — Split `RequestPresentation`; remove ACP's global run mutex

Two independent architectural fixes the lexicon calls out by name; do them
in either order, but both depend on Phase 6's `SessionRuntime` existing so
"provenance" has a clear runtime-owned home to move to.

- **Split `RequestPresentation`** (14 files) into `InputProvenance`
  (runtime-owned: where an input came from — mailbox, CLI, RPC, ACP — for
  routing/audit) and a frontend-owned `RendererRequest` (presentation-only:
  what a renderer needs to display it). Per lexicon: "Current
  `RequestPresentation` also carries runtime provenance, not just
  presentation." Use invariant 5 (transcript messages, mailbox entries, ACP
  messages, agent inputs, and events remain distinct types) as the test for
  whether a given `RequestPresentation` field belongs on the provenance side
  or the presentation side. Resolve the `RequestSource`/`AgentMessageSource`
  duplication flagged in Phase 5 as part of this split, if not already done.
  **Wire boundary**: `event_json.cpp` has a `request_json(const
  RequestPresentation &)` that serializes this type into the JSONL RPC event
  stream — splitting the type changes RPC wire output. Decide explicitly
  whether the RPC JSON shape stays flat (server-side merges provenance +
  presentation back together for wire output) or changes (bump/version the
  RPC event schema, update `test_rpc_mode.cpp` for the new shape) — do not
  let this fall out of the type split unnoticed.
- **Remove ACP's global run mutex**: per lexicon finding "ACP constructs a
  separate session path and globally serializes durable runs. Use shared
  construction and per-session execution ownership," and migration-sequence
  step 7 ("after the runtime boundary exists" — i.e. after Phase 6). The
  mutex is `durable_run_mutex` in `src/acp/handlers.cpp` (declared near the
  top of the file, taken as a `unique_lock` around run dispatch). Replace
  process-wide locking with per-`SessionRuntime` execution ownership, so two
  ACP sessions can run concurrently the way two CLI processes already can.
  This directly enables invariant 3 ("one activation binds to at most one
  active session at a time") without over-serializing at the process level.
  Before removing it, audit what it's currently protecting beyond run
  dispatch — in particular, the shared `SessionStore` and (pre-Phase-6's
  decision) a possibly process-wide `AgentTaskManager`/`task_root` in
  `server.cpp` — the mutex may be incidentally serializing access to state
  that isn't per-session-safe yet. `test_acp.cpp` already has an in-process
  `run_server(port, config)` fixture on a random port that can drive two
  concurrent HTTP requests against one server instance — use it directly for
  the new concurrency test rather than building a new fixture.

Definition of done: `RequestPresentation` no longer exists as a combined
type; the RPC wire-format decision above is made and reflected in
`test_rpc_mode.cpp`; ACP can run two independent sessions concurrently via
`test_acp.cpp`'s existing fixture (add a test for this — it's new coverage,
not in Phase 1's baseline since the current mutex makes it moot); the
pre-removal audit of what `durable_run_mutex` protects is written down;
Phase 1 frontend-parity test extended to assert concurrent ACP sessions
don't cross-contaminate transcripts.

## Phase 8 — Remaining nominal cleanup

Lower urgency, do opportunistically or as a final pass once Phases 1–7 are
stable:

- `core::Message` → `TranscriptMessage` (9 qualified references found, but
  check unqualified in-namespace usage inside `src/core/` before scoping —
  likely the largest single grep hit of the whole migration since it's the
  most-used type in the kernel; consider a compatibility `using Message =
  TranscriptMessage;` alias kept indefinitely per "Usage rules," since wire
  code and tests reference it pervasively).
- `acp::Message` → qualify as internal `AcpMessage` to avoid the transcript
  collision the lexicon calls out.
- `acp::Run` → enforce "ACP run" in all new code/comments/docs (already
  covered by Phase 0 going forward; this item is about auditing existing
  comments/docstrings for bare "run").
- ACP `agent_name` → document/comment as "ACP agent profile name" at
  call sites that currently read as if it were activation identity.
- `pi-core` link-target split: explicitly deferred by the lexicon itself
  ("a link boundary, not a strict domain boundary. Split it only after
  ownership is explicit") — do not attempt until Phases 2–6 are done and
  ownership really is explicit.

## README and docs

Per the review finding "README's opening architecture inventory describes
the original kernel rather than the current product runtime" (confirmed:
`README.md`'s `## Architecture` section already points to
`docs/architecture-lexicon.md` and `pici-architecture.html` but keeps the
old kernel-only diagram/table below, explicitly labeled "historical").
Update the `## Project Structure` table and the ASCII diagram once Phase 6
lands `SessionRuntime`, so the table reflects the current file layout
(`agent.h/.cpp`, `agent_state.h`, `agent_loop.h/.cpp` for the kernel;
`session_runtime.h/.cpp`, `agent_task.h/.cpp`, `mailbox/*` for the product
runtime; `cli/*`, `acp/*` for frontends) instead of the pre-mailbox,
pre-task, pre-ACP snapshot it currently shows.

## Risks and open questions to flag for review

- Phase 2's factory boundary is a judgment call about where "CLI-specific"
  ends — get a second opinion on the exact cut line before writing code,
  since getting it wrong means re-deriving the boundary in Phase 6 too.
- **Phase 2's capability-scope decision is a security question, not just an
  engineering one**: does the shared factory give ACP mailbox/hooks/skills
  it doesn't have today (widening ACP's attack surface — Lua execution and
  mailbox delivery reachable over HTTP), or does the factory stay
  capability-gated so ACP's current scope is preserved? This plan defaults
  to gating (Phase 2, option (a)) but flags it explicitly because it's easy
  to get this backwards by accident while "just" extracting a factory.
- **Phase 4b's SQLite/Lua-key migration approach is unresolved** — three
  options are laid out in Phase 4b, none chosen. Pick one before starting
  that sub-phase; it affects every pici process sharing a mailbox DB during
  rollout, not just this codebase.
- **Phase 6's ACP task-tree ownership model is unresolved** — per-durable-
  session `SessionRuntime` reuse vs. per-run construction with a per-run
  task manager. This is a real behavior change for ACP either way, not a
  rename, and needs its own decision before Phase 6 starts.
- Phase 6 is the one phase that's genuinely a redesign, not a rename; it
  deserves its own focused review pass independent of this plan's review.
- `delivered_at_ms` (Phase 6) needs a product decision, not just an
  engineering one: does anything actually need write-side delivery
  timestamps, or should the field be deleted? This plan doesn't have enough
  context to answer that and flags it rather than guessing.
- Phase 7's `durable_run_mutex` removal needs a pre-removal audit of what
  else it's incidentally protecting (shared `SessionStore`, and — depending
  on the Phase 6 decision above — a possibly-still-process-wide
  `AgentTaskManager`) before assuming per-session locking alone is
  sufficient.
- No estimate is given in weeks/PRs on purpose — phase sizes vary from
  "one grep-driven commit" (Phase 4a items) to "multi-week restructuring"
  (Phase 6), and this repo's own plan docs don't timebox either.
