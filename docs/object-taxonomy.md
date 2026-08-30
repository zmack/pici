# Pici object taxonomy

> Status: normative for object names, ownership, and dependency direction.
>
> When this document conflicts with migration-era implementation names or the
> older ownership diagrams in `docs/architecture-lexicon.md`, this document
> wins. The lexicon remains normative for semantic terms such as session,
> activation, turn, transcript, and mailbox acknowledgement.

## Archetypes

Pici uses seven object archetypes. A type should fit one before it becomes a
public architectural noun.

| Archetype | Responsibility | Examples |
|---|---|---|
| Aggregate | Owns a coherent domain boundary and its invariants | `ModelCatalog`, `Authentication`, `Mailbox`, `Agent`, `TaskTree` |
| Runtime | Owns one live activation of a durable or externally identified scope | `SessionRuntime` |
| Store | Owns durable operations and durable truth | `SessionStore`, `MailboxStore`, `CredentialStore` |
| Adapter | Translates across an external, protocol, or UI boundary | discovery, inference, OAuth, CLI, RPC, ACP, renderer |
| Operation | Performs one bounded action without becoming durable identity | `ModelPicker`, model refresh, compaction |
| Projection | Immutable read view assembled for a consumer | `ModelCatalogView`, task status view |
| Value | Typed data without identity or lifecycle | `ModelKey`, `ProviderModelReport`, `RequestAuth`, events, commands |

`Manager`, `service`, `helper`, `handler`, and unqualified `runtime` are not
default archetypes. Use them only when the type's ownership and boundary cannot
be expressed with the list above, and record the reason in the review.

## Ownership tree

```text
PiciProcess                         process composition root
+-- ModelCatalog                    process aggregate
|   +-- Provider[1..N]              provider definitions
|       +-- discovery binding       optional Adapter selection
|       +-- inference binding       required Adapter selection
|       +-- authentication binding  required Adapter selection
+-- Authentication                  process aggregate
+-- Mailbox                         process aggregate
+-- SessionStore                    durable session operations
+-- MailboxStore                    durable entry/presence operations
+-- CredentialStore                 durable credential operations
+-- adapter implementations         reusable process-owned implementations
+-- SessionRuntime[0..N]            one live durable-session activation
    +-- Agent                       one live executor
    |   +-- AgentState              mutable execution state
    |   +-- AgentRun[0..1]          active bounded operation
    |   +-- TaskTree                delegated-work ownership hierarchy
    |       +-- Task[0..N]
    |           +-- child SessionRuntime
    +-- MailboxAttachment[0..1]     address/delivery binding, not the mailbox
    +-- AddonAdapter[0..1]          Lua customization boundary
```

Ownership means construction, lifetime, and destruction. References and calls
do not imply ownership. In particular:

- A provider owns its three binding choices, not necessarily the shared
  adapter implementation selected by each binding.
- Process-owned stores remain durable authorities; aggregates use them but do
  not contain or independently create them.
- OAuth is an authentication adapter selected by a provider; it is not a
  process peer to providers.
- A task tree belongs to its agent. A task owns its child session runtime.
- The process owns the mailbox. A session owns only its attachment.
- A frontend owns its input/protocol state and renderer. It obtains session
  runtimes from the process and does not own domain policy.

## Stable contracts

Contracts are API seams, not additional object archetypes.

| Contract | Caller → callee | Commands / results |
|---|---|---|
| Session | frontend adapter → `SessionRuntime` | prompt, select model, compact, session transitions; typed events/results |
| Catalog | `SessionRuntime` or picker → `ModelCatalog` | view, search, resolve, refresh status |
| Persistence | `SessionRuntime` → `SessionStore` | append, replay, metadata commit |
| Input admission | `Mailbox` → `SessionRuntime` | route, accept, acknowledge callback |
| Inference | `Agent` → provider inference binding | effective context, auth, cancellation; typed events/result |
| Delegation | `Agent` → owned `TaskTree` | spawn, message, interrupt, wait, close, result |

The native model picker and a later Lua model picker must consume the same
`ModelCatalogView` and return the same `ModelKey`. Terminal drawing and Lua
tables are adapter details outside the catalog and session contracts.

## State authority

| Fact | Live authority | Durable authority |
|---|---|---|
| selected model | `AgentState` | `SessionStore` metadata |
| active transcript | `AgentState` | `SessionStore` journal |
| effective model inventory | `ModelCatalog` | configuration plus replaceable discovery cache |
| request credentials | `Authentication` | `CredentialStore` or startup environment snapshot |
| mailbox entries and leases | active `Mailbox` operation | `MailboxStore` |
| delegated-task status/result | agent-owned `TaskTree` | parent session journal only when explicitly recorded |

There must be exactly one live authority for a fact. Projections can become
stale; stores do not own the live objects they represent.

## Provider bindings

`Provider` is a value owned by `ModelCatalog`. It carries provider identity,
endpoint/configuration, and three explicit bindings:

- discovery: optional adapter that returns a `ProviderModelReport`;
- inference: required adapter/protocol used to execute a model request;
- authentication: required binding selecting none, API key, or a
  provider-specific adapter such as Codex OAuth.

Discovery never mutates the catalog directly. The catalog requests reports,
validates them, merges configured/built-in/discovered layers, and atomically
publishes a new `ModelCatalogView`. A failed refresh leaves the previous view
usable and records provider-scoped status.

## Naming and review rules

- Name public types after their archetype and responsibility, not the action
  currently being performed.
- Use `Catalog`, not `Registry`, for the effective model inventory and its
  refresh/merge policy.
- Use `Authentication`, not bare `Auth`, for the aggregate. `RequestAuth`
  remains the request-scoped value.
- Use `TaskTree`, not `AgentTaskManager`, for agent-owned delegation state.
- Use `Mailbox` for the process aggregate and `MailboxAttachment` for the
  per-session binding. Do not call either a session mailbox.
- `Renderer` is an output adapter owned by a frontend. It is distinct from the
  frontend adapter because it cannot admit input or invoke session commands.
- Keep protocol DTOs, transcript values, events, projections, and domain
  commands as distinct types.
- Compatibility aliases are temporary migration tools. Every alias must name
  its removal phase and must not appear in new code.

Before accepting a new architectural noun, answer:

1. Which archetype is it?
2. Who owns and destroys it?
3. What state is it authoritative for?
4. Which named contract does it implement or consume?
5. Is it domain vocabulary or merely an implementation detail?

