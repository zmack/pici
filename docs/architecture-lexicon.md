# Pici architecture lexicon and ownership model

> Status: **Normative target architecture**
>
> Scope: names, ownership, lifetimes, state boundaries, and subsystem interactions
>
> Last reviewed: 2026-08-31

> **Object-structure authority:** `docs/object-taxonomy.md` is normative for
> object archetypes, names, ownership, state authority, and dependency
> direction. This lexicon remains normative for semantic terms such as
> session, activation, turn, transcript, and acknowledgement. Where
> migration-era names below (for example `ModelRegistry`, `AuthResolver`, or
> `AgentTaskManager`) disagree with the object taxonomy, the taxonomy wins and
> the old name describes current migration debt only.

This document defines what Pici's architectural words mean and how the named
things are intended to interact. It is not a promise that every current C++
type already has the ideal name or owner. When this document and an accidental
name in the implementation disagree, use this document for new design work and
treat the implementation difference as migration debt.

`pici-architecture.html` is the descriptive architecture review. This is its
prescriptive companion: it fixes the vocabulary and describes the intended
shared runtime recommended by that review.

## Usage rules

- Use the canonical term in code, plans, APIs, and discussion.
- Qualify overloaded protocol terms such as **ACP run**.
- Name a type after what it is, not the operation that last touched it.
- “Owns” means controls lifetime and destruction. Otherwise say “references”
  or “uses.”
- Keep durable identity separate from live execution identity.
- Keep domain values separate from wire DTOs and presentation views.

## The shortest correct model

```text
Frontend (CLI, JSONL RPC, ACP)
    |
    v
PiciProcess
    +-- ModelCatalog -> Provider -> discovery/inference/auth bindings
    +-- Authentication
    +-- Mailbox
    +-- SessionStore / MailboxStore / CredentialStore
    +-- SessionRuntime[0..N]            one live durable-session activation
        +-- Agent                       one live executor activation
        |   +-- AgentState              mutable execution state
        |   +-- AgentLoop[0..1]         bounded agent-run operation
        |   +-- TaskTree
        |       +-- Task -> child SessionRuntime
        +-- MailboxAttachment[0..1]
        +-- AddonAdapter[0..1]

Typed TranscriptMessages and AgentEvents cross these boundaries.
Renderers and wire adapters observe events; they do not own agent behavior.
```

A **session** is a durable conversation branch. An **agent** is a live executor
temporarily attached to a session. A **turn** is one model round plus its tool
work. An **agent run** is one loop invocation and may contain several turns. A
**task** is managed delegated work, normally executed by a child agent in its
own session. A **mailbox entry** is a durable coordination record that may
become agent input.

## Ownership and lifetime

### Target ownership tree

```text
PiciProcess
+-- ModelCatalog
|   +-- Provider[1..N]
|       +-- discovery binding
|       +-- inference binding
|       +-- authentication binding
+-- Authentication
+-- Mailbox
+-- SessionStore, MailboxStore, CredentialStore
+-- reusable adapter implementations
+-- zero or more SessionRuntimes
    +-- active Session identity and references to process facilities
    +-- Agent
    |   +-- AgentState
    |   +-- current AgentLoop worker, if running
    |   +-- TaskTree
    |       +-- Task[0..N]
    |           +-- child SessionRuntime
    +-- MailboxAttachment[0..1]
    +-- AddonAdapter/composed hooks[0..1]

FrontendAdapter
+-- owns protocol/UI state
+-- obtains a SessionRuntime from PiciProcess
+-- translates external input to AgentInput
+-- owns renderer/serializer output adapters
```

| Thing | Intended owner | Lifetime | Must not own |
|---|---|---|---|
| `PiciProcess` | executable host | process | one global active session |
| `ModelCatalog` | `PiciProcess` | process | active session selection |
| `Authentication` | `PiciProcess` | process | transcripts or presentation |
| `Mailbox` | `PiciProcess` | process | session transcript or task tree |
| `SessionRuntime` | `PiciProcess`/runtime host | active session binding | UI or protocol responses |
| Session data | `SessionStore` | durable | threads, clients, renderers |
| `Agent` activation | `SessionRuntime` | live activation | durable store |
| `AgentState` | `Agent` | activation | frontend state |
| `AgentLoop` worker | `Agent` | one agent run | durable session identity |
| `TaskTree` | `Agent` | activation | parent frontend or mailbox |
| Child task | `TaskTree` | spawn through close | parent frontend |
| `MailboxAttachment` | `SessionRuntime` | session activation | mailbox store or process mailbox |
| `MailboxStore` | `Mailbox` | process/store connection | live agents |
| Add-on adapter/hooks | `SessionRuntime` | binding/reload generation | security/thread ownership |
| `Renderer` | frontend | UI/output | transcript truth or execution policy |
| `Provider` | `ModelCatalog` | catalog generation | shared adapter implementation |

Destruction follows inverse dependency order. Child tasks close before mailbox
observers disconnect; delivery disconnects before the coordinator dies; agent
workers stop before their state and referenced services disappear.

## Canonical lexicon

### Product boundaries

#### Pici

The whole product: kernel, runtime, frontends, providers, persistence,
coordination, extensions, and presentation. It is not a synonym for the loop
or the `pi-core` link target.

#### Kernel

The provider-neutral execution mechanism: transcript types, `AgentState`,
`Agent`, `AgentLoop`, tool contracts, typed events, and event streams. It
executes; it does not own terminal behavior, HTTP routes, mailbox policy, or
startup composition.

#### Product runtime / SessionRuntime

The reusable application layer composing the kernel with sessions, model/auth
services, tools, hooks, tasks, mailbox policy, compaction, and persistence.
`SessionRuntime` is the intended per-session boundary.

#### Frontend / adapter

An I/O boundary: interactive CLI, JSONL RPC, or ACP HTTP/SSE. It owns protocol
parsing, UI state, and formatting, but not a bespoke set of agent semantics.

#### Process / workspace

A **process** is one OS Pici instance; `process_id` names its leased mailbox
presence. A **workspace** is project/file scope; `workspace_id` is mailbox
isolation and `workspace_path` is resolution context. Neither is a session or
agent identity, and a workspace does not own its sessions.

### Conversation and identity

#### Session

A durable, replayable conversation branch identified by `session_id`. It
contains a `SessionHeader` and an append-only journal whose replay yields the
transcript: parent/fork metadata, optional name, persisted model/provider and
sandbox metadata, messages, truncations, metadata changes, and compactions.

A session does **not** contain a live thread, agent activation, UI queue,
renderer, mailbox lease, or provider connection. It can exist with no agent
and be resumed by a later process.

An **active session** is the session currently bound to a runtime.
`SessionRecord` is an in-memory replay snapshot, not the journal or runtime.
`SessionStore` owns journal operations and durable truth.

#### Agent / agent activation

An **agent** is a live executor owning `AgentState`, worker/cancellation
lifecycle, steering/follow-up queues, and loop configuration. `agent_id`
identifies one **activation** from creation/registration to close. It does not
identify a durable conversation or persona.

The live transcript in agent state is a working copy associated with the
active session; the agent does not durably own it. Restarting a session may
produce a new `agent_id` for the same `session_id`.

A **root agent** is directly owned by one top-level runtime; root is relative
to one task tree, not process-global. A **child agent** (user-facing:
**subagent**) executes a delegated task with separate mutable state and normally
its own session.

#### Agent runtime identity / agent profile

`AgentRuntimeIdentity` is routing identity for a live activation:
`agent_id`, `session_id`, kind, optional task ID/path, and owner agent ID.
It is request-local actor/address metadata, not transcript content.

An **agent profile** is a named capability advertised by a protocol. ACP
`AgentManifest` and `agent_name` mean profile, not activation. Call it an
**ACP agent profile** until the wire schema can be clearer.

### Execution

#### Agent input / request

An **agent input** is a transient transcript message plus source, provenance,
and optional acceptance callback. Intended name: `AgentInput`; current name:
`AgentMessageEnvelope`. It is not itself durable.

A **request** is external intent to do work. One request may contain several
messages and cause several turns. Qualify **provider request**, **mailbox
request**, and **HTTP/RPC request**.

#### Agent run / turn / model round

An **agent run** is one `AgentLoop` invocation from `AgentStartEvent` to
`AgentEndEvent`. It may contain multiple turns because of tools, steering,
or follow-ups. Avoid bare “run”; **ACP run** is a distinct protocol resource.

A **turn** is one context preparation, provider request/assistant response, and
the tool batch caused by that response, delimited by `TurnStartEvent` and
`TurnEndEvent`. If tool results require another provider request, that is
another turn in the same agent run. A visible exchange may span many turns.
**Model round** emphasizes the outbound provider request/stream inside a turn.

**Steering** is input injected at a safe boundary while a run is active.
**Follow-up** is input that causes work after the loop would otherwise finish.
They are timing behaviors, not durable message types.

#### Acceptance / completion / cancellation

**Acceptance** occurs after an input's message events are published and it is
appended to live context; its callback then runs. It does not mean answered,
run-complete, or journal-flushed.

Always qualify completion: turn, agent run, task, or ACP run completed.
**Cancellation** is the stop mechanism, **interruption** the semantic
operation/reason, and **aborted** the terminal event outcome. None implies
rollback.

### Transcript, context, and events

#### Transcript message / content block

A **transcript** is ordered provider-neutral conversation history. A
**transcript message** is one typed history item. Intended name:
`TranscriptMessage`; current core variant: `Message`. Variants are
`UserMessage`, `AssistantMessage`, `ToolResultMessage`, and
`ContextCompactionMessage`.

A transcript message is not a mailbox entry, ACP message, event, input
envelope, or rendered output. A **content block** is typed content inside it:
text, thinking, image, or tool call. ACP `MessagePart` is a wire value, not a
canonical content block.

#### Context / compaction

**Context** is request-ready system prompt, transcript, model, tools, and
runtime identity. **Raw context** is the canonical snapshot.
**Prepared/effective context** is a request-local derived view after add-on and
provider transforms; it does not become durable truth unless explicitly
committed.

**Compaction** is guarded replacement of a transcript with smaller replayable
history. The runtime owns its durable-write-then-install commit. It is not UI
summarization, arbitrary deletion, or forking.

#### Event / event stream

An **event** is an immutable typed fact at a lifecycle boundary; `AgentEvent`
is the canonical execution variant. It is observation, not command or
transcript content. An **event stream** carries ordered events and a final
result for one operation; it is neither journal nor necessarily network
stream. Emit facts at real commit points; never infer control truth from
formatted strings when a typed event can carry it.

### Tools and extensions

A **tool** is model-callable capability. `ToolDefinition` is its definition,
`ToolExecutionContext` is per invocation, `ToolResult` is its native
result, and `ToolResultMessage` is transcript representation. A **tool call**
is model-produced and correlated by `call_id`; parallel calls may interleave.

The **sandbox** is native process/filesystem isolation and a security boundary.
Lua may deny but never weaken it.

An **add-on** is a Lua module contributing tools, hooks, commands, policy, or
presentation. “Add-on” is canonical; use “plugin” only for an external
ecosystem using that word. A **hook** is one callback point; composed
`LuaHooks` adapts several add-ons and is not itself one add-on.

A **skill** is a discovered `SKILL.md` instruction package loaded on demand.
It guides behavior but is not native executable code, a Lua add-on, or a tool.

Native code owns sandboxing, task lifecycle, delivery, concurrency, and typed
transitions. Add-ons own optional policy/customization within those contracts.

### Models, providers, and auth

A **model** is an effective provider-neutral offering descriptor. **Model
selection** is intent resolved to one model. The process-owned **model catalog**
owns the effective inventory and publishes immutable views; an agent owns the
active selection. A model is not a live client.

A **provider** is a service/operator namespace and one catalog-owned definition
with explicit discovery, inference, and authentication bindings. An **API
protocol** is a request/stream dialect; providers and protocols are not
one-to-one. An **inference adapter** adapts a protocol between effective
context/wire requests and typed response messages/events. It does not select
models, persist, or render.

The process-owned **authentication** aggregate obtains request credentials
through the selected provider's authentication binding. OAuth is a
provider-selected adapter, not an independent peer aggregate. Credentials
never enter transcripts, mailbox payloads, catalog views, or presentation
events.

### Tasks and mailbox

#### Task

A **task** is managed delegated work with task ID/path, parent, lifecycle,
result, limits, queues, and—while resident—a child runtime/activation. It is
not its child agent. The **task tree** is owned by one agent; resident tasks own
their child session runtimes. Task IDs do not replace `agent_id` for routing
or `session_id` for durability.

#### Mailbox / presence / target

The **mailbox** is durable discovery and at-least-once delivery among sessions
and activations. It includes presence, entries, leases, acknowledgement, wait
notifications, retention, and cleanup. It is neither transcript nor in-memory
steering queue. The process owns the mailbox aggregate; a session runtime owns
only its move-only mailbox attachment.

`ProcessRecord` and `AgentRecord` are leased **presence records**, not
owners. Expiry means “not known live,” not “session deleted.” A **mailbox
target** contains session ID, optional activation ID, or both. Session targeting
survives activation changes; activation targeting pins an incarnation.

#### Mailbox entry / payload / kind

A **mailbox entry** is a durable coordination record. Intended:
`MailboxEntry`; current: `MailboxMessage`. It includes identities,
workspace, kind, payload, reply correlation, times, and delivery state.
`message_id` should become `entry_id`.

The **mailbox payload** is currently text plus metadata. Intended:
`MailboxPayload`; current: `MailboxBody`. It is not automatically a
transcript message.

| Kind | Meaning | Automatic delivery |
|---|---|---|
| `steer` | actionable input for current/next execution | yes, at safe boundary |
| `request` | actionable work expecting a reply | yes; may wake idle root |
| `note` | passive information for inspection | no |
| `reply` | correlated response to an entry | no; inbox-visible |

Kind is intent, not lifecycle state. An **inbox** is a query/view over
addressable entries, not separate storage. “Unread” means not acknowledged
under query policy, not necessarily “not displayed.”

#### Claim, delivery, acknowledgement

A **claim** grants temporary delivery rights under a **lease**; its token proves
that right. **Delivery** converts a claimed actionable entry into agent input.
Current code synthesizes a `UserMessage` inside an
`AgentMessageEnvelope` with mailbox provenance.

```text
enqueue -> available -> claim (leased) -> route -> accept -> acknowledge
                         ^                   |
                         +---- redeliver ----+  if lease/ack fails

acknowledged -> retain -> cleanup
```

**Acknowledgement** is durable and follows input acceptance. It means accepted
for processing by this activation—not answered, completed, rendered, or
journaled. Acceptance and acknowledgement are not one cross-store transaction,
so delivery is at least once; entry IDs are deduplication keys.

`delivered_at_ms`'s write-side commit point (plans/session-runtime-migration.md
Phase 6) is `MailboxStore::mark_delivered`, called from
`Mailbox::claim_idle_root_turn()`/`poll_inbox()` at the exact
"delivery" transition above (claimed entry -> agent input), not from `claim()`
itself. It is overwritten on redelivery, so it reflects the most recent
delivery attempt, not the first. `reply_to_message_id` is entry correlation,
not synchronous ownership; intended name: `reply_to_entry_id` (already the
in-memory C++ field name as of Phase 4b; only the SQLite column/Lua JSON key
keep the old name).

### Persistence, presentation, and protocols

A **store** owns durable operations, not live represented objects. A
**snapshot** is immutable point-in-time data and can become stale. A **commit
point** is the exact transition after which a named subsystem's promise is
true; always qualify it.

A **renderer** observes events and owns human-facing layout/output, not behavior
or transcript truth. Current `RequestPresentation` also carries runtime
provenance; split it into `InputProvenance` and frontend
`RendererRequest`.

The **CLI** owns terminal UI, editing, signals, wakeups, and command
presentation. **JSONL RPC** and **ACP** own their wire DTOs. All three should
call the same runtime behavior. ACP `Message`, `Run`, and `AgentManifest`
must be called **ACP message**, **ACP run**, and **ACP agent profile/manifest**.

## Required flows

### Ordinary input

1. Frontend parses external input into `AgentInput`.
2. Runtime validates active session/agent and invokes an agent run.
3. Loop accepts input, emits events, and performs turns.
4. Providers stream responses; tools run under native policy.
5. Runtime appends completed transcript messages to the journal.
6. Frontend renders or serializes observed events.

### Mailbox delivery

1. Sender enqueues an entry addressed to a session/activation.
2. Coordinator claims actionable work under a lease.
3. Runtime routes it to the matching root or child.
4. Entry becomes user transcript content plus provenance in agent input.
5. Agent accepts it; callback acknowledges the claim.
6. Processing, journal persistence, and replies are separate commits.

`note` and `reply` stay inbox-only unless explicitly promoted.

### Delegated task

1. Runtime spawns a task with explicit context, model, tool, and capability
   bounds.
2. Manager creates task and child runtime/activation.
3. Inherited context is copied, never shared mutable state.
4. Child executes and emits task-wrapped events.
5. Manager owns status/result, interruption, and close.
6. Results use task APIs or explicit mailbox entries; they never silently
   splice into the parent transcript.

### Session, model, and compaction transitions

- **Activate/fork:** load/create, require idle, install transcript/metadata,
  update presence. A fork gets a new session ID and is not a child task.
- **Model switch:** resolve in shared registry, require idle, update live
  configuration, persist metadata, update presence. Historical assistant
  messages retain their producing model.
- **Compaction:** snapshot transcript/epoch, produce/validate, recheck epoch,
  append replacement journal record, install live state, then emit completion.
  Failure leaves transcript unchanged.

## Architectural invariants

1. Session ID means durable conversation, agent ID live activation, and task ID
   delegated work. Never interchange them.
2. A session can exist without an agent; successive agents can activate it.
3. One activation binds to at most one active session at a time.
4. Frontends share runtime construction and domain behavior.
5. Transcript messages, mailbox entries, ACP messages, agent inputs, and events
   remain distinct types.
6. Mailbox acknowledgement means input acceptance, not completion.
7. Mailbox delivery is at least once until explicitly strengthened.
8. Task ownership and child-agent execution are distinct.
9. Typed events are observation truth; UI strings are not control-plane truth.
10. Session activation, model/sandbox changes, and transcript replacement are
    idle-only transitions.
11. Durable-write and in-memory-install order is explicit.
12. Native code owns security, concurrency, delivery, and lifecycle mechanisms;
    Lua selects policy within native contracts.
13. Credentials never enter transcripts, mailbox payloads, or presentation.
14. Context transforms are request-local unless explicitly committed.
15. Root/child relationships are task-tree-scoped, never process-global.

## Current names and intended names

| Current name | Intended name/concept | Reason |
|---|---|---|
| `core::Message` | `TranscriptMessage` | separates history from mailbox/ACP |
| `ModelRegistry` | `ModelCatalog` | owns inventory merge, refresh, and immutable projections |
| `ProviderDefinition` | `Provider` | catalog-owned definition with three explicit bindings |
| `AuthResolver` | `Authentication` | process aggregate, not a request helper |
| `AgentTaskManager` | `TaskTree` | agent-owned delegation hierarchy |
| `MailboxCoordinator` | `Mailbox` | process aggregate and durable routing authority |
| `MailboxRuntime` | `MailboxAttachment` | per-session binding, not the mailbox owner |
| `ModelSelector` | `ModelPicker` | bounded operation over a catalog projection |
| `RequestPresentation` | `InputProvenance` + presentation | separates runtime from UI |
| `agent_session.{h,cpp}` | `session_runtime.{h,cpp}` | file should name its declared object |
| missing composition root | `PiciProcess` | owns process aggregates and opens runtimes |
| `acp::Message` | qualify / internal `AcpMessage` | avoids transcript collision |
| `acp::Run` | always **ACP run** | differs from agent run and turn |
| ACP `agent_name` | agent profile name | not a live activation |
| `StreamRenderer` | `Renderer` | legacy alias; broader observer |

These are incremental direction-setting renames, not a flag-day patch. Preserve
wire compatibility where required.

## Review findings against the target

The code matches this document's target through plans/object-taxonomy-migration.md
Phase 10: typed messages/events, distinct `Agent` and `AgentState`, an
extracted `SessionRuntime`, append-only sessions, mailbox leases/acks, task
identities, provider-neutral clients, the native/Lua mechanism-policy split,
and every compatibility alias in the current-names table above removed.
Concretely:

- `ModelCatalog` owns provider-report refresh (`refresh()`) and publishes an
  immutable, generation-numbered projection (`ModelCatalogView`).
- `Provider` carries explicit discovery, inference, and authentication
  bindings (`DiscoveryBinding`/`InferenceBinding`/`AuthenticationBinding`).
- `Authentication` is the provider-bound process aggregate; Codex OAuth is
  registered as one of its adapters (`register_adapter("openai-codex-oauth", ...)`),
  not an independent peer.
- `PiciProcess` is the composition root shared by CLI and ACP; it owns
  `ModelCatalog`, `Authentication`, `SessionStore`, and lazily one `Mailbox`.
- `Agent` owns its `TaskTree` directly (`Agent::task_tree()`); `SessionRuntime`
  no longer holds it.
- `Mailbox` supports multiple per-session `MailboxAttachment`s
  (`attach()`/`detach()`), replacing the old coordinator's singular-root
  assumptions.
- The native `ModelPicker` exchanges `ModelCatalogView` and `ModelKey` values,
  not catalog pointers or concrete models.
- `ModelCatalog` owns its own `InferenceAdapterCollection`; ordinary
  execution paths (`main.cpp`, `src/acp/main.cpp`, threaded through
  `PiciProcess`) register the real providers into that explicit collection
  instead of reaching `LLMClientRegistry::instance()`. The singleton remains
  only as a constructor default for callers -- mostly tests -- that build an
  `Agent`/`ModelCatalog` without wiring a catalog through.

One deliberately out-of-scope deviation remains:

- `pi-core` is still a link boundary, not a strict domain boundary: it cannot
  link `pi-http`'s provider implementations (`OpenAICompatibleClient`,
  `OpenAICodexResponsesClient`, `MuseMessagesClient`), which is why the
  executable entry points build the explicit inference-adapter collection
  above rather than `PiciProcess` doing it internally. Split `pi-core` only
  after this ownership is made explicit elsewhere; taxonomy migration does
  not depend on it.

## Migration sequence

Follow `plans/object-taxonomy-migration.md`, one phase per delegated task. Its
dependency order is normative for the remaining structural migration; older
plans are historical context where they disagree.

## Code-review checklist

- Is this durable state, live state, snapshot, command, event, wire DTO, or UI?
- Which identity names it: process, workspace, session, activation, or task?
- Who owns it, who references it, and what destroys it?
- What is its exact commit point and failure behavior?
- Is it idle-only?
- Does frontend state leak into runtime behavior, or vice versa?
- Does “message,” “agent,” “session,” “run,” “request,” or “completion” need a
  qualifier?
- Is a typed event carrying truth, or is formatted output being parsed?
- Are append-only sessions and at-least-once delivery preserved?
- Is Lua selecting policy, or accidentally owning a native security/concurrency
  mechanism?
