# Region renderer turn structure, mailbox provenance, and Lua composition

> **Implementation handoff for Luna:** execute this plan one milestone at a
> time. Do not start the next milestone until the current milestone builds,
> passes its focused tests, passes formatting/lint checks for the touched code,
> has been visually inspected where required, and is committed. After each
> milestone, report the commit hash, tests run, and snapshot evidence before
> continuing. Preserve unrelated worktree changes, especially untracked plans.

## Goal

Make the region renderer answer three questions unambiguously for every turn:

1. What request caused this turn?
2. Which output is intermediate work (thinking, narration, and tools)?
3. Which output is the completed answer or an actual mailbox reply?

The target presentation is:

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

For an inbound mailbox request:

```text
-- REQUEST | MAILBOX | /root/luna ----------------------------
Implement milestone 2 and verify it in tmux.

WORK
Inspecting the renderer...
  [build] done
  [snapshot] done

REPLY -> /root/luna                                      queued
Milestone 2 is implemented and verified.

ANSWER
The mailbox reply was queued successfully.
```

Use ASCII in golden content unless an existing renderer convention requires a
Unicode glyph. The production UI may use Unicode separators after width and
fallback behavior are tested.

## Why this requires semantic changes

The current region model is a persistent flat `vector<RegionBlock>` containing
only `RegionTextBlock` and `RegionToolBlock`. `on_turn_start()` resets transient
state but does not append a turn boundary. User `MessageStartEvent`s are ignored
by `dispatch_event()`, and `Renderer::on_message_end()` receives only usage, not
the assistant message's stop reason. Consequently:

- the initiating user or mailbox request is absent from the rendered history;
- intermediate assistant narration and the terminal assistant response are the
  same `RegionTextBlock` type;
- mailbox provenance is lost before renderer dispatch;
- the renderer cannot know whether a mailbox reply was actually queued;
- visual styling alone cannot recover the missing semantics.

Do not solve this by parsing the synthetic `[pici mailbox message]` text, by
matching `agents_reply` by tool name inside the renderer, or by guessing from
text content. Preserve typed metadata through the event path.

## Existing integration points to read before editing

- `src/core/agent_loop.h`: `AgentMessageSource` and `AgentMessageEnvelope`.
- `src/core/agent_loop.cpp`: prompt publication near the existing
  `MessageStartEvent(prompt.message)` call; multiple prompt envelopes may be
  batched into one turn.
- `src/core/event_types.h`: `MessageStartEvent`, `MessageEndEvent`, and
  `AgentEvent`.
- `src/core/stream_renderer.h` and `.cpp`: renderer interface and
  `dispatch_event()`.
- `src/core/region_renderer.h` and `.cpp`: pure frame model, wrapping, tool
  expansion, paint loop, status row, and persistent history.
- `src/core/session/agent_session.cpp`: `run_prompt()` wraps an ordinary
  `AgentMessageEnvelope`; `run_messages()` preserves envelopes.
- `src/core/mailbox/mailbox_coordinator.cpp`:
  `mailbox_message_to_message()` currently flattens mailbox metadata into model
  text, and both delivery paths construct mailbox envelopes.
- `src/core/mailbox/mailbox_bindings.cpp`: native `reply` knows the original
  message ID, reply text, recipient IDs, and the returned queued receipt.
- `src/core/message_types.h`: `ToolExecutionContext` and `ToolUpdateCallback`.
- `src/core/agent_loop.cpp`: `execute_tool_safely()` is the existing pattern
  for turning a tool callback into an `AgentEvent`.
- `src/core/lua_tool.cpp`: `LuaRegistryPointerGuard` already places actor and
  stop-token pointers in the registry for native mailbox calls.
- `src/main.cpp`: `VerboseRenderer` invokes `format_tool_call` and
  `format_tool_result`, sanitizes output, and routes it through
  `on_tool_output_text()` when the renderer owns tool output.
- `src/cli/faux_control_mode.cpp` and `docs/faux-control.md`: deterministic real
  agent-loop/renderer harness. It currently calls `run_prompt("")`.
- `addons/tool_format.lua`: authoritative current formatter behavior.

## Frozen design decisions

### Use semantic labels, not speaker assumptions

Use `REQUEST`, not `YOU`. An ordinary local prompt needs no redundant source
label. Non-local input adds provenance, for example:

```text
REQUEST | MAILBOX | /root/luna
REQUEST | FOLLOW-UP | /root
```

The exact display-name priority is:

1. task path when available;
2. session name when available;
3. a shortened agent ID;
4. `unknown sender` only when the source is known to be mailbox but identity is
   missing.

Keep full message IDs, agent IDs, and session IDs in state for diagnostics, but
do not print them in the normal frame unless they are the only identity.

### Core owns structure; Lua owns tool presentation

The core renderer exclusively owns turn boundaries and the `REQUEST`, `WORK`,
`ANSWER`, and `REPLY` labels. Existing Lua hooks continue to own only the
contents of individual tool-call and tool-result regions inside `WORK`.

Do not add a Lua hook that can replace an entire turn or structural header in
this work. Such a hook could erase the distinction this project is adding.
Structural colors may be made configurable later through a constrained palette
API, but that is out of scope.

Preserve current Lua formatter behavior:

- last loaded non-`nil` formatter wins;
- output is sanitized and only SGR escape sequences survive;
- a running formatted call contributes at most its first physical header line;
- live raw output may appear below that header;
- a completed formatted result replaces the expanded tool region and remains
  capped by the renderer's tool-body limit;
- collapsed old tools use renderer-owned summaries.

### Be honest while an assistant message is streaming

The stop reason is unknown while early text deltas arrive. Never guess that
streaming text is final. Render the active assistant message under a temporary
`ASSISTANT...` label. At message end, classify the same block atomically:

- `StopReason::tool_use` becomes `WORK`;
- `StopReason::stop` becomes `ANSWER`;
- `StopReason::length` becomes `ANSWER | TRUNCATED` so partial useful output is
  retained without implying normal completion;
- errored or aborted message text remains under `WORK`, with existing error
  presentation retained.

If a tool-call event arrives before message end, the current provisional block
may be classified as `WORK` immediately. Classification must relabel existing
content rather than copy or reorder it. Tool/thinking blocks are always work.

This avoids both buffering away streaming output and falsely presenting a tool
preamble as a final answer.

### Mailbox reply status must come from the native operation

An `agents_reply` tool call is not proof of delivery. The native reply binding
returns a queued receipt. Emit a typed presentation event only after that
operation succeeds, carrying at least:

- originating request/message ID;
- tool call ID;
- recipient agent/session ID;
- reply text;
- delivery state (`queued` initially).

The renderer must not derive a reply from tool arguments or formatted Lua text.
On failure, keep the tool error in `WORK`; do not create a successful `REPLY`
section.

The mailbox operation currently guarantees `queued`, not remote consumption.
Render `queued`, not `sent` or `delivered`.

### Preserve behavior of non-region renderers

New `Renderer` callbacks must have default no-op implementations. Raw,
markdown, viewport, ACP, and tests using minimal renderers must retain their
current text behavior. Do not make all renderers print prompts as part of this
work; the user-visible change is scoped to `--render region`.

## Target state model

Prefer an explicit turn model over inserting decorative strings into the flat
transcript:

```cpp
enum class RegionAssistantTextKind { provisional, work, answer };

struct RegionRequestBlock {
  RequestPresentation metadata;
  std::string raw_text;
  std::size_t non_text_attachments{0};
};

struct RegionTextBlock {
  std::string raw;
  RegionAssistantTextKind kind{RegionAssistantTextKind::provisional};
  std::uint64_t message_sequence{0};
};

struct RegionReplyBlock {
  std::string request_message_id;
  std::string call_id;
  std::string recipient_label;
  std::string raw_text;
  MailboxReplyState state{MailboxReplyState::queued};
};

using RegionTurnBlock =
    std::variant<RegionTextBlock, RegionThinkingBlock, RegionToolBlock,
                 RegionReplyBlock>;

struct RegionTurn {
  std::vector<RegionRequestBlock> requests;
  std::vector<RegionTurnBlock> blocks;
  bool complete{false};
};
```

Names may be adjusted to match local style, but retain the explicit hierarchy.
`RegionState` should own persistent turns and an active-turn index. Active tool
lookup must identify both turn and block (or be scoped to the active turn) so
parallel updates still modify their original positions.

Multiple prompt envelopes in one agent turn must remain visibly ordered within
one `REQUEST` section. Do not silently keep only the last prompt.

For non-text user content, render a stable placeholder such as
`[image attachment]`; do not dump binary/base64 content into the terminal.

## Milestone 1: typed request provenance from envelope to renderer

**Outcome:** the renderer can receive the initiating request and its origin,
without changing the region layout yet.

1. Replace or augment the two-value `AgentMessageSource` with a presentation
   structure carried by `AgentMessageEnvelope`. Keep ordinary construction
   terse with safe defaults. Suggested fields:

   ```cpp
   enum class RequestSource { ordinary, mailbox, follow_up };

   struct RequestPresentation {
     RequestSource source{RequestSource::ordinary};
     std::optional<std::string> message_id;
     std::optional<std::string> message_kind;
     std::optional<std::string> sender_agent_id;
     std::optional<std::string> sender_session_id;
     std::optional<std::string> sender_task_path;
     std::optional<std::string> sender_session_name;
   };
   ```

   `follow_up` is useful for subagent task continuation, but do not fabricate it
   where the task manager lacks reliable metadata; ordinary is preferable to a
   wrong label.

2. Populate mailbox fields directly from `MailboxMessage` at both envelope
   construction sites in `mailbox_coordinator.cpp`. Keep the existing synthetic
   model-facing mailbox text for compatibility in this milestone. Renderer
   presentation metadata must not alter persisted/model-visible content.

3. Add optional request metadata to `MessageStartEvent`, pass it when publishing
   prompt envelopes in `agent_loop.cpp`, and include it in `event_to_json()` so
   faux-control traces remain diagnosable.

4. Add a default no-op renderer callback such as
   `on_request(const RendererRequest &request)`. `dispatch_event()` should call
   it only for `UserMessage` start events. Extract text safely from content
   blocks and count non-text attachments.

5. Add event/dispatch tests for ordinary, mailbox, multiple batched prompts,
   absent optional metadata, and non-text content.

**Focused verification:** build and run the event/agent/stream-renderer tests
that cover changed files, plus mailbox coordinator tests. Confirm existing raw
and markdown renderer output does not gain echoed prompts.

**Commit:** `Expose typed request provenance to renderers`

## Milestone 2: explicit turns and visible request sections

**Outcome:** each region-renderer turn begins with a persistent request that
remains associated with all later work and answers.

1. Refactor the pure `RegionState`/`build_region_frame()` model to explicit
   turns. Keep frame construction pure and terminal writes outside it.
2. `on_turn_start()` creates one active turn. `on_request()` appends request
   blocks to it. If a legacy/test caller starts streaming without a request,
   render the turn without a request section rather than crashing.
3. Render one `REQUEST` heading per turn, with optional source/sender suffix.
   Wrap request text with the same ANSI-aware physical-width logic as other
   content. Headers must remain readable without color.
4. Preserve completed turns, scrolling in physical rows, resize behavior,
   diff-paint behavior, status ownership, tool lookup, and the six-expanded-tool
   cap.
5. Add pure frame tests for ordinary and mailbox headings, multiple prompt
   envelopes, long wrapping, narrow widths, attachment placeholders, completed
   history, and scroll boundaries.

**Faux-control prerequisite:** extend the `turn` command with an optional
prompt object while retaining the old empty-prompt default:

```json
{
  "type": "turn",
  "id": "turn-1",
  "prompt": {
    "text": "Why are parallel tools reordered?",
    "source": "ordinary"
  }
}
```

Allow a deterministic mailbox form with explicit test metadata. Build an
`AgentMessageEnvelope` and call `AgentSession::run_messages()` instead of
`run_prompt("")` when it is supplied. This exercises the real event path.
Document and validate every field; malformed source metadata must return a
protocol error rather than being silently ignored.

**Snapshot gate:** at fixed `100x30`, capture ordinary and mailbox request
headers before accepting any golden. Inspect plain cells, ANSI cells, cursor,
geometry, alternate-screen state, and narrow-width wrapping.

**Commit:** `Render persistent request sections by turn`

## Milestone 3: classify assistant work and final answers

**Outcome:** intermediate assistant narration, thinking, and tools are visibly
separate from the terminal answer.

1. Give renderer message-end dispatch enough information to classify a message.
   Prefer a small presentation value containing stop reason and usage over
   passing a mutable/full model object. Update `VerboseRenderer` diagnostics and
   usage handling without changing existing usage output.
2. Open a provisional assistant text block on its first delta. Keep subsequent
   deltas for that assistant message in that block even when physical frames
   repaint.
3. Render provisional text under `ASSISTANT...`. At message end, relabel the
   block to `WORK` or `ANSWER` using the frozen rules above. A tool start marks
   preceding provisional text in the same message as work immediately.
4. Group thinking, intermediate text, and tool regions under one `WORK` heading
   per contiguous work section; do not repeat `WORK` before every tool.
5. Ensure the final answer remains after all earlier work in logical event
   order. Parallel tool completion must never move blocks.
6. Keep markdown rendering for assistant prose. Structural headings must not be
   interpreted as model markdown.

**Focused tests:** pure-frame and callback tests for:

- text-only final answer;
- text preamble -> parallel tools -> final answer;
- multiple tool rounds;
- thinking with and without visible assistant text;
- provisional repaint followed by atomic reclassification;
- length-truncated, error, and abort endings;
- empty terminal answer;
- narrow and tiny terminal heights.

**Snapshot gate:** use faux-control to capture both a live in-progress frame and
the stable completed frame. Verify that the live label is honest, work remains
visually subordinate, the final answer is high contrast, and cursor/status rows
remain correct.

**Commit:** `Separate assistant work from final answers`

## Milestone 4: preserve Lua formatter composition inside WORK

**Outcome:** all existing Lua tool themes continue to work, but none can replace
or obscure semantic turn structure.

1. Keep `VerboseRenderer` as the only caller of `format_tool_call` and
   `format_tool_result`; do not invoke Lua from the paint thread.
2. Route sanitized custom output to the correct `RegionToolBlock` exactly as
   today. Refactoring the tool address for explicit turns must not break
   call-ID correlation or parallel updates.
3. Apply the existing first-line and completed-result caps inside the tool
   region, after reserving any width used by structural indentation. Compute
   wrapping from visible columns, not bytes.
4. Reset SGR state on every physical row so diff-painting one row cannot leak a
   Lua color into `ANSWER` or `REQUEST`.
5. Add tests using custom multiline/SGR formatter output, nil fallback, Lua
   error fallback, parallel calls, collapsed old tools, and a formatter that
   attempts cursor movement (which sanitization must remove).
6. Run representative bundled formatter tests, including
   `addons/test_tool_format.lua`, `addons/test_minimal_dot.lua`, and
   `addons/test_nfo_format.lua`.

**Snapshot gate:** capture the same scripted turn once with built-in tool
formatting and once with a Lua theme. Only tool rows should change; request,
work, answer, status, cursor, and geometry must remain structurally identical.

**Commit:** `Compose Lua tool themes with semantic turn layout`

## Milestone 5: typed mailbox reply presentation

**Outcome:** a successful native mailbox reply produces a truthful persistent
`REPLY -> recipient` block; failed attempts do not.

Use the existing tool-update callback path as the pattern, but do not encode the
notice as display text. Introduce a typed, domain-neutral presentation callback
on `ToolExecutionContext`, whose initial variant is a mailbox-reply-queued
notice. The expected route is:

```text
pici.mailbox.reply native binding
  -> ToolExecutionContext presentation callback
  -> typed AgentEvent carrying call_id + reply receipt
  -> dispatch_event()
  -> Renderer::on_mailbox_reply_queued(...)
  -> active RegionTurn reply block
```

Implementation details:

1. Add the callback next to `ToolUpdateCallback` in `message_types.h` and install
   it in `execute_tool_safely()` next to `on_update`.
2. Carry its pointer through `LuaRegistryPointerGuard`, alongside
   `pici.inline_actor` and `pici.inline_stop_token`; clear it through the same
   RAII scope. Never retain the pointer after tool execution.
3. Extend the native mailbox binding callback context so `reply` can emit only
   after `MailboxCoordinator::reply()` returns its queued receipt. Include the
   original reply text and the authoritative recipient IDs.
4. Convert the callback to an `AgentEvent` using the same thread-safe event
   queue path as tool updates. Do not call the renderer from the tool worker.
5. Link the notice to the active request by message ID and to its tool by call
   ID. If correlation is absent, render a safe recipient-based reply block and
   retain diagnostics; never crash or attach it to an older completed turn.
6. Display the receipt state as `queued`. Do not claim remote acknowledgement.
7. Keep the ordinary formatted `agents_reply` tool region in `WORK`; it may
   collapse normally. The semantic reply block is renderer-owned and cannot be
   replaced by `format_tool_result`.

Add native mailbox-binding, Lua registry-scope, agent-event serialization,
dispatch, and region-frame tests for success, error, cancellation, missing
recipient agent ID, multiple replies, and parallel unrelated tools.

Extend faux-control scripted tools with optional typed presentation notices so
the full path can be tested without a real mailbox database. Do not add a
renderer-only command that bypasses the agent event stream.

**Snapshot gate:** capture a mailbox request with a queued reply and one with a
failed reply. Only the successful case may contain `REPLY`; the state must read
`queued`.

**Commit:** `Render authoritative mailbox reply receipts`

## Milestone 6: regression suite, documentation, and final polish

**Outcome:** the new structure is documented, reproducible, and protected by
unit and terminal-level verification.

1. Add a small deterministic driver under `test/` or `scripts/` that queues:
   ordinary request -> work text -> two out-of-order tools -> final answer, and
   mailbox request -> tool work -> queued reply -> final acknowledgement.
2. Store reviewed fixed-size goldens under a clearly named fixture directory
   only if they are stable across two fresh isolated tmux servers. Otherwise,
   document `/tmp` comparison commands and keep assertions in pure frame tests.
3. Update `docs/faux-control.md` with the prompt/provenance and presentation
   notice schema plus complete examples.
4. Add a renderer architecture section explaining the semantic turn model,
   provisional classification, mailbox receipt truthfulness, Lua ownership
   boundary, wrapping, scrolling, and diff-paint invariants.
5. Update `README.md` only with a short link to the detailed documentation.
6. Test at least `60x16`, `80x24`, and `100x30`; include long prompts, ANSI Lua
   formatting, multiline markdown, parallel tools, scrollback, resize, and
   cursor restoration after turn completion.
7. Verify no request or reply metadata leaks into model history beyond the
   existing intentional mailbox envelope text, and no binary attachment data is
   painted.

Use the global tmux snapshot helper at:

```text
/home/zmack/.codex/skills/tmux-snapshot/scripts/tmux_snapshot.py
```

Run `capture --wait-stable 2` only after deterministic output stabilizes. Review
`capture.plain`, ANSI cells/hash, pane size, cursor coordinates,
alternate-screen state, and pane mode before accepting a golden. On later runs,
use `assert`; exit code 1 is a renderer mismatch and exit code 2 is a harness or
tmux error. Never overwrite a golden merely to make an assertion pass.

**Final required gates:**

```sh
cmake --build build --target test-region-renderer test-faux_control test-faux_control_mode --parallel
ctest --test-dir build --output-on-failure
make format
make format-check
make lint
make test
```

If `make format` changes files, inspect the diff and rerun the focused tests and
`make format-check` before committing. Run the ThreadSanitizer target when the
new presentation callback or paint-thread state changes are complete; report an
environmental TSan failure separately from a real race, but do not silently skip
it.

**Commit:** `Document and verify semantic region turns`

## Cross-milestone invariants

- One milestone per commit; no partial next-milestone work in the same commit.
- Every commit builds and passes its focused tests before it is created.
- Run formatting and linting for all touched C++ at every milestone, not only at
  the end.
- Never call the renderer directly from tool worker, mailbox maintenance, or
  Lua execution threads. All UI mutation flows through the serialized event
  consumer and the renderer's existing paint-thread synchronization.
- Never parse model-visible synthetic mailbox text for presentation metadata.
- Never infer successful delivery from a tool name, arguments, or Lua-formatted
  string.
- Keep request/reply semantic text separate from structural decoration so
  wrapping, accessibility, and future palette changes remain testable.
- Every physical row ends in a safe SGR reset.
- No full-screen clear is introduced; preserve row diffing and completed turn
  history.
- Parallel tool regions remain fixed at call order even when updates and
  completion arrive out of order.
- Readline cursor restoration, alternate-screen teardown, Ctrl-C, scrolling,
  and resize behavior must remain intact.

## Definition of done

The work is complete only when:

- an ordinary request remains visibly attached to its answer in history;
- a mailbox request shows reliable provenance without printing raw envelope
  boilerplate as its header;
- intermediate narration, thinking, and tools are grouped under `WORK`;
- the terminal assistant response is visibly labeled `ANSWER`;
- streamed text is provisional until its stop reason makes classification
  truthful;
- a `REPLY` block appears only after the native mailbox operation returns a
  queued receipt;
- Lua tool themes change tool presentation without changing semantic layout;
- narrow terminals, wrapping, scrolling, resize, cursor, and parallel ordering
  pass pure tests and reviewed tmux snapshots;
- all focused and full tests, format checks, and lints pass on every committed
  milestone;
- documentation explains both the user-visible format and the event/data flow.
