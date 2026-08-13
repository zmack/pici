# Agent mailbox and cross-session coordination

## Status and intended reader

This is an implementation plan for adding a durable, Lua-first mailbox that
lets independent pici sessions discover one another and exchange messages.
It is intended to be execution-ready: it defines terminology, persistence,
session lifecycle, delivery semantics, Lua primitives, model-visible tools,
threading, configuration, tests, rollout order, and completion criteria.

The first consumer is the agent itself. A running agent should be able to list
other live pici sessions in the same workspace, inspect enough metadata to
choose a recipient, and send that session a message. Messages arriving while a
session is running should become steering input at a safe turn boundary. The
same substrate can support a future native UI or ACP adapter, but those are not
prerequisites.

Follow `AGENTS.md` during implementation:

- Prefer narrow build and test targets during the edit loop.
- Run `make format` and inspect `make lint` before final verification.
- Before committing, run `make test`, or the equivalent full build and CTest
  commands.
- Do not commit unless explicitly requested.
- Do not add decorative section-divider comments.

## Executive design decisions

1. Use one SQLite database shared by pici processes for session presence and
   mailbox messages.
2. Store it beside the effective pici config by default:
   `$XDG_CONFIG_HOME/pici/mailbox.sqlite3`, falling back to
   `~/.config/pici/mailbox.sqlite3`. Allow an explicit path override.
3. Treat an **agent instance** and a durable **pi session** as different
   identities. Each process activation gets a random `agent_id`; it also
   advertises the current durable `session_id`.
4. Keep the existing `AgentTaskManager` and `pici.agents` semantics for child
   tasks inside one process. Do not silently change `pici.agents.list()` from a
   local task-tree query into a global session query.
5. Add a small native `pici.mailbox` Lua API for cross-process primitives.
   Build the model-visible tools in a shipped Lua addon using `pici.add_tool`.
   The high-level coordination policy therefore remains Lua-first.
6. Make workspace isolation the default. Sessions share a physical database,
   but listing and sending are limited to the sender's canonical workspace
   unless configuration explicitly enables a broader scope.
7. Use heartbeat leases instead of assuming clean process shutdown. A process
   that crashes becomes stale automatically.
   Run one RAII-owned mailbox maintenance thread per pici process, regardless
   of how many root sessions or subagents that process currently hosts.
8. Use leased, at-least-once message delivery. Duplicate delivery is possible
   after a crash and must be visible through a stable message ID.
9. Deliver ordinary coordination messages cooperatively between model/tool
   turns through `Agent::steer(...)`. Do not cancel an HTTP request or tool
   process merely because a message arrived.
10. Do not make SQLite the transcript store. Existing JSONL session journals
    remain authoritative for conversation history.
11. Register local `AgentTaskManager` children as addressable subagent
    endpoints. Their liveness derives from the owning process lease; they do
    not receive independent heartbeat threads or SQLite connections.
12. Preserve the existing subagent lifecycle: `completed`, `errored`, and
    `interrupted` remain resident and addressable. Only explicit close, ancestor
    close, or process/task-manager shutdown removes them.
13. Expose request/reply correlation and a bounded send-and-wait operation.
    Timeouts return a durable request ID; late replies remain in the inbox.
14. Keep the model-facing surface curated. The bundled Lua addon exposes five
    mailbox intentions plus one local lifecycle tool, while claim tokens,
    acknowledgements, leases, generations, raw waits, and database health stay
    below the model boundary.
15. Treat A2A as an edge transport, not as the mailbox schema. Mailbox remains
    pici's local coordination kernel; an A2A adapter translates external Agent
    Cards, Messages, Tasks, Parts, Artifacts, streams, and cancellation into
    explicitly exported local capabilities.

## Terminology

- **Pi session**: the durable session represented by `SessionStore` and its
  JSONL journal. It has a `session_id` and can outlive a process.
- **Agent instance**: one live activation of a pi session in a pici process. It
  has a random `agent_id` and inherits liveness from its owning process lease.
- **Child task**: an agent owned by the current process's `AgentTaskManager`,
  addressed today by IDs and paths such as `/root/research`. In the mailbox it
  is advertised as a `subagent` endpoint owned by the process's root agent.
- **Mailbox message**: a durable envelope sent between pi sessions, root agent
  instances, or subagent endpoints.
- **Steering delivery**: injection of a mailbox message into a running agent at
  the next safe boundary in `run_agent_loop_worker_impl`.

The distinction matters because the current Lua API already exposes local
child-task operations:

```lua
pici.agents.spawn(...)
pici.agents.list(...)
pici.agents.send(...)
pici.agents.follow_up(...)
pici.agents.interrupt(...)
pici.agents.close(...)
```

Those bindings close over one in-memory `AgentTaskManager` in `src/main.cpp`.
They cannot see a second `pi-cli` process and should continue to mean local
task-tree coordination.

## Goals

- Register each active top-level pi session and its resident subagents in a
  shared directory.
- List live root agents and subagents with stable, useful, non-secret metadata.
- Send a message to a session, live root instance, or subagent endpoint.
- Send a correlated request, wait briefly for its reply, and recover late
  replies through the inbox.
- Read, wait for, claim, acknowledge, and inspect mailbox messages.
- Make these operations available first as native Lua primitives.
- Ship model-visible Lua tools with clear schemas and capability descriptions.
- Inject inbound steering messages between turns while an agent is active.
- Recover safely from process crashes, stale presence rows, and abandoned
  message claims.
- Support concurrent readers and writers from multiple pici processes.

## Non-goals for the first release

- Replacing session JSONL files with SQLite.
- Sharing in-memory `AgentTaskManager` child objects between processes.
- Remotely spawning or killing arbitrary OS processes.
- Remotely closing a root session or a subagent owned by another process.
- Interrupting a model stream or tool call for a normal message.
- Automatically starting an idle interactive CLI turn when readline owns the
  foreground input row. Idle sessions retain messages for explicit inbox reads
  or their next run.
- Network transport, multi-host coordination, or a server daemon.
- A2A support in the initial mailbox release. The plan defines the adapter
  boundary and follow-on phase so the mailbox does not accidentally preclude
  interoperability.
- Exactly-once delivery across a crash boundary.
- A SwiftUI, web, ACP, or RPC interface in the initial implementation.
- Storing API keys, prompts, transcripts, tool results, or auth headers in the
  presence table.
- Automatically closing a subagent as soon as it completes one turn. Completed
  subagents intentionally remain available for follow-ups and replies.

## Current implementation and reusable seams

The current tree already contains most of the in-process coordination pieces:

- `core::AgentTaskManager` owns a local root plus child tasks and implements
  spawn, get, list, send, follow-up, interrupt, wait, and close.
- Each local task already has an in-memory mailbox and condition variable.
- `src/main.cpp` binds the manager into `LuaHooks::AgentInfo::agents`.
- `src/core/lua_tool.cpp` exposes the bindings as `pici.agents.*` and converts
  JSON values to and from Lua.
- `pici.add_tool(...)` lets Lua register model-visible tool definitions.
- `Agent::steer(...)` already queues messages that
  `run_agent_loop_worker_impl` reads after a turn boundary.
- `Agent::follow_up(...)` already queues work for after the active run.
- `AgentSession` owns the active durable session ID and persists transcript
  messages through `SessionStore`.
- `SessionStore::default_sessions_dir()` already implements the analogous XDG
  path policy for session journals.

The missing pieces are cross-process presence, durable message envelopes,
lease/claim handling, an inbound event pump, and a Lua API that exposes
cross-process endpoints without leaking raw child-task internals.

## High-level architecture

```text
shipped Lua addon
  list / send / request / reply / inbox + local close
                         |
                  pici.mailbox.*
                         |
              C++ MailboxCoordinator
            /           |             \
 SQLite MailboxStore  AgentSession  AgentTaskManager
 process/endpoints    root steer    child steer/follow-up/close
            \           |             /
              mailbox.sqlite3 (WAL)
                         |
             other pi-cli processes
```

Split the native implementation into two layers:

1. `MailboxStore` is a synchronous persistence API with no knowledge of Lua,
   renderers, or agent-loop policy. It owns schema migration and SQLite
   transactions.
2. `MailboxCoordinator` owns the local agent identity, heartbeat, inbox polling,
   delivery policy, and mapping from stored envelopes to `Message` values.

Lua bindings call the coordinator through narrow callbacks, following the same
pattern as the current `LuaHooks::AgentInfo::agents` bindings. Do not give Lua a
database handle or allow it to issue SQL.

## Database location and configuration

Add an optional config table:

```toml
[mailbox]
enabled = true
# path = "~/.config/pici/mailbox.sqlite3"
scope = "workspace"          # workspace | global
heartbeat_interval_ms = 2000
stale_after_ms = 10000
poll_interval_ms = 250
claim_lease_ms = 30000
retention_days = 30
```

Defaults:

- `enabled = false`. Setting it to `true` initializes the coordinator and
  automatically loads the shipped mailbox addon; there is no second addon flag.
- `path` is `default_config_path().parent_path() / "mailbox.sqlite3"`.
- `scope = "workspace"`.
- Heartbeat every 2 seconds, stale after 10 seconds.
- Poll active sessions every 250 milliseconds. Make this configurable so local
  systems can trade latency for wakeups.
- Retain acknowledged messages for 30 days and stale presence rows for 7 days.

Resolve and create the path during single-threaded startup. Create the parent
directory with mode `0700` when possible and the database with mode `0600`.
Refuse symlinks or insecure ownership/permissions using the same defensive
style as the credential store. The mailbox does not contain credentials, but
it can contain private coordination messages.

The user suggested storing the database beside config, so that is the initial
default. If mailbox volume later becomes significant, migration to
`$XDG_STATE_HOME/pici` can be considered separately; do not split locations in
the first implementation.

Add corresponding CLI overrides only if they are needed for testing or
operational recovery:

```text
--mailbox <path>
--no-mailbox
```

`PICI_MAILBOX` may override the path for test harnesses and isolated processes.
Precedence should be CLI, environment, TOML, default.

## Identity and workspace scope

Generate a random `process_id` for every pici process and a random `agent_id`
for each root-session activation or subagent endpoint. Do not use a PID as
identity because PIDs are reused. Advertise the PID only for diagnostics.

The process presence record contains:

- `process_id`: random identity for this pici process lifetime.
- `workspace_id` and `workspace_path`.
- `pid`, `hostname`, and process start time.
- protocol/capability metadata.
- `last_seen_at_ms` and `lease_expires_at_ms`.

An agent endpoint record contains:

- `agent_id`: random endpoint identity.
- `kind`: `root` or `subagent`.
- `process_id`: the process that can deliver to this endpoint.
- `owner_agent_id`: null for a root; the root endpoint for a subagent.
- `session_id`: durable pi session identity.
- `session_name`: optional user-facing name.
- `task_id` and `task_path`: populated only for a subagent.
- current provider/model.
- lifecycle status. Roots use `starting`, `idle`, `running`, `closing`, or
  `closed`; subagents additionally retain `completed`, `errored`, and
  `interrupted` as live, addressable states.
- `started_at_ms` and `closed_at_ms`.

Only the process row is heartbeated. Endpoint liveness is computed as “endpoint
not closed and owning process lease not expired.” This gives every subagent a
first-class mailbox identity without creating a thread, timer, or SQLite
connection per child.

Derive the workspace root from the same root used for context discovery. A
canonical Git worktree root is ideal when available; otherwise use the
canonical current working directory. Do not run arbitrary shell commands from
the mailbox layer. Pass the already-resolved workspace path into it at startup.

By default:

- `list` returns only non-stale agents in the caller's workspace.
- `send` rejects a recipient outside the workspace.
- `scope = "global"` permits visibility across workspaces in the same database,
  but must be explicit.

Multiple live processes may activate the same durable `session_id`. Do not hide
this case. Listing should return both agent instances and mark the session as
multiply attached. A session-addressed message may be claimed by only one live
instance at a time; an instance-addressed message goes to exactly the specified
activation.

## SQLite schema

Use explicit migrations and a `PRAGMA user_version`. Four tables cleanly
separate process liveness from addressable endpoints:

```sql
CREATE TABLE processes (
    process_id           TEXT PRIMARY KEY,
    workspace_id         TEXT NOT NULL,
    workspace_path       TEXT NOT NULL,
    pid                  INTEGER NOT NULL,
    hostname             TEXT NOT NULL,
    protocol_version     INTEGER NOT NULL,
    capabilities_json    TEXT NOT NULL,
    started_at_ms        INTEGER NOT NULL,
    last_seen_at_ms      INTEGER NOT NULL,
    lease_expires_at_ms  INTEGER NOT NULL,
    closed_at_ms         INTEGER
);

CREATE TABLE agents (
    agent_id             TEXT PRIMARY KEY,
    process_id           TEXT NOT NULL REFERENCES processes(process_id),
    kind                 TEXT NOT NULL,
    owner_agent_id       TEXT REFERENCES agents(agent_id),
    session_id           TEXT NOT NULL,
    session_name         TEXT,
    task_id              TEXT,
    task_path            TEXT,
    provider              TEXT NOT NULL,
    model_id              TEXT NOT NULL,
    status                TEXT NOT NULL,
    started_at_ms         INTEGER NOT NULL,
    closed_at_ms          INTEGER
);

CREATE INDEX processes_workspace_live
    ON processes(workspace_id, lease_expires_at_ms);
CREATE INDEX agents_session_live
    ON agents(session_id, status);
CREATE INDEX agents_process
    ON agents(process_id, status);

CREATE TABLE messages (
    message_id            TEXT PRIMARY KEY,
    sender_agent_id       TEXT NOT NULL,
    sender_session_id     TEXT NOT NULL,
    recipient_session_id  TEXT NOT NULL,
    recipient_agent_id    TEXT,
    workspace_id          TEXT NOT NULL,
    kind                  TEXT NOT NULL,
    body_json             TEXT NOT NULL,
    reply_to_message_id   TEXT,
    created_at_ms         INTEGER NOT NULL,
    available_at_ms       INTEGER NOT NULL,
    claim_agent_id        TEXT,
    claim_token           TEXT,
    claim_expires_at_ms   INTEGER,
    delivered_at_ms       INTEGER,
    acknowledged_at_ms    INTEGER,
    failed_at_ms          INTEGER,
    failure               TEXT
);

CREATE INDEX messages_recipient_pending
    ON messages(recipient_session_id, acknowledged_at_ms, available_at_ms);
CREATE INDEX messages_workspace_created
    ON messages(workspace_id, created_at_ms);

CREATE TABLE mailbox_events (
    generation            INTEGER PRIMARY KEY AUTOINCREMENT,
    workspace_id          TEXT NOT NULL,
    event_type            TEXT NOT NULL,
    subject_id            TEXT NOT NULL,
    created_at_ms         INTEGER NOT NULL
);
```

`body_json` is a versioned envelope, initially:

```json
{
  "version": 1,
  "text": "Please inspect the auth resolver.",
  "metadata": {
    "sender_task_path": "/root/research"
  }
}
```

Do not store arbitrary serialized internal `Message` variants initially. A
versioned text envelope is easier to validate and keeps provider-specific
content, images, signatures, and tool calls out of the coordination channel.

`mailbox_events` provides a monotonic generation for waits and change queries.
SQLite has no portable cross-process condition variable, so waiting still uses
bounded polling; generations ensure callers do not miss changes between polls.
The table can be compacted after all retained messages and presence rows that
refer to an event have expired.

## SQLite connection and concurrency policy

Add SQLite through CMake's `FindSQLite3` and link it only where the mailbox
store is built. If requiring a system SQLite installation is unacceptable for
supported platforms, vendor the official amalgamation in a separate reviewed
change rather than downloading it during every build.

On every connection:

```sql
PRAGMA foreign_keys = ON;
PRAGMA journal_mode = WAL;
PRAGMA synchronous = NORMAL;
PRAGMA busy_timeout = 5000;
```

Use prepared statements exclusively. Never interpolate session IDs, paths, or
message text into SQL. Wrap registration, claims, acknowledgements, and event
generation in transactions.

Prefer one `MailboxStore` connection per process guarded by a mutex for the
first version. Background heartbeat/poll operations and Lua calls may arrive on
different threads, so no SQLite call may escape the store lock. If profiling
shows contention, move to one connection per role later; do not begin with a
connection pool.

Convert SQLite result codes to a small typed `MailboxError` with stable codes
such as `not_found`, `permission_denied`, `busy`, `invalid_message`,
`incompatible_schema`, and `internal`. Lua should receive `nil, error` using the
existing binding convention.

## Presence lifecycle

1. Open and migrate the database during CLI startup.
2. Register one process row and start one mailbox maintenance `std::jthread`.
   The same worker performs inbox polling and renews the process lease; never
   create a heartbeat thread per root agent or subagent.
3. Once an `AgentSession` has an active durable `session_id`, register a root
   endpoint with status `starting`.
4. Transition the root to `idle` when readline is accepting input and `running`
   while an agent run is active. The maintenance thread refreshes the process
   lease and batches any dirty endpoint metadata updates.
5. Subscribe to `AgentTaskManager` lifecycle events. Register a `subagent`
   endpoint on spawn, mirror its task status, and mark it closed on
   `AgentTaskClosedEvent`.
6. When switching or resuming sessions in one process, close the old activation
   row and register a new `agent_id`. This prevents messages meant for the old
   logical activation from being silently applied to the new transcript.
7. On normal shutdown, close every endpoint owned by the process, mark the
   process closed, and expire its lease immediately.
8. On startup and periodically, treat expired process leases—and therefore all
   endpoints owned by them—as stale. Physical cleanup
   happens only after retention; no correctness path depends on deletion.

The single maintenance thread must be RAII-owned, use `std::jthread` and
`stop_token`, and never outlive the coordinator, SQLite store, any callback
target, or hosted `AgentSession`. A server process such as `pi-acp` may register
many roots and children, but still uses one worker and one store connection.
Destructor order must be explicit.

### Subagent completion and close

Do not equate completion with close. The current `AgentTaskManager` keeps a
child resident after its current work finishes so it can accept follow-ups:

```text
pending -> running -> completed / errored / interrupted
                         |              |
                         +-- follow-up -+-> running
                         +-- close --------> closing -> shutdown -> removed
```

A completed, errored, or interrupted subagent remains listed and addressable.
It closes only when:

- its owner calls `pici.agents.close({ target = ... })`;
- an ancestor is closed, which recursively closes its subtree; or
- the owning `AgentTaskManager`/process shuts down.

Closing interrupts active work, joins the runner, emits
`AgentTaskClosedEvent`, removes the task from the manager, and marks its mailbox
endpoint closed. Do not add immediate auto-close on completion. A future idle
TTL may be considered as an explicit retention policy, but it is outside v1.

## Message addressing and delivery semantics

Support two recipient forms:

```lua
-- Durable session address; survives recipient process restart.
{ session_id = "01J..." }

-- Exact live activation; useful when a session is multiply attached.
{ agent_id = "agent_..." }
```

An `agent_id` may identify a root or subagent. Listing includes `kind`,
`owner_agent_id`, `task_id`, and `task_path` so callers can distinguish them.

Resolve an `agent_id` to its session and workspace inside the send transaction.
Reject stale or closed instance recipients. For a session recipient, require
the session to be known in the same workspace, but allow it to be temporarily
offline so messages can wait for its next activation.

Initial message kinds:

- `steer`: deliver at the next safe boundary if the target is actively running;
  otherwise leave it in the inbox.
- `note`: durable inbox-only message; never inject automatically.
- `request`: a steering message that expects one correlated response.
- `reply`: a response whose `reply_to_message_id` names the request.

Do not implement a remote hard-interrupt kind in the first release. It combines
authorization, cancellation, tool-process cleanup, and transcript semantics in
one operation. Add it only after steering behavior is proven.

Claim messages transactionally:

1. Select available, unacknowledged messages addressed to the current session
   and either no specific agent or the current `agent_id`.
2. Exclude claims whose lease has not expired.
3. Set `claim_agent_id`, a random `claim_token`, and `claim_expires_at_ms` in the
   same transaction.
4. Return envelopes ordered by `(created_at_ms, message_id)`.
5. Acknowledge using both `message_id` and `claim_token`.
6. If the process dies before acknowledgement, another activation can reclaim
   the message after the claim lease expires.

This is at-least-once delivery. Include the `message_id` in every Lua result and
in automatically injected steering text so duplicates are diagnosable:

```text
[Mailbox message 01J... from session 01H... / agent agent_...]
Please inspect the auth resolver and send me the result.
```

For automatic steering, acknowledge only after the agent loop has accepted the
message into its context, not when the polling thread merely calls `claim`.
This likely requires extending the steering queue from bare `Message` values to
an envelope with an optional acceptance callback or delivery token. Invoke the
ack callback immediately after the loop publishes/appends the steering message
at the safe boundary. Callbacks must be best-effort and must not hold agent or
SQLite locks while invoking one another.

Delivery to subagents routes through the owning process:

- Running subagent: call a new envelope-aware
  `AgentTaskManager::steer(target, envelope)` and accept it at the next safe
  boundary.
- Completed/errored/interrupted subagent: queue the envelope as a follow-up,
  reserve execution, and reactivate the existing task.
- Closing/shutdown/unknown subagent: reject the message as unavailable.

Remote mailbox callers may send or request work from a subagent, but may not
close or interrupt it. Lifecycle authority remains with its local owner.

## Between-turn delivery

Add an inbox pump owned by `MailboxCoordinator`:

- It polls only while a durable session is active.
- When a root or subagent is running, it claims `steer` and `request` messages
  and calls a new envelope-aware steering method.
- `run_agent_loop_worker_impl` continues to read steering only after the
  current assistant response and tool batch have reached `TurnEndEvent`.
- When the agent is idle, the pump does not call `run_prompt` from a background
  thread. It leaves messages pending for `pici.mailbox.inbox()` or the next
  explicitly started run.
- `note` messages are never auto-claimed by the pump.
- A completed subagent can be reactivated with a follow-up; an idle interactive
  root is not auto-run because readline owns its foreground lifecycle.

The initial safe-boundary semantics are therefore:

```text
message arrives -> SQLite commit -> poller claims -> Agent::steer
  -> current response/tool batch finishes -> TurnEndEvent
  -> message appended to context -> SQLite ack -> next LLM turn
```

Document the latency as current turn duration plus at most `poll_interval_ms`.
If future requirements need delivery before a tool batch starts, add an
explicit pre-tool safe point; do not make the poller mutate live context.

## Native Lua primitives

Add `MailboxBindings` to `LuaHooks::AgentInfo` and expose it as
`pici.mailbox`. Keep arguments and results JSON-compatible so the bridge can
reuse `lua_to_json` and `json_to_lua`.

### `pici.mailbox.self()`

Returns the current identity:

```lua
{
  agent_id = "agent_...",
  session_id = "01J...",
  session_name = "auth-debugging",
  workspace_id = "...",
  status = "running",
}
```

### `pici.mailbox.list(options)`

```lua
local sessions, err = pici.mailbox.list({
  include_self = false,
  include_stale = false,
  limit = 100,
})
```

Return agent instances, not a lossy session-only projection. Include
`agent_id`, `session_id`, optional name, workspace path, status, model,
`last_seen_at_ms`, `lease_expires_at_ms`, and whether another live process has
the same session attached. Do not expose environment variables or credentials.

### `pici.mailbox.send(message)`

```lua
local receipt, err = pici.mailbox.send({
  target = { session_id = "01J..." },
  text = "Please review the mailbox schema.",
  kind = "steer", -- or note
  reply_to = nil,
})
```

Return `message_id`, resolved recipient information, `created_at_ms`, and
`state = "queued"`.

### `pici.mailbox.request(options)`

```lua
local reply, err = pici.mailbox.request({
  target = { agent_id = "agent_..." },
  text = "Inspect the auth resolver and report back.",
  timeout_ms = 30000,
})
```

Insert a `request`, then wait only for a `reply` whose
`reply_to_message_id` matches the new request ID. Default to 30 seconds and cap
the wait at 60 seconds. On timeout return
`{ state = "pending", request_id = "..." }`; do not cancel the durable
request. A late reply remains available through the inbox.

### `pici.mailbox.reply(options)`

```lua
local receipt, err = pici.mailbox.reply({
  message_id = incoming.message_id,
  text = "The legacy key wins before the provider-scoped key.",
})
```

Resolve the original sender, insert a `reply`, and set
`reply_to_message_id = incoming.message_id`. Reject replies to unknown messages
or messages outside the current workspace.

### `pici.mailbox.inbox(options)`

```lua
local messages, err = pici.mailbox.inbox({
  kinds = { "note", "steer", "request", "reply" },
  claim = true,
  limit = 50,
})
```

Return sender identity, message text, timestamps, message ID, claim token, and
delivery state. `claim = false` is an inspection operation and must not change
delivery state.

### `pici.mailbox.ack(request)`

```lua
local result, err = pici.mailbox.ack({
  message_id = "01J...",
  claim_token = "claim_...",
})
```

Make acknowledgement idempotent for the same recipient and return the final
state.

### `pici.mailbox.wait(options)`

```lua
local result, err = pici.mailbox.wait({
  after_generation = 42,
  timeout_ms = 30000,
})
```

Poll with `stop_token` support and return `timed_out`, `generation`, and a
compact indication of presence/message changes. Cap a single wait at a
documented maximum, such as 60 seconds, so Lua hooks cannot strand shutdown.

Lua functions must return `nil, error` consistently on failure. They must not
throw a Lua error for expected mailbox conditions such as an offline target,
an empty inbox, or a stale claim.

### `pici.mailbox.status()`

Return database health, schema version, local process/agent identities, last
heartbeat, and unread counts. This is a Lua/human diagnostic primitive and is
not exposed directly to the model.

## Shipped model-visible tools

Create a shipped addon, for example `addons/mailbox.lua`, that uses only the
native `pici.mailbox` primitives and registers narrowly described tools with
`pici.add_tool`:

- `agents_list`: list concurrent root and subagent endpoints in this workspace.
- `agents_send`: send a `steer` or `note` message to a selected session.
- `agents_request`: send a request and wait up to 60 seconds for its correlated
  reply, returning a durable request ID on timeout.
- `agents_reply`: answer an inbound request.
- `agents_inbox`: inspect pending notes, requests, and late replies; claim and
  acknowledge internally.
- `agents_close`: close a locally owned subagent and all of its descendants by
  delegating to the existing `pici.agents.close`. Reject roots and remote
  endpoints.

The first five are the curated mailbox tools. `agents_close` is a local
subagent-lifecycle tool and is not a remote mailbox operation. Together they
present one coherent coordination vocabulary while preserving native authority
boundaries. Avoid exposing SQL concepts such as
claims, rows, or WAL in ordinary tool descriptions. The schemas should explain
that agent IDs identify live activations and session IDs identify durable
conversations.

Example registration shape:

```lua
pici.add_tool({
  name = "agents_send",
  description = "Send a message to another active pici session in this workspace.",
  parameters = {
    type = "object",
    properties = {
      session_id = { type = "string" },
      agent_id = { type = "string" },
      message = { type = "string" },
      kind = { type = "string", enum = { "steer", "note" } },
    },
    required = { "message" },
  },
  execute = function(args)
    -- Validate exactly one target and delegate to pici.mailbox.send.
  end,
})
```

Do not hard-code coordination instructions into the global C++ system prompt.
The registered tool names, descriptions, and schemas are already exposed to the
model. If experiments show that the model fails to discover them, add a concise
Lua-provided prompt fragment through an explicit addon hook rather than making
mailbox policy inseparable from the runtime.

`[mailbox] enabled = true` should initialize the coordinator and automatically
load the bundled addon. Users should not copy a Lua file or separately list it
under `[addons]`. Loading the addon in two independent `pi-cli` processes is the
primary end-to-end demonstration.

Do not expose `mailbox_ack`, `mailbox_claim`, raw `mailbox_wait`,
`mailbox_status`, or claim/generation/lease fields as model tools. The Lua addon
composes that bookkeeping behind the intention-level tools.

## Relationship to local child agents

Keep storage and authority separate even though the bundled addon presents a
coherent tool family:

| Capability | Existing local API | New cross-session API |
|---|---|---|
| Identity | task ID/path | root or subagent agent ID plus session ID |
| Scope | one `AgentTaskManager` | all live processes in workspace |
| Storage | memory | SQLite |
| Spawn | supported | not supported |
| Send | in-memory mailbox | durable envelope |
| Follow-up | queues child work | coordinator reactivates completed children |
| Interrupt | local cooperative cancellation | not initially supported |
| Wait | condition variable | SQLite generation polling |
| Close | local and recursive | never remote |

The coordinator mirrors local task lifecycle into SQLite and routes inbound
messages back to the owning `AgentTaskManager`. It does not reconstruct or own
the child task. This avoids treating a stale database endpoint as an
interruptible in-process object.

The model-visible `agents_close` tool is authorized only when the target
endpoint belongs to the current process and is a subagent. It calls the existing
recursive `AgentTaskManager::close()` path. It cannot close a root, a separate
session, or another process's subagent.

## A2A interoperability boundary

The repository currently has no A2A implementation. Add it after the local
mailbox protocol is stable, as an adapter rather than a second interpretation
of the mailbox tables.

The current A2A 1.0 model is deliberately broader than this mailbox: an Agent
Card advertises a remote service and its skills; Messages carry typed Parts;
stateful Tasks produce status updates and Artifacts; and clients may use direct
request/response, streaming, subscription, or asynchronous push notification.
See the [official A2A specification](https://github.com/a2aproject/A2A/blob/main/docs/specification.md)
and [normative protobuf model](https://github.com/a2aproject/A2A/blob/main/specification/a2a.proto).

The architecture should be:

```text
                          CoordinationRouter
Lua/model tools ----------+-------------------------------
                          |                               |
                   local target                     A2A target
                          |                               |
                 MailboxTransport                    A2ATransport
                          |                               |
                 mailbox.sqlite3                    HTTP / SSE
                          |                               |
                 local root/subagent                 remote agent

inbound A2A server -> A2A bridge task -> selected mailbox endpoint
```

Define a transport-neutral internal interface only after the mailbox semantics
are proven:

```cpp
class CoordinationTransport {
public:
  virtual ListResult list(const ListRequest &) = 0;
  virtual SendResult send(const SendRequest &) = 0;
  virtual RequestResult request(const RequestRequest &,
                                std::stop_token) = 0;
  virtual CancelResult cancel(const CancelRequest &) = 0;
  virtual ~CoordinationTransport() = default;
};
```

`MailboxTransport` and `A2ATransport` share intention-level operations, not
storage types or lifecycle enums.

### Identity and lifecycle mapping

Do not reuse identifiers across the boundary:

| pici/mailbox concept | A2A concept | Mapping rule |
|---|---|---|
| Workspace endpoint | Agent Card | Only explicitly exported pici capabilities receive an Agent Card |
| `process_id` | None | Local implementation detail; never transmitted |
| `agent_id` | Remote agent identity | Store a bridge mapping; never present the local ID as protocol identity |
| Mailbox `message_id` | A2A `messageId` | Preserve both IDs and use the external ID for A2A idempotency |
| Mailbox request/reply | A2A Send Message | Direct A2A Message may satisfy the request; an A2A Task creates asynchronous bridge state |
| Related mailbox work | A2A `contextId` | Use a bridge conversation mapping, not the pici session ID verbatim |
| One unit of delegated work | A2A `taskId` | Create a durable bridge task record |
| Mailbox text envelope | A2A Parts | Translate text directly; files/data require explicit artifact handling |
| Agent result/tool output | A2A Artifact | Produce an Artifact rather than treating final output as an ordinary status Message |
| Local interrupt | A2A Cancel Task | Allowed only for a bridge-owned, currently cancelable task |
| Process heartbeat | None | Remote agents are not inserted into the local `processes` table |

The most important mismatch is terminal state. A completed pici subagent remains
resident and can accept a follow-up. An A2A Task in completed, failed, canceled,
or rejected state is terminal and cannot accept another message. Therefore:

- never map an A2A Task one-to-one to a subagent lifetime;
- create a new A2A `taskId` for every new external unit of work;
- use a shared `contextId` to relate successive tasks when appropriate; and
- allow one resident pici subagent to execute many A2A tasks over time.

### A2A bridge persistence

Keep A2A state out of the generic mailbox message columns. Add adapter-owned
tables through a separate migration module, either in the same SQLite file or
in a dedicated A2A database if artifact volume later justifies it:

```sql
CREATE TABLE a2a_peers (
    peer_id              TEXT PRIMARY KEY,
    agent_card_url       TEXT NOT NULL,
    agent_card_json      TEXT NOT NULL,
    etag                 TEXT,
    fetched_at_ms        INTEGER NOT NULL,
    expires_at_ms        INTEGER
);

CREATE TABLE a2a_task_links (
    bridge_id            TEXT PRIMARY KEY,
    direction            TEXT NOT NULL,
    peer_id              TEXT,
    local_agent_id       TEXT,
    local_message_id     TEXT,
    a2a_message_id       TEXT NOT NULL,
    a2a_context_id       TEXT,
    a2a_task_id          TEXT,
    state                TEXT NOT NULL,
    created_at_ms        INTEGER NOT NULL,
    updated_at_ms        INTEGER NOT NULL
);

CREATE TABLE a2a_artifacts (
    bridge_id            TEXT NOT NULL REFERENCES a2a_task_links(bridge_id),
    artifact_id          TEXT NOT NULL,
    artifact_json        TEXT NOT NULL,
    PRIMARY KEY (bridge_id, artifact_id)
);
```

Do not store bearer tokens or A2A authentication secrets in these tables.
Resolve credentials through a provider-style credential boundary.

### Inbound A2A server

Implement the server adapter in `pi-acp`, reusing its HTTP/SSE infrastructure:

1. Publish Agent Cards only for explicitly configured capabilities. Never
   publish every live session/subagent or the workspace directory.
2. Authenticate and authorize the caller before resolving a local endpoint.
3. On Send Message, validate Parts, create an `a2a_task_links` row, route the
   input to the selected root/subagent, and return a Task immediately for
   asynchronous work or a direct Message only for genuinely immediate results.
4. Translate pici lifecycle/events into A2A task status updates. Translate
   final deliverables into Artifacts.
5. Implement Get Task before streaming. Then add Send Streaming Message and
   Subscribe To Task using SSE; add push notifications only after retry,
   authentication, and SSRF policy are designed.
6. Map Cancel Task only to work created and owned by this bridge. It may
   interrupt that execution, but it must not expose generic mailbox close or
   remote subagent-tree destruction.

### Outbound A2A client

Add a separate trusted Lua programming surface:

```lua
pici.a2a.discover({ agent_card_url = "https://agent.example/.well-known/agent-card.json" })
pici.a2a.send({ peer_id = "research", message = { parts = { ... } } })
pici.a2a.get_task({ peer_id = "research", task_id = "..." })
pici.a2a.cancel({ peer_id = "research", task_id = "..." })
```

Do not expose these transport mechanics directly to the model by default.
Extend the existing high-level tools so `agents_list` returns targets with
`transport = "mailbox"` or `transport = "a2a"`; the bundled Lua routing layer
then dispatches `agents_send`/`agents_request` to the appropriate native API.

For an outbound A2A request:

- if Send Message returns a direct Message, return it immediately;
- if it returns a Task, wait or subscribe for at most the normal 60-second tool
  budget;
- on timeout, return `{ state = "pending", transport = "a2a", task_id,
  context_id }` without canceling the remote task; and
- continue tracking the task so a later terminal update/artifact appears in the
  local inbox.

Agent Card discovery is explicit/configured or user-approved. There is no
ambient global discovery. Cache cards using their HTTP caching metadata and
keep peer availability separate from the local heartbeat directory.

### A2A security boundary

- An Agent Card describes capabilities; it is not proof of trust.
- Require an export allowlist mapping public skills to selected local routing
  policies. Do not expose session names, task paths, workspace paths, or the
  local directory by default.
- Treat all inbound Parts and metadata as untrusted user input.
- Validate MIME types, byte sizes, file URLs, extension URIs, and structured
  data before creating mailbox work.
- Never fetch arbitrary file URLs from the agent execution context without an
  explicit SSRF-safe fetch policy.
- Keep A2A authentication, tenant identity, and authorization decisions outside
  mailbox envelopes.
- Namespace external idempotency keys by peer/tenant so two callers cannot
  collide on `messageId`.
- Do not treat A2A Cancel Task as permission to call `agents_close`.

### A2A rollout phase and tests

Add A2A only after mailbox phases 1–6 pass. The follow-on sequence is:

1. Implement Agent Card parsing/cache and outbound Send Message/Get Task.
2. Add durable task/context/message correlation and late-result inbox delivery.
3. Add inbound Agent Card plus Send Message/Get Task to `pi-acp`.
4. Add SSE streaming/subscription and artifact chunk handling.
5. Add cancel semantics, then consider authenticated push notifications.

Test direct Message responses, asynchronous Tasks, terminal-state enforcement,
context reuse with new task IDs, idempotent message IDs, streaming reconnects,
artifact append/final-chunk behavior, cancellation ownership, malformed Parts,
authentication failures, export allowlists, and late completion after a local
tool timeout.

## Security and privacy

- Scope all normal operations to a workspace ID.
- Validate all string sizes before starting a transaction. Suggested defaults:
  64 KiB maximum message text, 256-byte IDs/names, and 4 KiB metadata JSON.
- Reject control characters in IDs and validate generated identifiers.
- Never persist API keys, auth headers, environment variables, full prompts,
  transcripts, or tool outputs in presence metadata.
- Do not allow Lua to select an arbitrary database path per call.
- Do not expose PID-based signaling through the mailbox API.
- Treat received text as untrusted user input. Inject it as a user message with
  explicit sender framing, never concatenate it into the system prompt.
- Ensure logs and errors do not include full message bodies by default.
- Use bound parameters and transactions for every database mutation.
- Detect a database schema newer than the binary understands and fail closed
  with a clear diagnostic.

Same-user access to the config directory is the initial trust boundary. If pici
later coordinates across machines or users, authentication and authorization
must be designed before reusing this protocol over a network.

## Failure behavior

- Database unavailable at startup: report a clear diagnostic. If the mailbox
  addon is optional, disable only mailbox features; do not break ordinary chat.
- Busy database: retry within `busy_timeout`, then return `busy` without losing
  the caller's message text.
- Corrupt database: do not silently recreate or truncate it. Report the path and
  recovery guidance.
- Newer schema: open read-only only if inspection is safe; otherwise refuse
  mailbox operations.
- Recipient expires after listing but before send: session-addressed sends may
  queue; instance-addressed sends fail as stale.
- Sender crashes after send commit: the message remains valid.
- Receiver crashes after claim: the claim lease expires and permits redelivery.
- Receiver crashes after context injection but before acknowledgement: the
  message may be delivered twice. Preserve its message ID.
- Heartbeat fails transiently: retain the local session but surface mailbox
  health through Lua and verbose diagnostics.

## Implementation phases

### Phase 1: Configuration and SQLite store

Add:

- `src/core/mailbox/mailbox_types.{h,cpp}`
- `src/core/mailbox/mailbox_store.{h,cpp}`
- configuration parsing for `[mailbox]`
- CMake SQLite discovery/linkage
- schema migration and path/permission handling

Implement store-level registration, heartbeat, close, list, send, inspect,
claim, acknowledge, wait-generation, and retention cleanup. Test this layer
without constructing an `Agent` or Lua state.

### Phase 2: Runtime presence coordinator

Add `MailboxCoordinator` with one RAII-owned maintenance thread per process.
Wire it to top-level `AgentSession` activation, model changes, run start/end,
session resume/switch, and shutdown in `src/main.cpp`. Subscribe it to
`AgentTaskManager` spawn/status/close events so it mirrors subagent endpoints
without giving them independent heartbeat workers.

### Phase 3: Native Lua primitives

Extend `LuaHooks::AgentInfo` and `LuaHooksImpl` with `pici.mailbox.*`. Reuse the
existing JSON conversion helpers and `nil, error` convention. Capture a shared
coordinator in callbacks so reloading addons cannot outlive native state.

Add Lua bridge tests for argument conversion, result shapes, unavailable
mailbox behavior, validation errors, and stop-aware waits.

### Phase 4: Shipped Lua coordination tools

Create `addons/mailbox.lua` and tests using the Lua test harness. Register
`agents_list`, `agents_send`, `agents_request`, `agents_reply`,
`agents_inbox`, and the local-only `agents_close`. Ensure the generated tool
schemas are visible in `AgentState::tools()` and inherited by child agents only
when the existing tool policy allows them. Enabling `[mailbox]` automatically
loads this bundled addon.

### Phase 5: Between-turn steering

Make steering queue entries capable of carrying mailbox acknowledgement state.
Have the coordinator claim inbound `steer`/`request` envelopes for an active
root or subagent and queue them through `Agent::steer` or
`AgentTaskManager::steer`. Reactivate completed subagents through the follow-up
path. Acknowledge after the loop appends/publishes the input at a safe boundary.

Test with a deliberately blocked/fake LLM client:

1. Start session A and enter a multi-turn tool loop.
2. Start session B against the same temporary database.
3. B lists A and sends a steering message.
4. A finishes its current safe unit, appends the message, and starts the next
   LLM turn with the message present.
5. The database records delivery and acknowledgement exactly once in the
   normal path.
6. B sends a request to a completed subagent owned by A; A reactivates it and
   returns a reply correlated by `reply_to_message_id`.

### Phase 6: Operational hardening

Add stale-session cleanup, expired-claim recovery, bounded retention,
diagnostic status, database integrity tests, and documentation. Validate two
real `pi-cli` processes manually before considering mailbox participation
enabled by default in a later release.

ACP/RPC adapters can follow by calling the same coordinator; they must not
duplicate SQL or message-delivery policy.

### Phase 7: Optional A2A interoperability

After the local mailbox completion criteria pass, implement the A2A adapter in
the staged order defined above. This phase has its own completion gate and must
not delay or weaken the local mailbox invariants.

## Test matrix

### Store unit tests

- Fresh database creates schema version 1.
- Reopening an existing database is idempotent.
- A newer unsupported schema fails clearly.
- Two independent store instances can register and list concurrently.
- Workspace filtering is enforced for list and send.
- Session-addressed and instance-addressed messages route correctly.
- Duplicate message IDs are rejected idempotently or return the original
  receipt according to the chosen API contract.
- Claims are exclusive while leased.
- Expired claims are recoverable.
- Acknowledgement requires the correct claim token and is idempotent.
- Messages retain creation order under concurrent writers.
- Busy timeout produces a typed error.
- Retention removes only eligible acknowledged/stale rows.

### Coordinator tests

- Registration, heartbeat, state changes, session switching, and shutdown
  update presence correctly.
- One process hosting many root/subagent endpoints still owns exactly one
  maintenance worker and one store connection.
- A crashed/abandoned instance becomes stale after lease expiration.
- The coordinator stops all threads before destroying the store/session.
- An active agent receives a `steer` envelope at the next safe boundary.
- An idle interactive agent does not spontaneously start a turn.
- `note` messages are never automatically injected.
- A failed acknowledgement leaves a recoverable claim.
- Stop requests cancel mailbox waits promptly.
- Spawn/status/close task events create, update, and close subagent endpoints.
- Completed subagents remain live and a mailbox request reactivates them.
- Closing a subagent recursively closes its descendants and their endpoints.
- Remote close attempts are rejected.

### Lua tests

- Every primitive converts successful results to stable Lua tables.
- Expected failures return `nil, error`.
- Missing/disabled mailbox bindings do not crash addon loading.
- Tool wrappers validate mutually exclusive `session_id` and `agent_id`.
- Tool schemas contain no implementation-only SQL fields.
- The five mailbox tools plus local `agents_close` are visible when mailbox is
  enabled; raw claim/ack/wait/status operations are not model-visible.
- `agents_request` ignores unrelated generations/messages, returns a pending ID
  on timeout, and later exposes a late correlated reply through the inbox.

### Integration tests

- Two simulated processes using separate SQLite connections discover each
  other and exchange messages.
- Two sessions in different workspaces remain isolated by default.
- Global scope works only when explicitly enabled.
- Multiple activations of one durable session are visible and instance routing
  is deterministic.
- Root and subagent endpoints are distinguishable and directly addressable.
- Crash-after-claim results in redelivery with the same message ID.
- Existing local `pici.agents` child-task behavior remains unchanged.
- Ordinary pici operation works when mailbox support is disabled.

### Verification commands

During implementation, add focused targets such as `test-mailbox` and extend
`test-lua-tool`. Before any commit:

```sh
make format
make lint
make test
```

Or run the equivalent full CMake build and CTest commands documented in
`AGENTS.md`.

## Observability and diagnostics

Add verbose-only diagnostics for:

- resolved mailbox path and schema version;
- local agent/session identity;
- presence registration and clean shutdown;
- heartbeat health changes, without logging every successful heartbeat;
- message IDs, sender/recipient IDs, kind, and state transitions;
- claim expiry/redelivery;
- SQLite busy/corruption errors.

Never log message text unless an explicit trace mode is enabled. A future
`pici.mailbox.status()` primitive can expose database health, schema version,
local identity, last heartbeat, and queue counts without revealing message
bodies.

## Rollout and compatibility

1. Land the store and coordinator behind `[mailbox] enabled = true`.
2. Land native Lua bindings with stable result shapes.
3. Automatically load the bundled addon when enabled and exercise it with
   multiple local processes.
4. Add between-turn delivery after inbox/list/send behavior is stable.
5. Add subagent endpoint mirroring, reactivation, and local close integration.
6. Consider making mailbox enabled by default only after startup latency,
   permissions, privacy, and concurrent process behavior are verified.
7. Add A2A as an optional edge adapter only after the mailbox protocol is
   stable; do not make A2A a prerequisite for local coordination.

No migration of existing sessions is required. A session becomes discoverable
the next time it is activated by a mailbox-enabled binary. Existing addons that
use `pici.agents` continue to work unchanged.

## Completion criteria

The feature is complete when:

- Two independently launched `pi-cli` processes in the same workspace register
  distinct agent instances in one SQLite database.
- A Lua addon in either process can list the other through
  `pici.mailbox.list()`.
- The addon can send a durable message to a session or specific live instance.
- The addon can send a bounded request and receive only its correlated reply;
  late replies remain durable after timeout.
- The recipient can inspect, claim, and acknowledge the message through Lua.
- A `steer` message sent while the recipient is running appears in its context
  at the next safe turn boundary and triggers another LLM turn.
- A crashed receiver's claimed message becomes available again after its lease.
- Stale presence is distinguished from live presence without relying on clean
  shutdown.
- Workspace isolation and database permissions are covered by tests.
- Existing child-agent tools and session JSONL persistence remain compatible.
- Subagents appear as endpoints without per-subagent heartbeat threads;
  completed children remain addressable and can be reactivated by follow-up.
- `agents_close` recursively closes only locally owned subagents, while remote
  close attempts and root close attempts fail.
- The bundled Lua addon exposes five mailbox intentions plus local
  `agents_close`, with delivery bookkeeping hidden from the model.
- Focused tests, formatting, lint inspection, and the full test suite pass.

The optional A2A phase is complete when an explicitly configured external
agent can be discovered and invoked through the same high-level tools, inbound
Send Message/Get Task can route to an exported pici capability, A2A task IDs
remain distinct from reusable pici subagents, direct and asynchronous results
map correctly, and artifacts/cancellation/authentication obey the adapter
boundary above.

## Open questions to resolve before implementation

1. Should a session-addressed message be claimable by any one live activation,
   or should multiply attached sessions require the sender to choose an
   `agent_id`?
2. Is workspace identity the Git worktree root, the context-discovery root, or
   an explicit configured path when those differ?
3. Should inbox messages be shown to the human in the terminal status area as
   well as exposed to the agent?
4. Should automatic steering claim only messages sent while the target is
   already running, or also drain previously queued `steer` messages when a new
   explicit turn starts?
5. Is system SQLite an acceptable required build dependency on every supported
   platform, or should the official amalgamation be vendored?
6. Should completed subagents ever receive an opt-in idle TTL, or should all
   cleanup remain explicit?
7. Should A2A bridge tables live in `mailbox.sqlite3` under modular migrations,
   or in a separate database once artifacts are supported?
8. Which pici capabilities are eligible for explicit Agent Card export, and
   what authentication policy is required before enabling inbound A2A?

Recommended initial answers are: enabling mailbox automatically loads the
bundled addon; require `agent_id` when a session is multiply attached; use the
resolved context/workspace root; show a small unread count to the human; drain
queued steering at explicit run start; use system SQLite first; and keep
subagent cleanup explicit in v1. For A2A, keep bridge tables in the same
database initially but behind separate store interfaces, and require explicit
capability export plus authentication for every inbound deployment.
