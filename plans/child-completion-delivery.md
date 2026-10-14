# Child completion delivery (task 175)

Closes item 2 of `plans/feature-gap-audit.md` ("Admit completion work to the
parent").

## Problem

A child task finishing only changes `TaskTree` status. The CLI activity
bridge redraws, ACP streams a status event, and `wait_agents` observes a
change, but nothing admits *input* to a parent that has already settled, so
the parent cannot continue synthesis without the user polling.

## Decisions

- Delivery is **opt-in** (`[subagents] deliver_completions = true`,
  `TaskTree::Limits::deliver_completions`). Off: no completion Value is built,
  no event is emitted, CLI/ACP/RPC behaviour is unchanged.
- Completion is admitted through the parent `Agent`'s input path
  (`Agent::admit_child_completion`), called by `TaskTree` at the child's
  terminal transition. Renderers/frontend callbacks never write transcripts.
- **Root `send_message` is rejected** (`invalid_state`), like `follow_up(root)`
  since task 174. Nothing consumes a passive root mailbox, so queuing there
  silently lost the message. Completion delivery does not reuse that path
  (it has its own bounded Agent-owned inbox); `steer_envelopes(root)` remains
  the mailbox/steering route.
- Only direct children of root are delivered (recursive delegation is 176).
- No new wake source: the CLI already wakes the REPL on task events.

## New nouns

| Noun | Archetype | Owner / lifetime | Live state authority | Contract |
|---|---|---|---|---|
| `ChildCompletion` (`core/child_completion.h`) | Value | built by `TaskTree` at a terminal transition; copied into the parent inbox and the event; identity is the key `(task_id, run_generation)` | none: a snapshot of facts `TaskTree` owns (status, result) | Delegation (TaskTree -> parent Agent) |
| `AgentTaskCompletionEvent` | Value (event) | emitted once per *admitted* completion through the existing task event callback | none | Delegation events; serialized `task.completion` |
| `ChildCompletionInbox` | part of `Agent` (input state), not a peer object | member of `Agent`, same lifetime, guarded by the Agent's own mutex | `Agent` is the sole authority for *pending* completion input; it also holds the bounded dedup/cancel records; `TaskTree` stays authority for child status and result | Input admission |

`run_generation` is a per-task counter incremented at each terminal run
transition inside `TaskTree` (also exposed on `AgentTaskSnapshot`). It tells
the n-th run of a resident, followed-up child from the (n-1)-th.

## Behaviour

- Delivered statuses: `completed`, `errored`, `interrupted` unless the
  interrupt cause was the parent side (`parent`, `replacement_task`,
  `shutdown`). Never for close/shutdown, or while the tree is shutting down.
- Content bounded by `Limits::max_completion_bytes` (4096, UTF-8 safe cut,
  `truncated=true`); full result stays readable via `TaskTree::get`.
- Dedup: the inbox keeps a bounded FIFO of admitted keys; a repeat is ignored.
- Cancellation/late: `TaskTree::close` cancels pending entries for the task
  and records the id so a later arrival is dropped; `Agent::reset()`, session
  restore/identity change clear the inbox; `Agent::interrupt(shutdown)` closes
  admission.
- Bound: 16 pending. Overflow rejects the newest and counts it; the next
  claimed batch carries one extra attributed note with the dropped count.
- Busy parent: pending entries are drained with the steering queue at the next
  turn boundary and enter the transcript as attributed user input
  (`InputProvenance::Source::child_completion`) through the normal loop.
- Idle parent: the frontend claims them (`Agent::claim_child_completions`) and
  runs a turn. CLI: on the existing wake, mirroring idle mailbox turns, sharing
  the autonomous-turn budget, with the readline draft preserved. RPC/ACP:
  report `task.completion` events; entries stay queued and are attributed
  steering input at the next run.
- Interrupted parent: pending entries survive (they are lifecycle facts) and
  are delivered at the next admission; an interrupt never auto-starts a turn.
- No wake loops: a claim drains the inbox, parent-caused child interrupts never
  produce a completion, and CLI completion turns are bounded by the budget.
- Child result text is untrusted model output; it is delivered as an
  attributed user message (header with task id/generation) and never as a
  system/tool message.

## RPC

`subscribe_tasks {enabled, child_events?}` streams `AgentTaskEvent`s as JSONL
via `core::task_event_to_json` (moved from `acp/task_events.cpp`, which now
forwards). Per-token `task.event` only with `child_events=true`. Snapshots gain
`run_generation`.
