# Pici architecture lexicon and ownership model

> Status: **Normative target architecture**
>
> Scope: names, ownership, lifetimes, state boundaries, and subsystem interactions
>
> Last reviewed: 2026-08-29

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
SessionRuntime                         one active durable session
    +-- Agent                          one live executor activation
    |    +-- AgentState                mutable execution state
    |    +-- AgentLoop                 bounded execution state machine
    +-- SessionStore                   durable conversation journals
    +-- AgentTaskManager               owned child-task tree
    +-- MailboxCoordinator             durable cross-activation delivery
    +-- AddonRuntime                   Lua policy/customization
    +-- tools, auth, model selection, compaction policy
         |
         v
Provider client -> remote model API

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
+-- shared ModelRegistry, provider factories, AuthResolver, configuration
+-- zero or more SessionRuntimes
    +-- active Session identity and SessionStore reference
    +-- one Agent activation
    |   +-- AgentState
    |   +-- current AgentLoop worker, if running
    |   +-- steering/follow-up queues
    +-- AgentTaskManager
    |   +-- child Tasks
    |       +-- child SessionRuntime (or bounded equivalent)
    |           +-- child Agent activation
    +-- mailbox runtime/coordinator attachment
    +-- AddonRuntime/composed hooks
    +-- effective tools, sandbox, model, and compaction policy

FrontendAdapter
+-- owns protocol/UI state
+-- creates or obtains a SessionRuntime
+-- translates external input to AgentInput
+-- translates AgentEvents to UI or wire output
```

| Thing | Intended owner | Lifetime | Must not own |
|---|---|---|---|
| `PiciProcess` | executable/service host | process | one global active session |
| `SessionRuntime` | frontend/runtime host | active binding | UI or protocol responses |
| Session data | `SessionStore` | durable | threads, clients, renderers |
| Agent activation | `SessionRuntime` or child runtime | live activation | durable store |
| `AgentState` | `Agent` | activation | frontend state |
| `AgentLoop` worker | `Agent` | one agent run | durable session identity |
| `AgentTaskManager` | `SessionRuntime` | root runtime | root frontend |
| Child task | `AgentTaskManager` | spawn through close | parent frontend |
| `MailboxCoordinator` | runtime/product service | attachment | transcript |
| `MailboxStore` | mailbox service | connection | live agents |
| Add-on runtime/hooks | `SessionRuntime` | binding/reload generation | security/thread ownership |
| `Renderer` | frontend | UI/output | transcript truth or execution policy |
| `ModelRegistry` | shared process services | immutable configuration | active selection |

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
selection** is intent resolved to one model. The immutable **model registry**
owns the effective catalog; a runtime owns active selection. A model is not a
live client.

A **provider** is the service/operator namespace and auth policy. An **API
protocol** is a request/stream dialect; providers and protocols are not
one-to-one. An **LLM client** adapts a protocol between effective context/wire
requests and typed response messages/events. It does not select models,
persist, or render.

The **authentication resolver** obtains request credentials under provider
policy. Credentials never enter transcripts, mailbox payloads, or presentation
events.

### Tasks and mailbox

#### Task

A **task** is managed delegated work with task ID/path, parent, lifecycle,
result, limits, queues, and—while resident—a child runtime/activation. It is
not its child agent. The **task tree** is the ownership hierarchy rooted at one
runtime. Task IDs do not replace `agent_id` for routing or `session_id` for
durability.

#### Mailbox / presence / target

The **mailbox** is durable discovery and at-least-once delivery among sessions
and activations. It includes presence, entries, leases, acknowledgement, wait
notifications, retention, and cleanup. It is neither transcript nor in-memory
steering queue.

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
`MailboxCoordinator::claim_idle_root_turn()`/`poll_inbox()` at the exact
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
| `MailboxMessage` | `MailboxEntry` | durable coordination record |
| `MailboxMessageKind` | `MailboxEntryKind` | kind belongs to an entry |
| `MailboxBody` | `MailboxPayload` | avoids body/wire ambiguity |
| mailbox `message_id` | `entry_id` | avoids cross-domain ID collision |
| `reply_to_message_id` | `reply_to_entry_id` | explicit correlation domain |
| `SendRequest` | `EnqueueMailboxEntryRequest` | avoids request-kind collision |
| `SendReceipt` | `MailboxEnqueueReceipt` | names the commit |
| `AgentMessageEnvelope` | `AgentInput` | transient input + provenance/callback |
| `MessageAcceptanceCallback` | `InputAcceptanceCallback` | commits input acceptance |
| `AgentMessageSource` | `InputSource` | source describes input admission |
| `RequestPresentation` | `InputProvenance` + presentation | separates runtime from UI |
| `AgentSession` | grow/fold into `SessionRuntime` | owns agent and switches sessions |
| `cli::MailboxRuntime` | runtime mailbox attachment | shared product behavior |
| `acp::Message` | qualify / internal `AcpMessage` | avoids transcript collision |
| `acp::Run` | always **ACP run** | differs from agent run and turn |
| ACP `agent_name` | agent profile name | not a live activation |
| `StreamRenderer` | `Renderer` | legacy alias; broader observer |

These are incremental direction-setting renames, not a flag-day patch. Preserve
wire compatibility where required.

## Review findings against the target

The code is directionally aligned through typed messages/events, distinct
`Agent` and `AgentState`, append-only sessions, mailbox leases/acks, task
identities, provider-neutral clients, and the native/Lua mechanism-policy
split. Main deviations:

- `cmd_run()` in `src/main.cpp` still assembles model/auth, hooks, tools,
  session, mailbox, tasks, rendering, commands, and REPL lifecycle. This is the
  missing `SessionRuntime` identified by `pici-architecture.html`.
- `AgentSession` sounds like one stable session but owns an `Agent` and can
  activate different durable sessions.
- `cli::MailboxRuntime` correctly isolates mailbox wiring, but root/task
  delivery policy belongs in shared runtime code.
- ACP constructs a separate session path and globally serializes durable runs.
  Use shared construction and per-session execution ownership.
- `pi-core` is a link boundary, not a strict domain boundary. Split it only
  after ownership is explicit.
- `RequestPresentation` contains runtime provenance, not just presentation.
- `delivered_at_ms` lacks a defined write-side transition.
- README's opening architecture inventory describes the original kernel rather
  than the current product runtime.

## Migration sequence

1. Adopt this vocabulary in new plans, comments, tests, and non-wire APIs.
2. Characterize construction, switching, mailbox accept/ack timing, task
   teardown, and frontend parity.
3. Introduce a shared `SessionRuntime` and factory; move mailbox wiring there.
4. Make CLI, JSONL RPC, and ACP thin adapters over it.
5. Incrementally rename `TranscriptMessage`, `AgentInput`, and
   `MailboxEntry`, using compatibility aliases only where useful.
6. Split provenance from presentation; define or remove delivered time.
7. Replace ACP's global run mutex with per-session execution ownership after
   the runtime boundary exists.

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
