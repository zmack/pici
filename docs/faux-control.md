# Faux control renderer testing

Faux control drives a real `pi-cli` agent loop and renderer with scripted model
and tool output. It supports deterministic terminal UI development—especially
concurrent tool regions—without an API key, network request, model, shell
command, or other real tool execution.

The controller sends newline-delimited JSON over a Unix domain socket. `pi-cli`
sends command responses and the real structured `AgentEvent` stream back over
the same connection while painting the selected renderer on its terminal.

## Start the server

Build the CLI, then run it in a terminal with a fixed size:

```sh
cmake --build build --target pi-cli --parallel
./build/pi-cli \
  --config /dev/null \
  --session-dir /tmp/pici-faux-session \
  --render region \
  --faux-control /tmp/pici-faux.sock
```

`--config /dev/null` and a temporary session directory are recommended for
repeatable renderer tests. Faux-control mode selects a synthetic
`faux-control` model, skips credential resolution, and does not load real,
Lua, or hook-registered tools. Tool names referenced by the script are
registered on demand as `ScriptedTool` instances.

The selected renderer is the real CLI renderer. Events pass through the same
`VerboseRenderer` and `dispatch_event` path used by an interactive turn; the
socket protocol is not a renderer simulation.

## Protocol model

A **round** is one scripted model response. A **turn** is one complete agent
run and may consume multiple rounds:

1. A `tool_calls` round produces assistant content and tool calls.
2. The real agent loop dispatches all eligible tools, including parallel calls.
3. Real tool start, update, and end events reach the renderer and socket client.
4. The agent loop requests the next queued round.
5. An `end_turn` round supplies final assistant content and ends the run.

Queue every known round before sending `turn` for the simplest deterministic
test. It is also valid to send a later round while a turn is waiting for it.

Every command may include an `id`. Its immediate acknowledgement looks like:

```json
{"type":"response","command":"round","id":"round-1","success":true}
```

Invalid commands receive `success:false` and an `error` string. Agent events
have `type:"event"`, an `event` name, a monotonic `sequence`, and event-specific
`data`. A run ends with `{"type":"turn.completed"}` or
`{"type":"turn.failed","error":"..."}`.

## Commands

### `round`

```json
{
  "type": "round",
  "id": "tools",
  "delay_between_ms": 5,
  "stop_reason": "tool_calls",
  "content": [
    {"type": "text", "text": "Checking both jobs."},
    {"type": "thinking", "text": "They can run concurrently."},
    {
      "type": "tool_call",
      "call_id": "slow-call",
      "name": "slow-tool",
      "args": {"job": "slow"},
      "updates": [
        {"after_ms": 20, "partial": "slow: started"},
        {"after_ms": 180, "partial": "slow: almost done"}
      ],
      "result": {"content": "slow result", "is_error": false},
      "finish_after_ms": 240
    },
    {
      "type": "tool_call",
      "call_id": "fast-call",
      "name": "fast-tool",
      "args": {"job": "fast"},
      "updates": [
        {"after_ms": 25, "partial": "fast: started"},
        {"after_ms": 70, "partial": "fast: done"}
      ],
      "result": {"content": "fast result", "is_error": false},
      "finish_after_ms": 90
    }
  ]
}
```

Required round fields are `type`, `stop_reason`, and the `content` array.
`stop_reason` must be `tool_calls` or `end_turn`.

Content entries are processed in array order:

- `text` requires a string `text`.
- `thinking` requires a string `text`.
- `tool_call` requires non-empty string `call_id` and `name` fields.
  `args`, when present, must be a JSON object.

Tool update times are non-negative milliseconds measured from that tool's
start, not from the preceding update. Updates must be sorted by `after_ms`.
`finish_after_ms` is also measured from tool start. Omitted arguments, updates,
result content, error state, and finish delay default to an empty object, empty
list, empty string, `false`, and zero respectively.

`delay_between_ms` paces synthesized assistant streaming events and is
cancellable; it does not control tool timing. Use unique call IDs. Behaviors
are consumed by call ID exactly once, so an accidental replay returns an error
tool result instead of blocking.

### `turn`

```json
{"type":"turn","id":"turn-1"}
```

`turn` starts `AgentSession::run_prompt` asynchronously. A second `turn` while
one is active is rejected. The empty synthetic user prompt is an implementation
detail and is not painted as assistant output.

### `quit`

```json
{"type":"quit","id":"quit"}
```

`quit` acknowledges the command, interrupts active work, exits `pi-cli`, and
removes the socket path.

## Complete Python driver

This example queues two tools that update and finish out of order, queues final
assistant text, starts the turn, and records the structured event stream.

```python
#!/usr/bin/env python3
import json
import socket

rounds = [
    {
        "type": "round",
        "id": "tools",
        "stop_reason": "tool_calls",
        "content": [
            {"type": "text", "text": "Opening text"},
            {
                "type": "tool_call",
                "call_id": "slow-call",
                "name": "slow-tool",
                "args": {"value": 1},
                "updates": [
                    {"after_ms": 10, "partial": "slow-start"},
                    {"after_ms": 90, "partial": "slow-late"},
                ],
                "result": {"content": "slow-result", "is_error": False},
                "finish_after_ms": 160,
            },
            {
                "type": "tool_call",
                "call_id": "fast-call",
                "name": "fast-tool",
                "args": {"value": 2},
                "updates": [
                    {"after_ms": 12, "partial": "fast-start"},
                    {"after_ms": 30, "partial": "fast-late"},
                ],
                "result": {"content": "fast-result", "is_error": False},
                "finish_after_ms": 50,
            },
        ],
    },
    {
        "type": "round",
        "id": "closing",
        "stop_reason": "end_turn",
        "content": [{"type": "text", "text": "Closing text"}],
    },
]

client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
client.connect("/tmp/pici-faux.sock")
stream = client.makefile("rwb", buffering=0)

for command in [*rounds, {"type": "turn", "id": "turn-1"}]:
    stream.write(json.dumps(command, separators=(",", ":")).encode() + b"\n")

with open("/tmp/pici-faux-events.jsonl", "wb") as events:
    while True:
        line = stream.readline()
        if not line:
            raise RuntimeError("faux-control disconnected before completion")
        events.write(line)
        value = json.loads(line)
        if value.get("type") == "turn.failed":
            raise RuntimeError(value.get("error", "turn failed"))
        if value.get("type") == "turn.completed":
            break

stream.write(b'{"type":"quit","id":"quit"}\n')
stream.readline()
client.close()
```

Partial tool updates are emitted as soon as their worker reports them, so the
event trace demonstrates real concurrency. Final tool-result events may be
committed in call order by the agent loop even when the underlying fast tool
returned first. The renderer must keep each region at its call-order position
through both kinds of ordering.

## Tmux snapshot verification loop

Use a fresh tmux server and fixed dimensions for each golden run:

```sh
tmux -L pici-render-test -f /dev/null new-session -d \
  -x 100 -y 30 -s verify \
  './build/pi-cli --config /dev/null \
    --session-dir /tmp/pici-faux-session \
    --render region \
    --faux-control /tmp/pici-faux.sock'
```

Run the Python driver from another terminal, but delay its `quit` command until
after capturing the alternate screen. With the `tmux-snapshot` Codex skill:

```sh
SNAPSHOT="${CODEX_HOME:-$HOME/.codex}/skills/tmux-snapshot/scripts/tmux_snapshot.py"
python3 "$SNAPSHOT" capture \
  --socket-name pici-render-test \
  --target verify:0.0 \
  --wait-stable 2 \
  --output /tmp/region-golden.json
```

Restart the tmux server, replay the script, and assert the live pane:

```sh
SNAPSHOT="${CODEX_HOME:-$HOME/.codex}/skills/tmux-snapshot/scripts/tmux_snapshot.py"
python3 "$SNAPSHOT" assert \
  --socket-name pici-render-test \
  --target verify:0.0 \
  --wait-stable 2 \
  /tmp/region-golden.json
```

Without that helper, inspect the visible pane directly:

```sh
tmux -L pici-render-test capture-pane -p -e -N -t verify:0.0
tmux -L pici-render-test display-message -p -t verify:0.0 \
  '#{pane_width}x#{pane_height} cursor=#{cursor_x},#{cursor_y} alt=#{alternate_on}'
```

Review goldens rather than blindly updating them. Pin the tmux version, pane
dimensions, renderer/configuration, locale, scripted content/timing, cursor,
alternate-screen state, plain cells, and ANSI cells.

Tmux validates terminal cell semantics, escape attributes, geometry, and cursor
state. It does not validate font rasterization or other emulator-specific
pixels.

## Connection and failure behavior

- One client connection is served at a time.
- Empty JSONL lines are ignored; malformed JSON receives an inline error.
- An idle disconnect preserves the remote client for a later connection.
- A disconnect during an active turn cancels the agent and closes the remote
  client. Reconnecting that interrupted turn is intentionally unsupported.
- Tool waits and assistant-event pacing observe cancellation promptly.
- A stale Unix socket is replaced at startup. An existing non-socket filesystem
  entry is never unlinked and causes startup to fail.
- Socket writes handle interruption, short writes, and closed peers without
  raising `SIGPIPE`.

## Implementation map

- `src/core/providers/faux_control.{h,cpp}`: round compilation, queued model
  replay, scripted tools, and behavior storage.
- `src/cli/faux_control_mode.{h,cpp}`: commands, turn lifecycle, event output,
  and the Unix socket server.
- `src/main.cpp`: synthetic model/tool wiring and renderer dispatch.
- `test/test_faux_control.cpp`: compilation, timing, results, and cancellation.
- `test/test_faux_control_mode.cpp`: lifecycle, concurrency, framing,
  reconnects, peer drops, and filesystem safety.
