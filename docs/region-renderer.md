# Region renderer architecture

This documents the semantic turn model behind `--render region`
(`src/core/region_renderer.{h,cpp}`): what a turn is, how streaming text is
classified into `WORK` versus `ANSWER`, why a mailbox `REPLY` block can only
ever be truthful, the boundary between core-owned structure and Lua-owned
tool presentation, and the wrapping/scrolling/diff-paint invariants that keep
the alternate-screen compositor stable.

See [faux control renderer testing](faux-control.md) for a deterministic way
to drive this renderer end to end without a model or real tools.

## Why a turn model, not a flat transcript

Earlier revisions of the region renderer kept one flat `vector<RegionBlock>`
of text and tool blocks. That flat model could not answer three questions
a user watching the terminal needs answered unambiguously:

1. What request caused this turn?
2. Which output is intermediate work (thinking, narration, tool calls)?
3. Which output is the completed answer, or an actual mailbox reply?

`RegionState` now owns an explicit sequence of turns:

```cpp
struct RegionTurn {
  std::vector<RegionRequestBlock> requests;
  std::vector<RegionTurnBlock> blocks;   // text | thinking | tool | reply
  bool complete{false};
};

struct RegionState {
  std::vector<RegionTurn> turns;
  std::size_t active_turn_index{0};
  bool has_active_turn{false};
  // ... legacy flat `blocks` remains for callers that never start a turn.
};
```

`on_turn_start()` opens one active turn; `on_request()` appends a
`RegionRequestBlock` to it. Multiple prompt envelopes batched into a single
agent turn (for example several queued mailbox messages) each get their own
request block and stay visibly ordered under one `REQUEST` heading — none of
them is silently dropped. A turn started without ever receiving a request
(a legacy or test caller) simply renders with no `REQUEST` section rather
than crashing.

Rendering walks `state.turns` in order and never reorders a completed turn:
completed turns are immutable history: only the active turn's blocks and
requests are still being appended to.

## The four sections: REQUEST, WORK, ANSWER, REPLY

```text
-- REQUEST ---------------------------------------------------
Why are parallel tool calls displayed out of order?

WORK
Checking how tool blocks are inserted...
  [slow-tool] running...
  [fast-tool] done

ANSWER
Tool blocks were ordered by completion rather than call order.
```

- `REQUEST` is rendered once per turn from `turn.requests`, wrapped with the
  same ANSI-aware physical-width logic as everything else
  (`request_heading()` / `append_request_lines()` in `region_renderer.cpp`).
  A local prompt needs no source suffix; non-local input adds one:
  `REQUEST | MAILBOX | /root/luna` or `REQUEST | FOLLOW-UP | /root`. Display
  name priority is task path, then session name, then a shortened agent ID,
  then `unknown sender` only if the source is known to be non-ordinary but no
  identity survived. Non-text user content (e.g. an image) renders as a
  stable `[image attachment]` placeholder — binary/base64 payloads are never
  painted.
- `WORK` groups thinking, intermediate assistant narration, and tool call
  regions. Contiguous work is grouped under one `WORK` heading; it is not
  repeated before every tool call (`append_section_heading()` only re-emits a
  heading when the section actually changes).
- `ANSWER` (or `ANSWER | TRUNCATED`) is the terminal assistant response for
  the turn — see classification below.
- `REPLY -> <recipient>` is a persistent, renderer-owned block that appears
  only after a native mailbox reply is confirmed queued — see the truthfulness
  section below. It is a distinct `RegionTurnBlock` variant, not a relabeled
  tool region, so it can never be silently replaced by tool output.

## Honest streaming: provisional text and atomic reclassification

The stop reason for an assistant message is unknown while its first text
deltas are still arriving, so the renderer never guesses. Every assistant
text block starts life as:

```cpp
enum class RegionAssistantTextKind { provisional, work, answer, answer_truncated };
```

rendered under a dim, explicitly temporary `ASSISTANT...` heading
(`region_section_heading()`). Two things can end that uncertainty, and both
relabel the same block in place rather than copying or reordering it:

- **A tool call starts.** Any `RegionAssistantTextKind::provisional` text in
  the same message immediately becomes `work` — a tool call proves the
  preceding narration was a preamble, not a final answer (see the two
  `if (text->kind == provisional) text->kind = work;` sites in
  `region_renderer.cpp`, at tool-start and tool-update dispatch).
- **The message ends.** `MessageEndEvent`'s stop reason classifies the block
  atomically:
  - `StopReason::stop` → `answer` (`ANSWER`)
  - `StopReason::length` → `answer_truncated` (`ANSWER | TRUNCATED` — the
    partial output is kept and clearly marked, not discarded)
  - anything else (including `tool_use`, and errored/aborted messages) →
    `work`, using the existing error presentation

This is why a live in-progress frame and the stable completed frame can look
different for the same message without either one lying: the label always
reflects what is actually known at paint time.

## Mailbox reply truthfulness

An `agents_reply` tool *call* is not proof that anything was delivered. The
`REPLY` block is never inferred from a tool name, its arguments, or
Lua-formatted output. Instead, a typed presentation notice flows from the
point where delivery is actually confirmed:

```text
pici.mailbox.reply native binding
  -> only after MailboxCoordinator::reply() returns its receipt
  -> ToolExecutionContext::on_presentation callback
  -> ToolPresentationEvent (AgentEvent) on the thread-safe event queue
  -> dispatch_event()
  -> Renderer::on_mailbox_reply_queued(call_id, MailboxReplyQueuedNotice)
  -> RegionReplyBlock appended to the active turn
```

`MailboxReplyQueuedNotice` carries the originating request message ID, the
tool call ID, the recipient session/agent ID, and the reply text. The
renderer paints `REPLY -> <recipient> queued` — never `sent` or
`delivered`, because the native operation only guarantees the message was
queued, not that a remote peer consumed it. If correlation to an active turn
is missing (for example the notice arrives after `on_turn_end()`), it is
dropped rather than attached to a stale, already-completed turn. On any tool
error or cancellation, no notice is ever emitted, so `WORK` shows the
ordinary failed/collapsed tool region and no `REPLY` block appears at all.

The same callback threads through `LuaHooksImpl::execute_inline_tool()` via a
`LuaRegistryPointerGuard` on `"pici.inline_presentation"`, alongside the
existing actor and stop-token guards, so Lua-defined mailbox tools (the
`agents_reply` bundled in `addons/mailbox.lua`) can reach it through
`pici.mailbox.reply(...)` without any Lua code choosing when the notice
fires — that decision stays entirely on the C++ side, next to the actual
`MailboxCoordinator::reply()` call.

## Core owns structure; Lua owns tool presentation

The core renderer exclusively owns turn boundaries and the `REQUEST`, `WORK`,
`ANSWER`, and `REPLY` labels. Lua's `format_tool_call` / `format_tool_result`
hooks (see `addons/tool_format.lua`) only ever own the *contents* of an
individual tool-call/tool-result region inside `WORK`:

- the last loaded non-`nil` formatter wins;
- formatter output is sanitized so only SGR escape sequences survive — a
  formatter that attempts cursor movement has it stripped;
- a running formatted call contributes at most its first physical header
  line, with live raw output allowed to stream below it;
- a completed formatted result replaces the expanded tool region and stays
  capped by `kMaxToolBodyLines`;
- collapsed old tools (beyond `kMaxExpandedToolRegions`, currently 6) fall
  back to a renderer-owned one-line summary regardless of formatter output.

There is no Lua hook that can replace an entire turn or a structural
heading — that boundary is intentional so a theme can restyle tool output
without being able to erase the WORK/ANSWER/REPLY distinction this model
exists to guarantee.

## Wrapping, scrolling, and diff-paint invariants

- All text (`REQUEST` bodies, tool output, assistant text) wraps through the
  same ANSI-aware physical-width splitter, so escape sequences never throw
  off column counting.
- Every physical row ends in an explicit `\033[0m` reset
  (`with_sgr_reset()`), so diff-painting a single row can never leak a Lua
  color into a neighboring `ANSWER` or `REQUEST` row.
- Frames are coalesced at roughly 60 fps (`kFrameIntervalMs = 16`) and
  painted through `diff_region_rows()`, which only rewrites rows that
  actually changed — no full-screen clear is issued, and completed-turn
  history is preserved rather than reflowed.
- `scroll_offset_rows` is tracked in wrapped physical rows and clamped to
  `frame.max_scroll_rows`; line, page, top (`Home`), and bottom (`End`)
  scrolling are the only ways it changes outside of a resize.
- Parallel tool regions stay fixed at call-order position
  (`RegionToolAddress{turn_index, block_index}` in `tool_addresses`) even
  when updates or completion events arrive out of call order — the two
  out-of-order tools in the example above always paint in call order, never
  completion order.
- Readline cursor restoration, alternate-screen teardown, Ctrl-C, and resize
  behavior are unaffected by any of the above: they operate on the painted
  frame, not on `RegionState`.

## Testing

- `test/test_region_renderer.cpp` is the source of truth for pure
  `build_region_frame()` behavior: turn/request rendering, provisional
  reclassification, tool call-order preservation, the reply block, and
  narrow/tiny terminal sizing.
- `test/test_faux_control_mode.cpp` drives the real event path
  (`AgentSession` → `dispatch_event()`) deterministically, including the
  ordinary out-of-order-tools scenario and the mailbox queued-reply scenario
  described in [faux control renderer testing](faux-control.md).
- Terminal-level verification (tmux snapshots at fixed sizes, ANSI cells,
  cursor, alternate-screen state) is described in the "Tmux snapshot
  verification loop" section of the same document.
