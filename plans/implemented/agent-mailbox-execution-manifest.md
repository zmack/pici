# Agent mailbox v1 execution manifest

> Follow-up: mailbox caller identity, child tool authority, and autonomous
> idle-root turns are specified in
> `mailbox-agent-identity-and-autonomous-turns.md`. That plan intentionally
> supersedes this v1 manifest's exclusions for the next implementation phase.

This manifest turns `agent-mailbox-and-session-coordination.md` into gated
implementation work. The architecture plan remains the source for semantics;
this file freezes choices that an implementation agent must not reinvent.

## Scope gate

Implement local, same-host mailbox phases 1 through 6 only. Phase 7 A2A is a
separate project and must not add code, dependencies, schema, or abstractions to
mailbox v1. Do not build SwiftUI, ACP/RPC endpoints, remote process control, or
automatic idle-root execution.

Mailbox remains disabled by default. When enabled, it automatically loads the
bundled mailbox Lua addon. Existing `pici.agents` behavior and JSONL session
storage must remain unchanged.

## Frozen decisions

- Use system SQLite through CMake `find_package(SQLite3 REQUIRED)` when mailbox
  support is compiled. SQLite support is part of the normal build, while the
  runtime feature remains disabled by default.
- Require `agent_id` for a multiply attached durable session. A session-addressed
  send fails with `ambiguous_target` while more than one live activation exists.
- Use the already resolved context/workspace root supplied by startup code.
  Canonicalize it before coordinator construction; mailbox code never runs Git
  or another subprocess.
- Drain queued `steer` and `request` messages when an explicit run starts as
  well as while it is running. Never start an idle interactive root from the
  maintenance thread.
- Keep completed subagents resident indefinitely in v1. Only explicit close,
  ancestor close, or manager/process shutdown closes them.
- Show an unread count only through diagnostics/status in v1; do not change the
  renderer or readline status area.
- `pici.mailbox.request()` is intentionally bounded and synchronous: 30-second
  default, 60-second maximum. Timeout returns a durable pending request ID.
  Do not implement Lua coroutines or an async Lua scheduler.
- One process owns one coordinator, one store/SQLite connection, and one
  `std::jthread`. Subagents never own heartbeat threads or SQLite connections.
- Use the config-directory database default from the architecture plan. Do not
  move it to XDG state storage in v1.
- Use schema version 1 exactly as specified in the architecture plan. Add CHECK,
  NOT NULL, and foreign-key constraints where they express frozen invariants,
  but do not add A2A tables.

## Cross-cutting implementation constraints

- Namespace new core code under `pi::core` and place it in
  `src/core/mailbox/`.
- Keep `MailboxStore` synchronous and unaware of Lua, agents, renderers, and
  maintenance threads.
- Represent expected errors with `MailboxError`, carrying a stable
  `MailboxErrorCode`. Do not use JSON as the internal C++ API.
- Use strongly typed request/result structs from `mailbox_types.h`. JSON
  conversion belongs only at the Lua boundary.
- All time persisted to SQLite is Unix epoch milliseconds. Inject a clock into
  store/coordinator tests; do not sleep to test lease expiry.
- All generated IDs come from one injectable ID-generator callback. Production
  IDs must be random and collision-resistant; tests use deterministic IDs.
- Prepared statements only. A store mutex covers every SQLite operation and
  transaction. No callback may run while that mutex is held.
- The coordinator may call agent/task-manager callbacks only after releasing
  its own mutex and the store mutex.
- Never invoke Lua from the maintenance thread.
- Preserve RAII ownership and explicitly delete copy/move when ownership of a
  database, thread, or callback target would otherwise be duplicated.
- Do not log message bodies by default.

## Phase 1: configuration and persistence

Commit title: `Add durable mailbox store`

Add:

- `src/core/mailbox/mailbox_types.{h,cpp}`
- `src/core/mailbox/mailbox_store.{h,cpp}`
- `test/test_mailbox_store.cpp`
- `[mailbox]` config parsing and CLI/environment precedence
- CMake SQLite discovery, linkage, source registration, and focused test target

The public store surface must cover these intentions, although names may be
adjusted only for existing repository style:

```cpp
class MailboxStore {
public:
  explicit MailboxStore(MailboxStoreOptions options);

  void migrate();
  void register_process(const ProcessRecord &process);
  void heartbeat_process(std::string_view process_id, TimestampMs now,
                         TimestampMs lease_expires_at);
  void close_process(std::string_view process_id, TimestampMs now);

  void register_agent(const AgentRecord &agent);
  void update_agent(const AgentUpdate &update);
  void close_agent(std::string_view agent_id, TimestampMs now);
  std::vector<AgentRecord> list_agents(const AgentQuery &query);

  SendReceipt send(const SendRequest &request);
  std::vector<MailboxMessage> inspect(const InboxQuery &query);
  ClaimResult claim(const ClaimRequest &request);
  void acknowledge(const AcknowledgeRequest &request);
  WaitResult wait_for_change(const WaitRequest &request,
                             std::stop_token stop_token = {});
  MailboxStatus status(const StatusRequest &request);
  CleanupResult cleanup(const CleanupRequest &request);
};
```

The exact records must preserve every schema field from the architecture plan.
Store methods enforce workspace isolation rather than relying on Lua callers.

Phase gate:

- Fresh migration, reopen, newer-version rejection, workspace filtering,
  addressing, claims, ack, recovery, ordering, concurrency, and retention tests
  pass.
- Config tests cover TOML, environment, CLI precedence, disabled behavior, and
  default path relative to the effective config path.
- Run focused formatting and lint inspection for touched files before commit.

## Phase 2: process and endpoint presence

Commit title: `Coordinate mailbox presence`

Add `MailboxCoordinator` under `src/core/mailbox/`. Construct it after startup
has resolved config, workspace, and durable session identity. Destroy it before
the hosted `AgentSession`, `AgentTaskManager`, or store callbacks can disappear.

The current task manager accepts one constructor `EventCallback`; it does not
offer general subscription. Compose mailbox observation with any existing ACP
or UI observer at the construction site through a small fan-out callback. Do
not add independent mailbox polling to `AgentTaskManager`.

The coordinator API must include explicit root activation/deactivation, run
state changes, model changes, task-event observation, and status inspection.
The maintenance loop owns heartbeat, claim expiry/cleanup cadence, and inbound
poll scheduling. Tests use short injected intervals or a controllable clock;
production defaults come from config.

Phase gate:

- Presence, heartbeat, stale lease, session switch, task lifecycle mirroring,
  one-worker ownership, and destruction-order tests pass.
- Ordinary CLI startup remains unchanged when mailbox is disabled.
- Run focused formatting and lint inspection for touched files before commit.

## Phase 3: native Lua primitives

Commit title: `Expose mailbox Lua primitives`

Add a `MailboxBindings` callback group beside `AgentBindings` in
`LuaHooks::AgentInfo`. Bind `pici.mailbox.self`, `list`, `send`, `request`,
`reply`, `inbox`, `ack`, `wait`, and `status` in `LuaHooksImpl` using the
existing Lua/JSON conversion helpers and `nil, error` convention.

Callbacks capture shared native lifetime, never a raw coordinator pointer.
Blocking request/wait operations honor `stop_token`, cap waits at 60 seconds,
and are documented as serializing that addon's Lua state during the call.

Phase gate:

- Lua result shapes, validation, disabled behavior, error conversion,
  correlation, timeout, and cancellation tests pass.
- Existing Lua addon tests remain green.
- Run focused formatting and lint inspection for touched files before commit.

## Phase 4: bundled model tools

Commit title: `Add mailbox coordination tools`

Add `addons/mailbox.lua` with exactly these model-visible tools:

- `agents_list`
- `agents_send`
- `agents_request`
- `agents_reply`
- `agents_inbox`
- `agents_close`

The first five compose native mailbox primitives. `agents_close` delegates only
to local `pici.agents.close` and rejects roots and remote endpoints. Do not
expose claim tokens, acknowledgements, generations, raw waits, leases, database
health, or SQL terminology to the model.

Mailbox enablement loads this addon exactly once without requiring an `[addons]`
entry. Child-agent inheritance follows existing tool policy; do not add a
mailbox-specific bypass.

Phase gate:

- Tool schema, target validation, tool visibility, auto-load, local-close
  authorization, pending request, late reply, and duplicate-load tests pass.
- Run focused formatting and lint inspection for touched files before commit.

## Phase 5: between-turn delivery

Commit title: `Deliver mailbox steering between turns`

Introduce an envelope-aware steering queue entry containing the user `Message`,
mailbox message ID, and an acceptance callback or equivalent delivery token.
Plain existing steering remains supported without a token.

Acceptance occurs only after the agent loop appends/publishes the steering input
at the existing safe boundary. The callback acknowledges outside agent and
store locks. If acceptance or acknowledgement fails, the lease expires and the
stable message ID permits redelivery.

Add a coordinator route for roots and an `AgentTaskManager` route for children:
running children steer at the safe boundary; completed, errored, or interrupted
children reactivate through follow-up; closing or unknown children reject.

Before coding this phase, inspect and record the proposed queue type and lock
order in the commit message or a short plan update. This phase must not cancel
HTTP requests, interrupt tool processes, add a pre-tool safe point, or auto-run
an idle interactive root.

Phase gate:

- Blocked fake-client integration tests prove safe-boundary injection and ack.
- Crash-after-claim/redelivery, duplicate IDs, idle root, note exclusion,
  completed-child reactivation, and shutdown races are covered.
- Run focused formatting and lint inspection for touched files before commit.

## Phase 6: hardening and documentation

Commit title: `Harden mailbox operations`

Add retention scheduling, integrity/newer-schema diagnostics, busy/corruption
errors, permission validation, verbose-safe observability, and user-facing
configuration/API documentation. Manually exercise two independent CLI
processes against a temporary mailbox database when practical.

Phase gate:

- `make format`
- `make lint`, with every new warning in touched code fixed
- `make test`
- `git status --short` contains only intentional files

Do not make mailbox enabled by default and do not begin A2A work.

Phase 6 implementation status: complete in the `Harden mailbox operations`
commit. The store validates identities and envelopes, rejects insecure or
corrupt databases, reports stable busy/schema/corruption/permission errors,
and uses persistent cleanup scheduling. Startup diagnostics, selected-config
path handling, mailbox documentation, and POSIX security/reopen fixtures are
included. Packaging the source-tree bundled addon remains a follow-up; A2A is
still out of scope.

## Commit and review protocol

Each phase is one reviewable commit after its phase gate passes. A phase may be
split into an additional preparatory commit only when it independently builds,
tests, and leaves behavior coherent. Never combine unrelated cleanup.

Before every commit:

```sh
make format
make lint
make test
```

During the inner loop, use the narrowest available target. Fix all new warnings
in touched code even though the repository's tidy target is advisory. Record
pre-existing failures or warnings separately; do not suppress or mechanically
rewrite unrelated code.

After each commit, report the commit hash, tests run, and any deviation from
this manifest before starting the next phase.
