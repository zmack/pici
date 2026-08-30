# Pici repository guidance

Read `claude.md` for build, test, formatting, and C++ repository conventions.
Those rules apply to all agents, regardless of the filename an agent normally
discovers automatically.

## Architectural taxonomy

Before planning or implementing an architectural change, read:

1. `docs/object-taxonomy.md` — normative object archetypes, names, ownership,
   state authority, and contract direction;
2. `docs/architecture-lexicon.md` — normative semantic vocabulary;
3. `plans/object-taxonomy-migration.md` — ordered current-to-target migration.

`docs/object-taxonomy.md` wins if migration-era code, old plans, comments, or
older diagrams disagree about object names or ownership. The HTML architecture
document is explanatory, not the source of truth.

The intended ownership facts are:

- `PiciProcess` owns process-lifetime `ModelCatalog`, `Authentication`,
  `Mailbox`, stores, reusable adapter implementations, and open session
  runtimes.
- `SessionRuntime` owns one live `Agent`, an optional `MailboxAttachment`, and
  an optional add-on adapter for one active durable session.
- `Agent` owns `AgentState`, at most one active run, and its `TaskTree`.
- `TaskTree` owns tasks; each resident task owns its child `SessionRuntime`.
- `ModelCatalog` owns `Provider` definitions. Each provider explicitly selects
  discovery, inference, and authentication bindings. OAuth is selected through
  a provider; it is not a free-standing peer service.
- Frontend adapters admit input and invoke the session contract. Renderers are
  output adapters owned by a frontend and never drive domain state.

Use the standard archetypes: Aggregate, Runtime, Store, Adapter, Operation,
Projection, and Value. Do not introduce a public `Manager`, `Service`,
`Helper`, `Handler`, or unqualified `Runtime` merely because the existing code
has one. Before adding an architectural noun, state its archetype, owner,
lifetime, live state authority, and contract.

New code must use target vocabulary even while compatibility aliases exist:
`ModelCatalog`, `Provider`, `Authentication`, `TaskTree`, `Mailbox`,
`MailboxAttachment`, and `ModelPicker`. Compatibility names are migration-only
and must carry the removal phase from `plans/object-taxonomy-migration.md`.

## Architectural change checklist

For changes that touch these boundaries:

- identify the current and target owner before editing;
- preserve one live authority for each fact;
- keep domain values separate from transcript types, wire DTOs, Lua values,
  terminal state, and rendered output;
- preserve durable-write-before-live-install transitions;
- preserve credential isolation and at-least-once mailbox semantics;
