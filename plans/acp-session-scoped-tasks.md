# ACP session-scoped tasks and correlated human replies

> Status: design only (tracker task 178; `plans/feature-gap-audit.md` order
> item 5). No application code changes. Object names follow
> [object-taxonomy.md](../docs/object-taxonomy.md); vocabulary follows
> [architecture-lexicon.md](../docs/architecture-lexicon.md). "ACP run" always
> means the protocol resource; "session" means a durable session activation.

## Problem

ACP exposes two unrelated delegation worlds:

- `/runs` (with optional `session_id`) drives one `SessionRuntime` per durable
  session, found or created in a map local to `register_routes()`
  (`src/acp/handlers.cpp`, `find_or_create_session_runtime`). Those runtimes
  are **never `activate()`d**, so `agent().task_tree()` is null: a model run
  under `/runs` cannot delegate, and nothing can inspect its tasks.
- `/tasks/*` addresses a single process-wide *scratch* `SessionRuntime`
  (`run_server()` in `src/acp/server.cpp`) with no `SessionStore` identity,
  one `TaskEventHub`, and no `session_id` anywhere on the wire. Task IDs
  (`root`, `agent_1`, ...) are unique only within one `TaskTree`, so they
  cannot be resolved across sessions.

Consequences: HTTP clients cannot delegate on behalf of a durable session, task
state is lost with the process and not associated with the transcript that
spawned it, and there is no typed channel for a delegated or running agent to
ask a human a question or request approval (today: free text, plus Lua denial
hooks that ACP excludes).

Non-goals (preserved deliberately): ACP keeps its capability exclusions
(`enable_mailbox`, `enable_hooks`, `enable_skills`, `enable_context_files`,
`enable_auto_compaction` all false); no durable child tasks (audit item 4);
no recursive delegation (item 1/176); no write-tool children (item 6);
task IDs stay tree-scoped, never globally unique.

## 1. Current vs target owner, per fact

| Fact | Today | Target live authority | Durable authority |
|---|---|---|---|
| Open session runtime for session `S` | ACP-local `unordered_map` + mutex built inside `register_routes`, never evicted | Process-owned open-session registry (`PiciProcess`, per taxonomy); until `PiciProcess::open_session` exists, an ACP-adapter-local registry extracted from `handlers.cpp` into its own file, flagged migration-only with removal phase "PiciProcess::open_session lands" | none (runtime is an activation) |
| Task tree for session `S` | none for `/runs`; one scratch tree for `/tasks` | `Agent` of `S`'s `SessionRuntime` (`agent().task_tree()`), created by `SessionRuntime::activate()` | parent journal only when explicitly recorded (unchanged) |
| Task status/result/generation | scratch `TaskTree` | the owning `TaskTree`, one per session | none |
| Task ID scope | one tree | one tree; **always qualified by session** on the wire | n/a |
| Task event stream/backlog | one process-wide `TaskEventHub` | one `TaskEventHub` **per session registry entry** (adapter-owned fan-out of that tree's `AgentTaskEventCallback`; Projection of tree events, never authority) | none |
| Pending question/approval (domain) | none | `Agent` (pending execution state; blocks the run at the request point) | none (not journaled in v1) |
| Question/approval presentation, HTTP/SSE delivery, reply lookup | none | ACP adapter, per session entry | none |
| `/runs` model / transcript | `AgentState` | unchanged | `SessionStore` |
| Scratch tree (legacy `/tasks`) | process-wide | unchanged, **compatibility only**, same removal phase as legacy routes (§2.4) | none |

Rules: one live authority per fact; the adapter registry owns lookup and
event fan-out but never task state, model state or pending-request state.
Registry entries do not own the `SessionStore`.

## 2. Session addressing and compatibility

### 2.1 New wire surface

Session is a path component, not a body field or header, so it is visible in
logs and cannot be omitted by accident:

```text
GET  /sessions/{sid}/tasks                  list (?path_prefix=)
GET  /sessions/{sid}/tasks/events           poll or ?stream=1 SSE
GET  /sessions/{sid}/tasks/{id}             snapshot
POST /sessions/{sid}/tasks                  spawn
POST /sessions/{sid}/tasks/wait
POST /sessions/{sid}/tasks/{id}/message
POST /sessions/{sid}/tasks/{id}/follow-up
POST /sessions/{sid}/tasks/{id}/interrupt
POST /sessions/{sid}/tasks/{id}/close
POST /sessions/{sid}/requests/{request_id}/reply     (§3)
GET  /sessions/{sid}/requests                          pending list (§3)
```

Request/response bodies are exactly today's bodies (same
`task_snapshot_json`, same error envelope, same `after_generation` semantics),
so the handler bodies are reused with the tree and hub resolved from the
session instead of captured at registration. Every task response, snapshot,
event record and wait result additionally carries `"session_id": "<sid>"`.
Event records gain a per-session `sequence` (already per hub).

`sid` is the same string `/runs` accepts as `session_id`. It must name a
session whose runtime is registered in this server process.

### 2.2 Session resolution and lifecycle

- **Resolve, don't create.** A session-qualified `/tasks` route resolves an
  already-registered runtime; unknown `sid` is `404 session_not_found`. A
  durable session that exists in the `SessionStore` but has no registered
  runtime (server restarted) is also 404 until a `/runs` call with that
  `session_id` reopens it. Rationale: creating a runtime on a GET is a hidden
  durable write and an easy resource-exhaustion vector. A later opt-in
  `POST /sessions/{sid}/open` is out of scope.
- **Activation at registration.** The registry activates a runtime exactly once
  on creation (`SessionRuntime::activate(child_options, limits,
  ChildWriteTools::none, event_callback)`) *before* publishing it. Today a
  `/runs` runtime is not activated; this plan makes every registered runtime
  activated. Limits are `TaskTree::Limits{}` defaults and write tools stay
  `none`, matching the scratch tree. Sequencing matters: `activate()` must
  precede the first run and throws on a second call, so creation, activation
  and insertion happen under the existing "construct outside the lock,
  try_emplace, loser discarded" pattern, with activation done on the winner
  only (the loser is destroyed unactivated).
- **Does activation change `/runs` behaviour?** It makes `agent().task_tree()`
  non-null. Whether the model sees delegation tools is decided by the tool
  list, which ACP passes unchanged (`cfg.tools`); this plan does **not** add
  delegation tools to ACP model runs. So `/runs` output is unchanged; the tree
  only becomes reachable through the HTTP surface. Stage 1 must assert that
  tool schemas given to the model are byte-identical before/after.
- **Eviction/close.** Out of scope (the registry never evicts today). The plan
  records the obligation: closing a registry entry must follow taxonomy
  shutdown order (stop admitting, close tasks, then destroy runtime). Until an
  eviction story exists, every entry's tree lives until server shutdown; the
  per-session `TaskTree::Limits` (active/resident caps) bound growth per
  session. Server shutdown must close registry entries before stores die.
- **Concurrency.** Registry mutex guards only lookup/insert (as today). A
  route handler holds a `shared_ptr` to the entry for the call; entries are
  never removed while the server runs, so no use-after-erase.

### 2.3 Scope preserved

- Task-ID scope is tree scope: `agent_1` in session A and `agent_1` in session
  B are different tasks. No route searches all trees for an ID. A task ID that
  does not exist in the addressed session is `404 not_found` even if another
  session has it (isolation test).
- Events: a client only ever receives events of the addressed session's tree.
- Capability exclusions unchanged (list above). Child sessions still get no
  mailbox/hooks/skills and read-only tools.

### 2.4 Compatibility for existing clients

Existing clients use unqualified `/tasks/*` and have no `session_id`.

1. **Keep legacy routes unchanged and keep the scratch tree** for at least one
   release. Behavior, bodies, status codes and event shapes are byte-identical
   (additive `session_id` is **not** added to legacy responses: they have
   none, and `"session_id": null` is avoided to keep golden clients stable).
   `test_acp`'s existing `AgentTasks` test stays untouched and must keep
   passing as the compatibility proof.
2. Stage 3 adds a `Deprecation: true` header and
   `Link: </sessions/{sid}/tasks>; rel="successor-version"` to legacy
   responses. Headers are additive and ignored by body-only clients; Stage 1
   and 2 leave legacy responses untouched.
3. Advertise capability: `GET /agents/{name}` manifest metadata gains
   `"session_tasks": true` (additive key) so clients can feature-detect
   without probing for 404.
4. Removal or aliasing of legacy `/tasks/*` (e.g. making them an alias to a
   configured default session) **is not part of this plan**. It would change
   semantics for any client relying on the scratch tree having no transcript,
   and is the user's call. See Open question 1.
5. The scratch tree is documented as *"detached scratch delegation, not
   attached to any session, not persisted"*; the existing rationale comment in
   `server.cpp` is replaced with a pointer to this plan.

Error contract for the new routes (extends the existing envelope
`{"error":{"code","message"}}`): `session_not_found` (404), `bad_session_id`
(400: empty or fails `session_id` validation used by `SessionStore`),
existing task codes unchanged. Session-ID path segments must be URL-decoded
once and validated with the same predicate the store uses before any lookup,
so `..` or separators never reach a filesystem path.

## 3. Typed question and approval Values

Pici has text conversation and Lua denial hooks only. Required: a request the
model-run (parent or child) can raise, a human reply correlated to it, native
enforcement, and per-frontend presentation. Layering follows the audit:
**Values cross the Session contract; Agent owns pending state and
enforcement; adapter owns wire/UI state.**

### 3.1 Values (domain, `src/core`, no HTTP/JSON-wire/terminal types)

Placed with the Session contract values, not in transcript or `event_json`:

```text
HumanRequestId      Value  opaque string; unique per Agent activation
                           (monotonic counter + activation nonce so a stale id
                           from a previous activation can never match)
HumanRequest        Value  { id, origin{agent_id, task_id?, tool_call_id?},
                             kind: question | approval,
                             prompt: string (bounded),
                             choices?: [{key,label}] (bounded, question),
                             action?: { tool, args_summary } (approval),
                             issued_generation }          // run generation
HumanReply          Value  { id, outcome: answered | approved | denied |
                                          cancelled,
                             answer?: string (bounded), choice_key? }
HumanRequestResolution Value  { id, how: replied | cancelled_run |
                                          closed_task | superseded | shutdown }
```

Events (typed, added to the existing event variant, not new transport):
`HumanRequestedEvent{HumanRequest}`, `HumanResolvedEvent{Resolution}`.
Session contract command: `SessionRuntime::reply(HumanReply) ->
ReplyResult{accepted | unknown_id | stale | already_resolved | not_pending}`.
`ReplyResult` is a Value; unlike `RunResult` it never throws for
protocol-level outcomes. Archetype statement for review (taxonomy checklist):
these are Values; no new Aggregate/Manager is introduced.

### 3.2 Agent owns pending state and enforcement

- `Agent` owns the pending-request table (`id -> promise/slot`, bounded, e.g.
  16 outstanding; excess requests resolve immediately as `denied`). It is
  part of `AgentState`'s run-scoped state: cleared on run end.
- **Approval** is enforced natively at the existing tool-call seam
  (`before_tool_call`): a tool marked as requiring approval by native policy
  suspends the tool call on a pending request, then proceeds only on
  `approved` and returns a blocked tool result (`reason: denied by user`)
  otherwise. No reply, cancellation, timeout or shutdown ⇒ **denied**
  (fail closed). The decision is never read from a frontend callback result
  that a frontend could forge without a matching live `id`.
- **Question** is raised by a native `ask_user` tool (bounded prompt/choices);
  the tool result to the model is the typed answer or an explicit
  `cancelled` marker, never silently empty.
- A question/approval is bound to the **run generation**; replies are
  accepted only while that run is the active run (§3.4).
- Child tasks: a child agent's request is emitted on the **child's** event
  stream with `origin.task_id` set and is surfaced to the human through the
  *root* session's request list (the human talks to the session, not to a
  child). Replies are routed by `HumanRequestId` to the owning child `Agent`
  by `TaskTree`; the tree must therefore expose "route reply to task"
  through the Delegation contract (a new `reply(task_id, HumanReply)`
  alongside spawn/message/interrupt), keeping `TaskTree` the only owner of
  child access. Children inherit only what the root's native policy allows;
  until audit item 1/6 define child tool policy, **child approvals default to
  denied and children get no `ask_user` tool** (v1 scope fence; the Values
  and routing are designed now, enabled later behind that policy).
- Idle-only commands: `reply()` is *not* idle-only (it targets a live run) but
  must not mutate session/model/compaction state.
- Durable journaling of requests/replies is out of scope for v1 (a pending
  request cannot survive its run). If later journaled, journal the
  resolution before releasing the blocked tool (durable-write-before-live).

### 3.3 Adapter-owned wire/UI state (ACP)

- DTOs are ACP types in `src/acp/types.{h,cpp}`, distinct from the Values:
  `{ "request_id", "session_id", "run_id"?, "kind", "prompt", "choices",
  "action", "task_id"? }` and reply
  `{ "outcome": "answered|approved|denied|cancelled", "answer", "choice_key"
  }`. Translation functions live in the adapter; core never sees JSON.
- SSE on a streaming `/runs` response: new event `request.created`
  (carrying the DTO) and `request.resolved` with `how`. The run's ACP status
  uses the **already declared but unused `RunStatus::awaiting`**: emitted as
  a `run.awaiting` event while at least one request is pending, and
  `run.in-progress` again after the last resolves. Sync-mode runs
  (`mode: sync`) cannot be answered in-band; for those, a request is either
  answerable via the out-of-band route within the run's lifetime or
  resolves as `denied`/`cancelled` on a bounded server-side timeout
  (adapter policy, default 60 s, configurable; the Agent just sees the
  reply). Stage 4 must document that sync + approval-requiring tools is a
  client contract hazard.
- `POST /sessions/{sid}/requests/{request_id}/reply` and
  `GET /sessions/{sid}/requests` (pending only) are the out-of-band path and
  the only reply path for sync runs. The adapter keeps a small per-session
  map `request_id -> {run_id, delivered}` solely to render DTOs and produce
  accurate errors; it is a cache, not authority: `Agent` decides acceptance.
- Child-task requests also appear on `/sessions/{sid}/tasks/events`
  (`task.event` wrapping the request event) and in the session's pending list
  with `task_id`.
- HTTP codes for reply: `200` accepted; `404 request_not_found` (id never
  issued in this session or for another session); `409 request_stale`
  (run ended/cancelled/generation mismatch, or request resolved already);
  `400` malformed outcome/choice. Replies are idempotent only in the sense
  that the same reply to an already-resolved id returns `409
  request_already_resolved` without changing state.

### 3.4 Correlation, stale and cancelled replies

1. Reply is matched on `(session_id, request_id)`; `request_id` embeds the
   activation nonce, so ids from a previous server process or runtime never
   match. A reply posted to session B using session A's id is `404`
   (cross-session isolation).
2. Run end for any reason (completed, failed, cancelled, `Agent::interrupt`)
   resolves all of that run's pending requests as `cancelled_run` *before*
   the run is reported finished; the blocked tool calls observe `cancelled`
   and the run unwinds normally. A late reply then finds `request_stale`.
3. Task close/interrupt resolves that task's requests as `closed_task`.
4. Server shutdown resolves everything as `shutdown` (denied) before runtimes
   are destroyed; no thread may be left blocked on a promise.
5. A reply racing a cancellation is decided by the Agent's single lock: first
   writer wins; the loser gets `409` and the model sees exactly one outcome.
6. Duplicate request ids are impossible by construction; duplicate/replayed
   replies never resolve twice.
7. A request whose stream consumer disconnected stays pending (reply route
   still works) until the run ends; it is **not** auto-approved.
8. Bounds: prompt/answer/choices byte caps and a pending cap, rejected at
   Value construction in core, so the adapter cannot bypass them.

## 4. Test matrix

All tests use the in-process fixture in `test/test_acp.cpp` (fake provider, no
network). New Agent/Session-level tests live in existing core test targets.

| # | Area | Case | Layer |
|---|---|---|---|
| T1 | Compat | existing `AgentTasks` flow over legacy `/tasks/*` unchanged (bodies, 404/409 codes, event shapes) | acp |
| T2 | Compat | legacy responses contain no `session_id`; manifest has `session_tasks:true` | acp |
| T3 | Addressing | spawn/get/list/message/follow-up/interrupt/close/wait/events through `/sessions/{sid}/tasks*` mirror T1 flow | acp |
| T4 | Isolation | spawn `agent_1` in S1 and S2; each GET/list/wait/close sees only its own; close in S1 leaves S2 running | acp |
| T5 | Isolation | `GET /sessions/S2/tasks/<S1-only id>` ⇒ 404 `not_found` (no cross-tree search) | acp |
| T6 | Isolation | events stream of S1 never contains S2 events; generations are per session; `after_generation` of one session is meaningless in another | acp |
| T7 | Isolation | scratch tree and session trees are disjoint: legacy `/tasks` never lists session tasks and vice versa | acp |
| T8 | Resolution | unknown sid ⇒ 404 `session_not_found`; GET never creates a runtime or journal file; bad/`..` sid ⇒ 400 | acp |
| T9 | Lifecycle | `/runs` with session_id registers an activated runtime; second `/runs` reuses it; racing creators yield one activated runtime, loser unactivated and destroyed | acp |
| T10 | Compat | `/runs` model-visible tool schemas identical before/after activation; capability exclusions still off (no mailbox/hooks/skills) | acp/core |
| T11 | Limits | per-session `TaskTree::Limits` enforced independently (resident cap in S1 does not consume S2's) | acp |
| T12 | Concurrency | task ops on S1 while a `/runs` is in flight on S1 and S2; no deadlock; ASan/TSan clean | acp |
| T13 | Values | `HumanRequest`/`HumanReply` bounds; unique ids; ids from another activation rejected | core |
| T14 | Agent | approval-gated tool: approved ⇒ runs; denied/cancelled/no reply/timeout/shutdown ⇒ blocked result, never executed (fail closed) | core |
| T15 | Agent | `ask_user` returns typed answer / explicit cancelled marker to the model | core |
| T16 | Agent | pending cap exceeded ⇒ immediate denied; table empty after run end | core |
| T17 | Stale | reply after run completed / after cancel / for resolved id ⇒ `request_stale`/`already_resolved`, no state change | core+acp |
| T18 | Cancel race | reply vs `Agent::interrupt` race (loop N times) ⇒ exactly one outcome, no hang | core |
| T19 | Isolation | reply to S2 with S1's `request_id` ⇒ 404, S1's request still pending | acp |
| T20 | Wire | SSE emits `request.created`, `run.awaiting`, `request.resolved`, `run.in-progress`; DTO shapes; disconnect keeps request pending | acp |
| T21 | Wire | sync-mode request resolves by out-of-band reply or by adapter timeout; never hangs the worker thread | acp |
| T22 | Children | child task request surfaces on root session list with `task_id`; routed reply reaches only that child; v1 default denies child approvals and exposes no `ask_user` to children | core+acp |
| T23 | Shutdown | server stop with pending requests unblocks and denies; no leaked threads | acp |
| T24 | Docs | `make format`, `git diff --check`; docs reference wire surface | docs |

## 5. Staged implementation breakdown

Each stage is a separate session/commit with the repo gate (`make format`,
`make test` with `-j2`, relevant narrow targets first). Stages 1-3 are
independent of 4-7.

### Stage 1 — Session registry extraction and activation
*Scope:* move the `/runs` runtime map out of `register_routes` into an
adapter-local registry type (`src/acp/session_registry.{h,cpp}`; archetype:
Adapter-owned lookup table, owner: ACP server, lifetime: server run, state
authority: none, contract: Session; migration-only, removal phase =
`PiciProcess::open_session` landing). It owns entry = runtime +
`TaskEventHub`; activates on creation with the hub publisher; deterministic
shutdown closes entries before the store is destroyed.
*Acceptance:* T9, T10, T12 (run side); `/runs` behaviour and tool schemas
unchanged; legacy `/tasks/*` untouched (T1 passes); no new public
`Manager`/`Service` names; clang-tidy clean for touched files.
*Risk:* `activate()` throws if called twice; activation only on race winner.

### Stage 2 — Session-qualified task routes
*Scope:* register the §2.1 routes; refactor the task handlers to take a
"resolve tree/hub" callable so legacy routes (fixed scratch resolver) and new
routes (registry resolver) share one body; add `session_id` to new
responses/events; error codes `session_not_found`, `bad_session_id`;
validate sid.
*Acceptance:* T3-T8, T11; legacy bytes unchanged (T1/T2 golden comparison).

### Stage 3 — Compatibility surface and docs
*Scope:* manifest `session_tasks`, `Deprecation` header on legacy routes,
replace scratch rationale comment in `server.cpp`, document wire surface in
a new `docs/acp-wire.md` (or the existing ACP doc location if one is found),
update `plans/feature-gap-audit.md` row "Frontend parity" and AGENTS.md if
ownership statements change.
*Acceptance:* T2, T24; docs reviewed against taxonomy; Open question 1
resolved or explicitly deferred in the doc.

### Stage 4 — Human request Values and Session contract
*Scope:* §3.1 Values with bounds, `HumanRequested/ResolvedEvent` in the event
variant (with `event_json` rendering), `SessionRuntime::reply()` forwarding to
`Agent`; no behaviour yet beyond table + resolution.
*Acceptance:* T13, T16, T17 (core part); existing RPC/CLI event consumers
compile and ignore the new events; no frontend DTO leaks into core.

### Stage 5 — Agent enforcement
*Scope:* pending-request table; approval at `before_tool_call` seam with
native "requires approval" policy flag on `ToolDefinition` (default false so
nothing changes); native `ask_user` tool (opt-in per frontend; ACP enables it
only when configured); run-end/interrupt/shutdown resolution (§3.4).
*Acceptance:* T14, T15, T17, T18; fail-closed proven by test; threads never
left blocked (TSan run of T18).

### Stage 6 — ACP request wire and UI state
*Scope:* DTOs, SSE events, `run.awaiting` status, reply/list routes, adapter
cache, sync-mode timeout policy, session-scoped isolation of ids.
*Acceptance:* T19, T20, T21, T23; ACP's other behaviour unchanged.

### Stage 7 — Child requests (gated)
*Scope:* `TaskTree` reply routing via the Delegation contract, surfacing
child requests on root list/events. **Do not start until** audit item 1
(child tool policy) is settled; ship disabled-by-default with the v1 fence.
*Acceptance:* T22.

Ordering: 1 → 2 → 3; 4 → 5 → 6 → 7. Stage 6 depends on 1 (registry) and 5.

## Open questions for the user

1. **Legacy `/tasks/*` fate.** Keep the scratch tree indefinitely
   (recommended for now), or after a deprecation release alias unqualified
   routes to a designated/default session (breaking: tasks gain a transcript
   and persist across clients) or remove them (breaking)? The plan is
   additive and does not require this answer to start Stages 1-3.
2. Should `ask_user`/approval be enabled for ACP by default or only via
   server config? Plan assumes opt-in config.

## Unverified assumptions

- `SessionRuntime::activate()` is safe to call on a runtime that has not yet
  opened a session and before the first `run_prompt` (inferred from
  `session_runtime.h` comments; not tested).
- `before_tool_call` can block on a pending request without starving the
  agent loop's stop-token handling; Stage 5 must confirm and, if not, add a
  stop-token-aware wait.
- Whether the lexicon wants `HumanRequest` named differently
  (`UserInputRequest`); naming is open to review with Stage 4.
