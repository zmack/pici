# Mailbox identity and autonomous-turn follow-up

## Status and purpose

This is the execution plan for the two gaps discovered after mailbox v1:

1. A model cannot reliably tell which mailbox endpoint it is acting as.
2. An idle interactive root can receive a durable request but does not get an
   agent turn in which to answer it.

This plan follows `agent-mailbox-and-session-coordination.md` and deliberately
supersedes the v1 execution manifest's exclusion of automatic idle-root
execution. It does not reopen the mailbox storage design or introduce A2A.
Ordinary delivery to a running agent still waits for a safe model/tool boundary;
only the interactive CLI's main loop may claim and run work for an idle root.

Follow `AGENTS.md` while implementing it. Keep commits reviewable, run a narrow
build/test target during each phase, run `make format` and inspect `make lint`
before each commit, and run `make test` before every commit.

## Implementation status

The native identity, caller-scoped tools, request-local context, wakeable
readline, and autonomous idle-root phases are implemented on the CLI path:

| Phase | Status | Commit |
| --- | --- | --- |
| 1. Native runtime identity and endpoint registration | Complete | `41c227d` |
| 2. Caller-scoped Lua tools and child exposure | Complete | `a9307b2` |
| 3. Cache-preserving request-local identity context | Complete | `c52d2d9` |
| 4. Wakeable readline | Complete | `14a4236` |
| 5. Autonomous idle-root turns | Complete | `54c0a4f` |
| 6. Documentation and verification | Complete | documentation commit |

The implemented v1 boundary is deliberately narrower than a general agent
transport: mailbox storage/coordinator APIs are reusable by other hosts, but
the self-pipe wake adapter and autonomous-turn loop currently belong to the
interactive CLI. ACP and future GUI hosts need their own event-loop adapters;
A2A transport, remote lifecycle control, and installed bundled-addon packaging
remain follow-up work. The documentation updates in this phase are intentionally
uncommitted so the parent review can inspect them with the implementation.

## Outcome

After this work:

- Every root and subagent has one immutable runtime identity for its current
  activation.
- The identity visible in model context, mailbox tools, presence rows, and
  delivery routing is the same identity.
- `agents_self` returns that identity, and `agents_list` marks the caller with
  `is_self`.
- The bundled Lua mailbox tools are usable by subagents under the subagent's
  authority rather than accidentally operating as the root.
- A `request` or `steer` delivered to an idle interactive root wakes readline,
  runs an autonomous turn on the CLI main thread, and then restores any
  partially typed input. The maintenance thread only emits a coalesced wake
  hint.
- `note` and `reply` remain inbox-only. A synchronous `agents_request` continues
  to observe its reply directly through correlation.
- Mailbox turns cannot recursively run without a local safety bound: up to 16
  actionable envelopes are batched per turn and at most 8 autonomous turns
  run consecutively before human input is required.

## Frozen design decisions

### Identity has one native source of truth

Add a core `AgentRuntimeIdentity` type that is not owned by the SQLite or Lua
layers. Its initial shape is:

```cpp
struct AgentRuntimeIdentity {
  std::string agent_id;
  std::string session_id;
  std::string kind; // "root" or "subagent"
  std::optional<std::string> task_id;
  std::optional<std::string> task_path;
  std::optional<std::string> owner_agent_id;
};
```

The mailbox coordinator still owns presence and endpoint registration, but it
must not infer the caller from one coordinator-wide `active_root_agent_id_`.
Each `Agent` receives its identity in `Agent::Options`; `AgentContext` exposes
the immutable value to request preparation and tool execution.

An activation gets a new `agent_id`. Resuming the same durable conversation
keeps its `session_id` but does not reuse the previous activation's `agent_id`.
Subagents share the durable root `session_id`, have their own `agent_id`, and
carry their local task ID/path.

### Do not synthesize an unmatched tool result

A raw `ToolResultMessage` is not a safe identity carrier. OpenAI-style and Muse
converters require it to correspond to an assistant tool call, and the task
context normalizer already removes unmatched results.

Use two complementary model-facing mechanisms instead:

1. Add `agents_self`, backed by `pici.mailbox.self`, for an authoritative,
   structured lookup.
2. Add one request-local runtime identity `UserMessage` to the beginning of the
   conversation message list. It is placed after the unchanged system prompt
   and before persisted transcript messages. Frame it explicitly as
   pici-generated runtime context, not user-authored content.

Example text:

```text
[pici runtime context; not user-authored]
mailbox agent_id=agt_...; session_id=sess_...; kind=subagent;
task_id=agent_2; task_path=/root/research
Use agents_self when you need the authoritative structured identity.
```

This message is assembled for each LLM request and is never appended to
`AgentState`, JSONL, compaction input, or inherited conversation history. Its
position and contents remain stable for the lifetime of an activation. The
system prompt therefore remains identical across agents and cacheable as a
shared prefix; subsequent requests by the same activation also have a stable
identity-plus-transcript prefix.

Do not fake an assistant tool call merely to make an identity tool result
protocol-valid. Do not put the activation ID in the system prompt.

### Tool calls are explicitly caller-scoped

Extend tool execution with immutable caller metadata. Do not use a process
global or thread-local current agent: tools can execute in parallel, and one
Lua addon instance may be shared by root and child agents.

The preferred API is a `ToolExecutionContext` passed to every
`ToolDefinition::execute` call:

```cpp
struct ToolExecutionContext {
  std::string_view call_id;
  std::optional<AgentRuntimeIdentity> actor;
  std::stop_token stop_token;
  ToolUpdateCallback on_update;
};
```

The exact field ownership may be adjusted to avoid dangling views, but the
actor must travel as call data, not mutable tool state. Built-in tools ignore
it. `InlineLuaTool` installs the actor in the Lua registry for the duration of
the protected call, next to the existing inline stop token, and clears it with
RAII before releasing the Lua mutex. Native mailbox callbacks receive that
actor explicitly and reject calls without a registered live actor.

All mailbox operations derive their sender/claimant from the execution actor:

- `self`: look up the actor's `agent_id`.
- `list`: calculate `is_self` against the actor.
- `send`, `request`, and `reply`: set sender agent/session from the actor.
- `inbox` and `ack`: claim or acknowledge for the actor's endpoint.
- `close`: retain the existing local child-only authorization, evaluated from
  the actor rather than assuming the root caller.

Every model-facing read must use the same recipient eligibility rule as a
claim:

```text
recipient_session_id == actor.session_id
AND (
  recipient_agent_id == actor.agent_id
  OR (recipient_agent_id IS NULL AND actor.kind == "root")
)
```

Apply recipient and kind predicates before `LIMIT`. The current store
`inspect()` implementation filters exact recipients and kinds after a
session-wide SQL `LIMIT`, and treats a null `recipient_agent_id` as visible to
any agent in the session. Fix that mismatch. An unscoped whole-session inspect
may exist only as an explicitly privileged diagnostic/store operation; Lua and
model tools never use it. `reply(message_id)` must prove that the original
message is eligible for the replying actor before sending. Workspace-wide
`wait()` notifications may remain broader because they are only wake hints and
the subsequent inspect/claim performs authorization.

### Endpoint allocation precedes agent execution

The current child runner can start before `AgentTaskSpawnedEvent` is observed,
while the coordinator allocates the mailbox endpoint only in that observer.
That race must be removed before child mailbox tools are exposed.

Introduce a synchronous endpoint-registration seam between
`AgentTaskManager` and `MailboxCoordinator`:

1. The task manager assigns the local task ID and path.
2. Outside the task-manager mutex, it asks the coordinator to register and
   return the child's `AgentRuntimeIdentity`.
3. It constructs the child `AgentSession` with that identity.
4. Only after registration and construction succeed does it start the runner.
5. Failure rolls back the presence row and the reserved task slot.

Do not invoke coordinator/store callbacks while holding the task-manager mutex.
The existing spawned/status/closed events remain observational; endpoint
creation is no longer an asynchronous side effect of the spawned event.

Root activation follows the same rule: `activate_root` returns or exposes the
registered identity, and startup installs it into the root agent before any
model request can run.

### Mailbox tools become an explicit child capability

Replace the `source_path() == "builtin"` and name allowlist in
`is_child_safe_tool` with explicit capability metadata on `ToolDefinition`.
Mailbox tools shipped by `addons/mailbox.lua` are marked child-safe; arbitrary
user addons remain non-inheritable by default.

The default child set becomes the existing read-only tools plus the curated
mailbox tools:

- `agents_self`
- `agents_list`
- `agents_send`
- `agents_request`
- `agents_reply`
- `agents_inbox`

Do not give a child `agents_close` in the first pass. A child can ask its parent
to close it; self-close during tool execution complicates runner ownership.
Raw `pici.mailbox.ack`, `wait`, status, claims, leases, and tokens remain below
the model boundary.

### Idle roots wake; the maintenance thread never runs an agent

Keep one mailbox maintenance thread per process. It detects actionable work
for an idle root and signals the interactive host through a nonblocking
self-pipe (portable across the project's POSIX targets). It must never call
Lua, the renderer, readline, `AgentSession::run_messages`, or model providers.

The main CLI thread remains the sole owner of interactive root turns:

```text
maintenance thread       main CLI thread
------------------       -------------------------------
detect pending request   poll(stdin, mailbox_wake_fd)
signal self-pipe    -->  return readline result: mailbox_wake
                         claim actionable root envelopes
                         run AgentSession::run_messages(envelopes)
                         persist/render normally
                         resume readline with saved draft
```

The wake signal is level-like. Coalescing multiple writes is acceptable; the
main thread drains the pipe and then drains a bounded batch of actionable
messages. A full pipe is not an error because it already represents a pending
wake.

### Claim only when a consumer can accept the turn

The maintenance thread may inspect enough state to decide that a wake is
needed, but it must not lease messages to an idle root. The main thread claims
them immediately before starting the autonomous turn. Existing envelope
acceptance callbacks acknowledge only after the agent loop accepts the input.
If execution cannot start, the unacknowledged lease expires and permits
redelivery.

Add coordinator APIs with intent-revealing names rather than exposing the
store directly, for example:

```cpp
bool idle_root_work_pending() const;
std::vector<AgentMessageEnvelope> claim_idle_root_turn(std::size_t limit);
```

`claim_idle_root_turn` must atomically verify the current root activation and
transition it to running before returning envelopes, or fail without claiming.
The RAII running guard returns it to idle after the turn.

### Readline returns a reason and preserves drafts

Change the interactive readline API from `optional<string>` to a tagged result:

```cpp
enum class ReadlineExit { submitted, eof, mailbox_wake };

struct ReadlineResult {
  ReadlineExit reason;
  std::string text;
};
```

Accept an optional initial draft and wake file descriptor. On a mailbox wake,
readline restores terminal mode, returns the current buffer without submitting
it, and lets the normal renderer own autonomous output. The next readline call
receives that buffer as its initial draft and redraws it with its cursor at the
previous logical position. Preserve both text and cursor index; a string alone
is insufficient when the user was editing in the middle.

The non-TTY fallback keeps its current blocking `getline` behavior. Print mode
and one-shot invocations exit normally and do not remain resident solely for
mailbox work.

### Autonomous-turn policy is narrow and bounded

Only `request` and `steer` trigger an autonomous root turn. `note` and `reply`
remain durable inbox items. A request envelope presented to the model includes
its `message_id`, sender identity, kind, and a direct instruction to answer
with `agents_reply(message_id=...)`. A steer envelope is ordinary additional
input and does not imply a correlated response.

Run at most 8 consecutive autonomous root turns before requiring human input.
Coalesce up to 16 actionable messages into one turn, preserving store order.
Expose both limits in mailbox config only if operational testing demonstrates
a need; start with named constants to avoid prematurely expanding config.

After the budget is exhausted, leave later messages pending, show an unread or
paused status, and return to readline. A submitted human turn resets the
consecutive-autonomous-turn budget. Do not auto-generate a reply when the model
does not call `agents_reply`; delivery and semantic response are separate.

## Implementation phases

### Phase 1: native runtime identity and registration

Suggested commit: `Give agents stable runtime identities`

- Add `AgentRuntimeIdentity` in a mailbox-independent core header.
- Carry optional identity through `Agent::Options`, `AgentState`, and
  `AgentContext` snapshots.
- Make root activation return the exact registered identity.
- Add the pre-run subagent registration seam and rollback path.
- Convert coordinator `self`, send, inspect, claim, acknowledge, and reply
  entry points to require an explicit actor.
- Remove any mailbox path that silently substitutes the root actor.

Tests:

- Root context identity equals its SQLite presence record.
- Every child gets a distinct endpoint before its first model/tool call.
- Root and child share `session_id` but not `agent_id`.
- A registration or runner-start failure leaves neither a resident task nor a
  live presence row.
- Closed and stale actors cannot operate on another endpoint's inbox.
- A child cannot inspect or reply to root session-addressed mail, and neither
  root nor sibling can inspect an exact child-addressed message.
- Recipient and kind filtering occur before the requested result limit, so
  unrelated earlier rows cannot starve an actor's inbox.

### Phase 2: caller-scoped Lua tools and child exposure

Suggested commit: `Scope mailbox tools to their calling agent`

- Add `ToolExecutionContext` and migrate all tool implementations and tests.
- Pass the current agent identity through sequential and parallel tool paths.
- Install/clear the actor around `InlineLuaTool` execution with RAII.
- Change `MailboxBindings::Callback` to accept the actor explicitly.
- Add explicit child-inheritance metadata to `ToolDefinition`.
- Mark only the curated mailbox tools as child-safe.
- Add `agents_self`; add `is_self` to `agents_list` output.

Tests:

- Parallel root/child calls cannot observe or send as one another.
- `pici.mailbox.self` fails with a stable authorization error when no actor is
  supplied.
- `agents_self` returns root identity for root and child identity for child.
- `agents_list` has exactly one `is_self=true` entry for each caller.
- A child request has the child's sender ID, and a reply routes back to that
  child.
- Untrusted addon tools remain unavailable to children.

### Phase 3: cache-preserving identity context

Suggested commit: `Expose request-local agent identity context`

- Compose a native request preparation step with addon `prepare_context`
  rather than replacing addon behavior.
- Insert one identity message before all persisted conversation messages.
- Keep the runtime message out of state, session JSONL, compaction, and context
  inheritance.
- Ensure the message is present on every provider request in a multi-tool turn.
- Document the protocol reason for not using `ToolResultMessage`.

Tests:

- Effective provider context is `system prompt`, runtime identity, transcript.
- The system prompt bytes are unchanged.
- Repeated requests for one activation produce an identical identity prefix.
- The runtime message is absent from raw agent state and saved JSONL.
- Context compaction and child inheritance do not duplicate it.
- OpenAI completions, Responses, and Muse conversions accept the context.

### Phase 4: wakeable readline

Suggested commit: `Wake readline for mailbox work`

- Add an RAII self-pipe/wake primitive with nonblocking close-on-exec ends.
- Poll stdin and the wake FD in TTY mode, handling `EINTR` and coalesced wakes.
- Return a tagged readline result and preserve buffer plus cursor position.
- Restore raw mode before handing control back to the CLI loop.
- Leave non-TTY behavior unchanged.

Tests use pipes or pseudo-terminals and must not depend on sleeps:

- Input submission and EOF retain existing behavior.
- A wake returns promptly with the exact draft and cursor position.
- Re-entering readline restores and redraws that draft.
- Multiple wake writes coalesce safely.
- Wake/EOF and wake/submission races have deterministic accepted outcomes and
  do not lose submitted text.

### Phase 5: autonomous idle-root turns

Suggested commit: `Run idle roots for mailbox requests`

- Have the coordinator signal when an idle root has pending `request` or
  `steer` work without claiming it on the maintenance thread.
- Add the main-loop mailbox-wake branch.
- Claim through the coordinator, mark the root running, and call
  `AgentSession::run_messages` with the claimed envelopes.
- Route events through the same persistence, usage accounting, title guard,
  diagnostics, and renderer path as a human turn.
- Restore the saved readline draft after the autonomous run.
- Add message framing that identifies kind, sender, recipient, message ID, and
  reply expectations.
- Enforce the batch and consecutive-turn limits.

Integration tests:

- An idle root receives a request, runs without human input, calls
  `agents_reply`, and unblocks the sender's `agents_request`.
- A request arriving while the root is streaming enters at the existing safe
  boundary and does not start a concurrent root run.
- A completed subagent still reactivates on a request and replies as itself.
- Notes and replies do not wake an idle root.
- A model that omits `agents_reply` does not cause an invented reply.
- Failure before envelope acceptance redelivers; acceptance acknowledges once.
- Eight consecutive autonomous turns run; the ninth remains pending until a
  human submission resets the budget.
- Partially typed input survives autonomous rendering unchanged.
- Shutdown closes the wake FD, stops the poller, and cannot call a destroyed
  readline, renderer, session, or task manager.

### Phase 6: full verification and documentation

Suggested commit: `Document autonomous mailbox turns`

- Update the mailbox architecture HTML and user-facing mailbox documentation.
- Explain activation identity versus durable session identity.
- Document `agents_self`, child mailbox capability, wake policy, safe-boundary
  interruption, autonomous-turn limits, and non-TTY behavior.
- Record that ACP and future GUI hosts consume the same coordinator pending-work
  API but need their own event-loop adapter; the CLI self-pipe is not the
  architecture boundary.

Before each phase commit:

```sh
make format
make lint
make test
```

Fix every new warning in touched code. Do not combine unrelated cleanup or the
existing untracked Lua subsystem plan with these commits.

## Main implementation hazards

### Provider-valid context versus cache reuse

Changing the system prompt per activation damages its reusable prefix. An
unmatched tool result is invalid. A stable, request-local first conversation
message gets the useful properties of both approaches: protocol validity,
unchanged system prompt, and no transcript pollution. Tests must assert the
actual request-ready context, not merely raw agent state.

### Shared Lua state and parallel tools

The Lua mutex serializes calls within one addon state, but that does not make a
mutable, coordinator-wide identity correct. Actor identity must enter with the
specific tool invocation and live only for that protected call. Native
callbacks must copy the actor they need before returning or scheduling work.

### Spawn registration race

Simply adding `agents_self` exposes the existing race in which a child begins
before its mailbox endpoint exists. Endpoint allocation and presence
registration must become part of child construction, with rollback, rather
than remain an observer side effect.

### Readline and renderer ownership

The maintenance thread cannot paint a notification or run a turn safely.
Readline owns the input row and the viewport renderer owns assistant/status
output. Returning control to the main thread, restoring terminal mode, and
later redrawing the saved draft preserves those ownership boundaries.

### Claims and wakeups are different state

A wake is only a hint that work may exist. Claiming is durable delivery state.
Conflating them creates long leases while the UI is busy and makes shutdown
redelivery slow. Always re-check and claim on the consuming main thread.

### Autonomous coordination loops

Two agents can request work from each other indefinitely even when delivery is
correct. The local consecutive-turn budget limits resource use without
discarding durable messages. A later orchestration layer may add hop/depth
metadata, but it is not required to make this follow-up safe.

## Completion criteria

This work is complete when all of the following are true:

- Root and subagent identity agree across model context, tools, delivery, and
  SQLite presence.
- No mailbox primitive can silently act as root on behalf of a child.
- The model can discover its identity without a tool call and verify it through
  `agents_self`.
- Identity injection leaves the system prompt unchanged and does not persist.
- An idle interactive root can autonomously answer a correlated request.
- No maintenance/background thread executes an agent or touches terminal UI.
- Human and mailbox root turns are serialized on the main CLI thread.
- Typed readline input survives an autonomous turn.
- Safe-boundary behavior for already-running agents remains unchanged.
- Child agents can use the curated Lua-first mailbox tools as themselves.
- Focused tests, `make format`, `make lint`, and `make test` pass with no new
  warnings in touched code.
