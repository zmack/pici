# Migration to the Pici object taxonomy

## Status and audience

This is the execution plan for moving the current C++ implementation to
`docs/object-taxonomy.md`. It is written for a delegated implementation model
such as Luna. Execute exactly one phase per delegated task unless the phase
explicitly says otherwise.

Baseline inspected while writing this plan: commit `68abbfb`. The repository
already contains the earlier `SessionRuntime` extraction and most of
`plans/session-runtime-migration.md`; do not reimplement that plan. This plan
supersedes its object names and ownership tree where they differ.

Read, in order, before changing code:

1. `AGENTS.md`
2. `docs/object-taxonomy.md`
3. `docs/architecture-lexicon.md`
4. this plan
5. the headers and tests named by the assigned phase

Do not commit unless explicitly asked. Preserve unrelated working-tree changes.

## Outcome

The migration is complete when the code has this ownership shape:

```text
PiciProcess
+-- ModelCatalog
|   +-- Provider[1..N]
|       +-- discovery binding
|       +-- inference binding
|       +-- authentication binding
+-- Authentication
+-- Mailbox
+-- SessionStore / MailboxStore / CredentialStore
+-- reusable adapter implementations
+-- SessionRuntime[0..N]
    +-- Agent
    |   +-- AgentState
    |   +-- active run[0..1]
    |   +-- TaskTree
    |       +-- Task[0..N] -> child SessionRuntime
    +-- MailboxAttachment[0..1]
    +-- AddonAdapter[0..1]
```

Frontends communicate through the session contract. The native V1 model
picker consumes a catalog projection and returns a `ModelKey`; a later Lua UI
can implement the same picker boundary without reaching into the catalog,
agent, or terminal renderer.

## Current-to-target map

| Current implementation | Target | Required structural change |
|---|---|---|
| `core::ModelRegistry` in `core/models.*` | `ModelCatalog` | Rename after introducing catalog projections; add refresh/generation ownership. |
| `ProviderDefinition` | `Provider` | Give it explicit discovery, inference, and authentication binding values. |
| `LLMClientRegistry::instance()` | process-owned inference adapter collection | Keep a temporary compatibility path, then inject it through `PiciProcess`. |
| `auth::AuthResolver` | `Authentication` | Become a process aggregate; resolve through the selected provider's auth binding. |
| `OpenAICodexOAuth` reached specially | provider-selected OAuth adapter | Register it under the OpenAI Codex provider binding. |
| `core::SessionRuntime` in `agent_session.*` | `SessionRuntime` | Retain behavior, narrow dependencies to process aggregates/stores, rename files. |
| `AgentTaskManager` owned by `SessionRuntime` | `TaskTree` owned by `Agent` | Break its `SessionRuntime &root_` dependency; inject child-session creation. |
| `MailboxCoordinator` plus `MailboxRuntime` | process `Mailbox` plus session `MailboxAttachment` | Support multiple attachments; remove singular-root ownership assumptions. |
| `cli::SessionRuntimeConfig/Bundle` | runtime builder or frontend composition input | Rename so it is not confused with the runtime object. |
| `run_model_selector(vector<Model *>)` | native picker adapter over `ModelCatalogView` | Return `ModelKey`, not an owned `Model`; keep terminal details in CLI. |
| CLI/RPC/ACP call concrete runtime methods ad hoc | session contract | Share command/result/event types; renderer remains output-only. |

## Dependency graph

```text
Phase 0  guidance and normative taxonomy
   |
Phase 1  characterization + boundary values
   |
Phase 2  ModelCatalog and Provider names
   |
Phase 3  provider bindings + model discovery
   |
Phase 4  Authentication aggregate
   |
Phase 5  PiciProcess composition root
   |
Phase 6  SessionRuntime contract and dependency cleanup
   +--------------------+
   |                    |
Phase 7  Agent owns     Phase 8  process Mailbox + attachments
          TaskTree       (starts after Phase 6; coordinate with Phase 7)
   +--------------------+
              |
Phase 9  native ModelPicker + frontend boundary cleanup
              |
Phase 10 compatibility removal, file moves, documentation audit
```

Phases 7 and 8 both touch task/mailbox event wiring. Do not run them in
parallel. Execute Phase 7 first unless the code has materially changed.

## Rules for every delegated phase

At the start:

1. Run `git status --short`; treat existing changes as user-owned.
2. Read the entire assigned phase and every file it names.
3. Run the phase's narrow baseline tests before editing.
4. Write down the exact old dependency being removed and the replacement seam.

During implementation:

- Preserve behavior unless the phase names a behavior change.
- Do not mix public renames, ownership moves, and new behavior in one patch
  unless the phase explicitly combines them.
- Prefer compatibility aliases at C++ boundaries over mass call-site churn,
  but mark each alias with `TODO(taxonomy-phase-N): remove`.
- Never add a second live authority for selected model, transcript, mailbox
  state, task state, or credentials.
- Do not let adapters call stores directly unless their named contract says so.
- Do not pass terminal types, Lua values, JSON DTOs, or provider wire values
  through domain contracts.
- Update tests in the same phase as the behavior or ownership change.

At handoff, report:

- files changed;
- old dependency removed and new dependency introduced;
- tests run with pass/fail counts;
- compatibility aliases or TODOs introduced;
- any deviation from this plan and why;
- the next unblocked phase.

Stop rather than guessing if the phase would change wire compatibility,
session journals, mailbox SQLite schema, credential storage, or frontend
feature scope beyond what is written here.

## Phase 0 — Persist the taxonomy and agent guidance

This phase is documentation-only and should land before structural code work.

Deliverables:

- `docs/object-taxonomy.md` is normative for object names, ownership, contracts,
  state authority, and provider bindings.
- `docs/architecture-lexicon.md` points to the taxonomy and yields to it on
  object structure while remaining normative for semantic terms.
- Root `AGENTS.md` requires architectural work to follow the taxonomy.
- `claude.md` contains the same pointer so non-Codex agents receive it.
- This plan is the implementation handoff.

Definition of done:

- A future agent can determine, without reading the HTML, that the process owns
  `ModelCatalog`, `Authentication`, and `Mailbox`; `SessionRuntime` owns an
  `Agent`; the `Agent` owns `TaskTree`; each provider selects its auth adapter.
- No guidance document calls `ModelRegistry` or session-owned
  `AgentTaskManager` the intended end state.

## Phase 1 — Characterize boundaries and introduce target values

Goal: establish tests and small value/projection types before renaming owners.
Do not change runtime ownership in this phase.

Add target values, initially alongside `core/models.h` if a new directory would
create unnecessary churn:

- `ModelKey { provider_id, model_id }`, equality/order/hash as needed;
- `ModelCatalogEntry`, an immutable effective-model projection safe for UIs;
- `ModelCatalogView { generation, entries, refresh_status }`;
- `ProviderModelReport { provider_id, models, observed_at, source_revision }`;
- `ProviderRefreshStatus` with idle/refreshing/succeeded/failed and an
  actionable provider-scoped diagnostic;
- `ModelSelectionRequest` containing a `ModelKey` plus optional overrides.

Do not expose pointers into the catalog in new APIs. A view owns or shares
immutable data for its complete lifetime.

Characterization tests:

- Extend `test/test_core.cpp` or add `test/test_model_catalog.cpp` and a CMake
  target covering current provider/config merge precedence, exact resolution,
  slash-containing model IDs, search ordering, and registered API validation.
- Extend `test/test_config.cpp` to snapshot every built-in provider's current
  `api`, endpoint, and auth policy before bindings are introduced.
- Extend `test/test_auth_resolver.cpp` for no-auth, optional key, required key,
  runtime override, environment snapshot, OAuth availability, and cancellation.
- Extend `test/test_cmd_run_repl.cpp` around `/model` so the current native
  selection/commit behavior is pinned.
- Add a compile-time or unit assertion that picker-facing values do not contain
  `Model *`, terminal types, JSON, or Lua state.

Narrow verification:

```bash
cmake --build build --target test-core test-config test-auth_resolver test-cmd_run_repl --parallel
ctest --test-dir build -R 'test-(core|config|auth_resolver|cmd_run_repl)$' --output-on-failure
```

Definition of done: target values exist and are tested; existing runtime paths
still use `ModelRegistry`; no ownership moved.

## Phase 2 — Establish `ModelCatalog` and `Provider`

Goal: make the model inventory an aggregate with stable projections while
preserving the existing built-in/configured behavior.

Implementation:

1. Rename `ModelRegistry` to `ModelCatalog` and `ProviderDefinition` to
   `Provider`. Keep temporary aliases for old call sites.
2. Move or split `src/core/models.{h,cpp}` only if doing so does not combine a
   semantic change with broad include churn. Preferred final files are
   `src/core/model/model_catalog.{h,cpp}` and `src/core/model/provider.h`.
3. Keep `ProviderConfig` and `ConfiguredModel` as configuration input values;
   they are not the effective provider/model objects.
4. Make `ModelCatalog::view()` return one immutable `ModelCatalogView` with a
   monotonically increasing generation. The initial built-in/config merge is
   generation 1.
5. Make `search` and `resolve` accept/return stable values (`ModelKey`,
   `ModelCatalogEntry`, `ModelResolution`) rather than borrowed model pointers
   in new APIs. Old pointer APIs may delegate temporarily.
6. Keep merge precedence exactly as characterized in Phase 1.
7. Update `SessionRuntime`, `Agent::Options`, CLI configuration, ACP startup,
   and tests to use the new name. Do not add discovery yet.

Invariants:

- A catalog view never changes after publication.
- A `ModelKey` is always two fields; never parse it by splitting at the first
  slash after it enters the domain.
- Active model selection remains in `AgentState`, not in `ModelCatalog`.

Verification: `test-core`, `test-config`, `test-agent`, `test-rpc_mode`,
`test-acp`, and `test-frontend-parity`.

Definition of done: new code says `ModelCatalog` and `Provider`; compatibility
aliases are the only remaining old names; generation-1 behavior matches the
old registry exactly.

## Phase 3 — Add provider bindings and discovery

Goal: each provider explicitly selects discovery, inference, and
authentication behavior; providers can report supported models.

Add value types:

```text
DiscoveryBinding       optional adapter ID + provider-specific options
InferenceBinding       required adapter/protocol ID
AuthenticationBinding  none | api-key | adapter ID
```

`Provider` owns these binding values. Shared implementations live in
process-owned adapter collections; do not put networking objects or credential
stores inside `Provider`.

Add a discovery adapter contract, for example:

```cpp
class ModelDiscoveryAdapter {
public:
  virtual ~ModelDiscoveryAdapter() = default;
  virtual ProviderModelReport discover(const ProviderDiscoveryRequest &,
                                       std::stop_token) = 0;
};
```

The request contains a provider projection, endpoint/options, and
request-scoped auth if discovery needs it. The report contains provider wire
facts converted to domain values. It cannot mutate `ModelCatalog`.

Catalog refresh behavior:

1. Snapshot providers and the current generation.
2. Mark selected providers refreshing in a new published view or separate
   refresh-status projection.
3. Call discovery adapters outside the catalog lock.
4. Validate provider IDs, duplicate keys, required fields, limits, and binding
   compatibility.
5. Merge built-in, configured, and discovered layers with documented
   precedence: explicit configuration/overrides win over discovery; discovery
   can add and refresh provider-reported models; built-ins remain fallback.
6. Atomically publish one new generation.
7. On one provider's failure, retain that provider's last usable models and
   publish its failure status. Do not erase the entire catalog.

Implement a deterministic fake discovery adapter first. Add real provider
discovery only for endpoints already supported and documented in the code; do
not invent endpoints. It is acceptable for V1 providers without discovery to
have no discovery binding and use built-in/configured models.

Inference migration:

- Treat the existing `api`/`LLMClientRegistry` selection as the initial
  `InferenceBinding` implementation.
- Introduce an injectable inference-adapter collection around the existing
  registry before removing `LLMClientRegistry::instance()`.
- Preserve provider-specific request transforms in existing clients.

Tests:

- fake provider reports zero, one, and many models;
- partial failure preserves last good generation;
- concurrent readers always see one complete generation;
- configured overrides beat discovery;
- unknown adapter IDs fail startup with provider-qualified errors;
- refresh cancellation does not publish a half-built view;
- providers with no discovery binding remain usable.

Add a dedicated `test-model-catalog` target if Phase 1 did not.

Definition of done: all effective providers have explicit inference/auth
bindings, supported providers may have discovery bindings, and the catalog can
refresh without invalidating active sessions or borrowed data.

## Phase 4 — Make `Authentication` the process aggregate

Goal: replace special-case credential resolution with provider-selected auth
adapters while preserving every security property.

Implementation:

1. Introduce `Authentication` as the public aggregate. It owns request-time
   resolution policy and references the provider catalog, credential store,
   startup environment snapshot, and registered authentication adapters.
2. Keep `AuthResolver` as a temporary alias/facade; new code must use
   `Authentication`.
3. Add an `AuthenticationAdapter` contract returning `RequestAuth` and
   availability. Register `OpenAICodexOAuth` as the adapter selected by the
   OpenAI Codex provider's authentication binding.
4. API-key and no-auth policies may be built-in binding variants rather than
   heap-polymorphic adapters, but they still resolve through the provider
   binding rather than provider-name `if` statements.
5. Keep `CredentialStore` process-owned alongside the other durable stores.
   `Authentication` receives a reference, and OAuth adapters receive a
   reference through it; neither the aggregate nor adapters independently
   create stores.
6. Resolve credentials for both discovery and inference using the same
   provider binding and request-scoped value.
7. Remove provider-name branching from callers. Any unavoidable provider
   specificity belongs inside the registered adapter.

Security invariants:

- Credentials never enter catalog views, model reports, transcripts, mailbox
  values, typed presentation events, or logs.
- Runtime key overrides remain provider-scoped.
- Environment variables are snapshotted before worker threads.
- OAuth refresh remains cancellation-aware and provider-isolated.
- OpenAI Codex remains OAuth-only unless an explicit product decision changes
  it outside this migration.

Verification: `test-auth_resolver`, `test-credential_store`,
`test-openai_codex_oauth`, `test-openai_codex_responses`, `test-config`, and a
new test proving discovery auth and inference auth select the same binding.

Definition of done: OAuth is visibly attached through `Provider` →
authentication binding → adapter; no composition root constructs a standalone
Codex OAuth object for callers to reach directly.

## Phase 5 — Introduce `PiciProcess`

Goal: establish one composition root for process-lifetime aggregates, stores,
adapter implementations, and session creation.

Preferred files: `src/core/process/pici_process.{h,cpp}`. Add them to `pi-core`.

`PiciProcess::Config` should contain resolved product configuration and
capability choices, not CLI `Args`. `PiciProcess` owns or holds the sole
process-lifetime instances of:

- `ModelCatalog`;
- `Authentication`;
- `Mailbox` when enabled;
- session/mailbox/credential stores or their factories;
- discovery, inference, auth, and add-on adapter implementations;
- immutable product configuration required to open a session.

Expose narrow accessors and a session factory such as
`open_session(SessionOpenRequest)`. Do not expose mutable implementation
registries to frontends.

Migration steps:

1. Wrap existing startup construction without changing CLI or ACP capabilities.
2. Build `PiciProcess` in `src/main.cpp` and `src/acp/main.cpp`.
3. Rename `cli::SessionRuntimeConfig`, `SessionRuntimeBundle`, and
   `open_session_runtime` to `RuntimeBuildConfig`, `RuntimeBundle`, and/or a
   builder name that cannot be confused with the runtime object. Eventually
   fold the shared parts into `PiciProcess::open_session`.
4. Preserve ACP's current capability exclusions. Do not enable mailbox, Lua
   hooks, skills, or interactive UI merely because the process can own them.
5. Preserve explicit shutdown: stop accepting frontend work, close session
   runtimes, detach mailbox delivery, stop mailbox maintenance, then destroy
   stores/adapters.

Tests:

- construction with mailbox/hooks disabled and enabled;
- two simultaneous session runtimes share catalog/auth/store facilities but
  not live Agent state;
- process shutdown closes sessions before shared facilities;
- CLI and ACP equivalent config resolves equivalent catalog/auth/sandbox;
- failed process construction leaves no maintenance thread running.

Definition of done: CLI and ACP construct a `PiciProcess`; process facilities
are not separately assembled in each frontend; feature scope is unchanged.

## Phase 6 — Narrow `SessionRuntime` and establish the session contract

Goal: make the runtime one live durable-session activation and give every
frontend the same domain API.

Implementation:

1. Rename `src/core/session/agent_session.{h,cpp}` to
   `src/core/session/session_runtime.{h,cpp}` after the CLI builder collision
   was removed in Phase 5.
2. Replace owned/shared process facilities in `SessionRuntime::Config` with
   explicit references/handles supplied by `PiciProcess`: catalog,
   authentication, session store, inference adapters, sandbox/tool policy.
3. Keep the live authority boundaries unchanged: AgentState owns selected model
   and active transcript; SessionStore owns journal truth.
4. Define session command/result/event values for prompt, model selection,
   compaction, activate/fork, and cancellation. The concrete class may
   implement the contract directly; do not add a facade object unless a real
   lifetime or substitution need appears.
5. Make CLI, JSONL RPC, and ACP call this contract. They may translate their
   DTOs into commands and events but must not duplicate domain validation.
6. Split input provenance from renderer requests if any combined
   `RequestPresentation` residue remains.
7. Keep all session/model/compaction transitions idle-only and preserve
   durable-write-before-live-install commit order.

Do not move `TaskTree` or mailbox ownership in this phase; those are Phases 7
and 8. The contract must avoid exposing `task_manager()` or
`mailbox_runtime()` so those later moves do not affect frontends.

Verification: `test-agent`, `test-compaction`, `test-rpc_mode`, `test-acp`,
`test-frontend-parity`, `test-cmd_run`, and `test-cmd_run_repl`.

Definition of done: all frontends share the same session commands/results and
typed events; no frontend reaches session internals for core behavior.

## Phase 7 — Move `TaskTree` under `Agent`

Goal: make delegated work part of the live executor that owns it.

This is an ownership change, not merely a rename. Current
`AgentTaskManager` has only a few direct dependencies on `SessionRuntime`:
root status, steering root input, and construction of child runtimes. Replace
those dependencies deliberately.

Implementation sequence:

1. Introduce `TaskTree` as the target name with a temporary
   `AgentTaskManager` alias.
2. Replace `SessionRuntime &root_` with an owner endpoint containing only what
   the tree needs: an `Agent &` (or narrow callbacks) for root status/steering,
   plus a `ChildSessionFactory` callback supplied from process/session
   composition.
3. Define `ChildSessionSpec` explicitly: inherited context, `ModelKey`, tools,
   sandbox/capability limits, runtime identity, session durability policy, and
   event sink. The task tree must not assemble a partial
   `SessionRuntime::Config` itself.
4. Make the child factory create the child `SessionRuntime`; each `Task` owns
   the returned child runtime. Preserve the current arena bind-before-create
   and release-after-join ordering in `agent_task.cpp`.
5. Add `std::unique_ptr<TaskTree>` (or value if construction allows) to
   `Agent`. Construct it from explicit delegation services/options. Declare it
   so it is destroyed before AgentState and worker infrastructure it observes.
6. Move `activate()` task-tree construction out of `SessionRuntime`. Remove
   `SessionRuntime::task_manager()` after callers migrate to
   `session.agent().task_tree()` or session-level delegation commands.
7. Keep root and child task IDs tree-scoped; a child agent owns its own nested
   task tree, bounded by inherited depth limits.
8. Update mailbox event observation to subscribe to TaskTree events without
   making the tree own the mailbox.

Tests to extend in `test/test_agent_tasks.cpp`:

- destroying Agent closes every task and joins children;
- destroying SessionRuntime therefore destroys Agent then its task tree;
- root steering reaches the owning Agent without a SessionRuntime backpointer;
- child creation uses the injected factory exactly once;
- nested child gets a distinct SessionRuntime and Agent-owned TaskTree;
- task result never splices into the parent transcript;
- arena lifetime and endpoint registration order remain unchanged;
- limits and cancellation behavior remain identical.

Also run `test-mailbox-runtime`, `test-mailbox-coordinator`,
`test-memory-stats`, and `test-frontend-parity`.

Definition of done: `Agent` owns `TaskTree`; `SessionRuntime` has no task-tree
member; `TaskTree` has no `SessionRuntime &root_`; tasks own child runtimes.

## Phase 8 — Make `Mailbox` process-owned and sessions attach to it

Goal: separate the durable/process coordination aggregate from per-session
delivery bindings.

Current `MailboxCoordinator` owns `MailboxStore` but also assumes one root via
`root_agent_id`, `initial_session_id`, one delivery target, and singular root
status. Do not simply rename it. First make it multi-attachment capable.

Target API shape:

- `Mailbox` owns store, presence/lease maintenance, entry operations, routing,
  and an attachment registry.
- `Mailbox::attach(MailboxAttachmentSpec)` returns a move-only
  `MailboxAttachment` handle.
- The attachment owns session/activation address, delivery callbacks, wake
  callback, and task-event subscription. Destroying it unregisters only that
  session/activation.
- `SessionRuntime` owns its optional attachment; it references, never owns,
  `Mailbox`.

Implementation sequence:

1. Characterize existing single-root behavior in `test-mailbox-coordinator`
   and `test-mailbox-runtime` before changing it.
2. Replace singular root fields and `delivery_targets_` with a mutex-protected
   attachment map keyed by activation ID, with session-ID lookup for durable
   routing. Reject ambiguous live activation routing explicitly.
3. Move `activate_root`, `deactivate_root`, model/name status, wake, queued-drop,
   and root-turn state behind the attachment handle.
4. Preserve claim → route → accept → acknowledge timing and at-least-once
   semantics. Attachment destruction must not acknowledge unaccepted work.
5. Rename `MailboxCoordinator` to `Mailbox` only after the multi-attachment
   behavior is green.
6. Rename `MailboxRuntime` to `MailboxAttachment`; remove construction helpers
   that imply session ownership of the mailbox.
7. Have `PiciProcess` create the mailbox once and pass attachment capability to
   sessions. ACP remains unattached unless explicitly enabled.
8. Subscribe each attachment to its Agent-owned TaskTree for child activation
   routing; the mailbox never owns the tree or child agents.

Tests:

- two sessions attach to one mailbox and receive only addressed work;
- session-targeted work survives activation replacement;
- activation-targeted work does not drift to a replacement activation;
- detaching one session leaves other sessions and maintenance alive;
- lease expiry/redelivery/ack timing remains at least once;
- task routing follows the owning session's TaskTree;
- process shutdown detaches all sessions before stopping the mailbox;
- SQLite schema and Lua mailbox keys remain unchanged in this phase.

Definition of done: `PiciProcess` owns one mailbox aggregate; each runtime owns
at most one attachment; no mailbox type claims ownership by a session.

## Phase 9 — Stabilize `ModelPicker` and frontend adapters

Goal: ship the native V1 picker on a UI-neutral contract and leave a clean Lua
implementation seam.

Separate the operation from presentation:

```text
ModelPickerInput
  catalog_view: ModelCatalogView
  current: ModelKey
  availability: provider/model availability projection

ModelPickerResult
  cancelled: bool
  selected: optional<ModelKey>
```

Implementation:

1. Rename `model_selector` to the `ModelPicker` vocabulary. Keep the native
   terminal implementation in `src/cli`; raw mode and alternate-screen RAII
   remain frontend concerns.
2. Replace `vector<const Model *>` and returned `optional<Model>` with the
   immutable picker input and `ModelKey` result.
3. Resolve and commit the selected key through the session contract. The picker
   cannot call `AgentState`, `SessionStore`, or `Authentication` directly.
4. Keep renderer and frontend adapter separate: the frontend admits commands
   and owns UI flow; renderer/serializer only presents typed events/results.
5. Add a testable picker navigation reducer if key handling is currently tied
   too tightly to file descriptors. Keep actual terminal I/O in the native
   adapter.
6. Define, but do not yet implement, the Lua picker adapter boundary as a
   conversion between `ModelPickerInput/Result` and Lua values. Lua must receive
   a projection, not a catalog pointer, and return a key, not mutate runtime
   state.
7. Ensure CLI, RPC, and ACP model-list/select surfaces derive from the same
   catalog view and session command even when their UI/wire representation
   differs.

Tests:

- navigation/filter/cancel/current-selection behavior without a TTY;
- native terminal RAII on normal return, cancellation, and exception;
- stale generation selection is re-resolved against the live catalog and
  either accepted by key or rejected clearly;
- selecting during a run is rejected by the session contract;
- picker cannot leak request credentials into its availability projection;
- CLI/RPC/ACP expose equivalent model keys and selection outcomes.

Definition of done: native V1 uses the shared picker values; no terminal or Lua
type crosses into `ModelCatalog` or `SessionRuntime`; a future Lua adapter can
be added without changing either domain API.

## Phase 10 — Remove migration scaffolding and audit the taxonomy

Do this only after Phases 1–9 are green.

Remove:

- `ModelRegistry`, `ProviderDefinition`, `AuthResolver`, `AgentTaskManager`,
  `MailboxCoordinator`, and `MailboxRuntime` compatibility aliases;
- singleton inference lookup from ordinary execution paths;
- obsolete `agent_session.*` and conflicting CLI `session_runtime.*` names;
- two-phase construction that exists only for the old ownership graph;
- comments pointing to completed migration phases as present architecture.

Update:

- `docs/architecture-lexicon.md` so its shortest model, ownership table,
  current-name table, and review findings describe the final code directly;
- README architecture inventory;
- CMake source lists and test names;
- `docs/object-taxonomy.md` only if implementation review found a deliberate
  design correction. Do not silently edit the taxonomy to excuse accidental
  implementation drift.

Required grep audit:

```bash
rg -n 'ModelRegistry|ProviderDefinition|AuthResolver|AgentTaskManager|MailboxCoordinator|MailboxRuntime|AgentSession' src test docs README.md
rg -n '\b(manager|service|helper|handler)\b' src/core
```

Every remaining match must be a qualified protocol/framework term, historical
migration document, or explained implementation detail—not a public domain
object.

Final verification:

```bash
make format
cmake --build build --parallel
ctest --test-dir build --output-on-failure
make lint
```

Perform manual smoke checks for interactive CLI prompt/model selection,
JSONL RPC model list/select/run, ACP session/run streaming, mailbox delivery,
OAuth login/use, and child-task spawn/wait/close.

Definition of done: implementation, tests, agent guidance, lexicon, and README
all express the same object names, ownership tree, state authorities, and
contract directions, with no compatibility alias left.

## Suggested delegation prompt

Use this template for each Luna task:

```text
Implement only Phase N of plans/object-taxonomy-migration.md.

Before editing, read AGENTS.md, docs/object-taxonomy.md, the complete Phase N,
and every source/test file named there. Inspect git status and preserve all
unrelated changes. Run the phase's baseline tests first.

Do not begin Phase N+1, do not make unlisted wire/schema/security changes, and
do not commit. Keep compatibility shims explicitly marked for their removal
phase. At completion, run the phase's verification and report files changed,
the dependency removed/replaced, tests and counts, remaining shims/TODOs, and
whether the next phase is unblocked.
```

