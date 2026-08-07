# pici add-ons

Lua plugins loaded via `--hooks-file`, `--hooks-dir`, or `config.toml [addons]`.

## costline.lua — show last cost in the status line

Displays token cost / usage of the last turn above the REPL prompt.

### Install

```bash
mkdir -p ~/.config/pici/addons
cp costline.lua ~/.config/pici/addons/
```

`~/.config/pici/config.toml`:

```toml
[addons]
files = ["~/.config/pici/addons/costline.lua"]
```

Or per-run:

```bash
pici --hooks-file addons/costline.lua
```

### What it shows

| Situation | Status line |
|-----------|--------|
| First turn (no usage yet) | no status line |
| Model with pricing | `[$0.0042 sess:$0.01]` |
| Model without pricing | `[1.0K tok]` or `[803→123]` |
| With cache hits | `[$0.001 cache:627]` |
| With session total | `[$0.002 sess:$0.015]` |

### How it works

Uses the `status_line(ctx)` hook. `ctx` contains:

```lua
{
  turn = 2,
  model = "muse-spark-1.1",
  tools = 7,
  last = {
    input, output, cache_read, cache_write, total_tokens,
    cost = {input, output, cache_read, cache_write, total}
  },
  session = {
    -- same shape as last, accumulated
  }
}
```

Returns `nil` before first turn → leaves the status line empty. Return a string to render above the prompt.

The separate `tab_title(ctx)` hook controls the terminal tab/window title.
Both hooks receive the same context, including `session_id` and optional
`session_name`.

Terminal title behavior:

- When no hook supplies a title, the idle title is the startup project label
  (Git root basename, or cwd basename, falling back to `pici`).
- During a primary `run_prompt` turn the title is prefixed with a Braille
  spinner (`⠋` … `⠏` every 100 ms), e.g. `⠋ my-project`, and stays active
  through tool calls and follow-up model requests without flickering.
- `nil` leaves the current base title unchanged; `""` sets an empty base
  (idle writes an empty OSC payload, active shows only the spinner frame).
- A non-nil hook value replaces the default label verbatim — pici does not
  prepend `pici` automatically.
- Titles are sanitized (ANSI/OSC controls, bidi and invisible formatting
  stripped, whitespace collapsed, truncated to 240 Unicode scalars) and built
  as a single `ESC ] 0 ; <title> BEL` write.
- Title updates are only emitted when stdout is a TTY; piped output,
  `pi-cli --rpc`, and `pi-acp` never write them, and background task children
  do not drive the title independently.
- On exit pici clears the title it manages with an empty OSC 0 payload rather
  than trying to restore the previous terminal title.

### Testing

```bash
./build/pi-cli --test addons/test_costline.lua
./build/pi-cli --test addons/test_context.lua
```

See the test files for `pici.test` examples. The context suite exercises the
genome, heatmap, tool graph, effective-unavailable behavior, and JSON fallback.
They also run automatically as the `test-addons` CTest target.

### Customising

Edit `costline.lua` — e.g. to show only cost, no session:

```lua
return {
  status_line = function(ctx)
    if not ctx.last or ctx.last.cost.total == 0 then return nil end
    return string.format("[$%.4f]", ctx.last.cost.total)
  end
}
```

Or with ANSI colour:

```lua
return {
  status_line = function(ctx)
    local c = ctx.last.cost.total
    if c == 0 then return nil end
    return string.format("[\27[32m$%.4f\27[0m]", c)
  end
}
```

## turn_guard.lua — one safe diagnostic turn

Blocks `bash`, `edit`, and `write`, then stops after the first assistant/tool
batch. This is useful for inspecting model behavior without allowing an
existing worktree to be modified:

```bash
./build/pi-cli --hooks-file addons/turn_guard.lua
```

The blocked call is returned to the model as an error result, and the
`should_stop_after_turn` hook ends the run before the model can choose another
tool. The completed turn is then persisted to the session JSONL file.

## context.lua — inspect model context

`context.lua` adds static TUI views:

```text
/context                    full JSON snapshot
/context genome             compact message-shape visualization
/context heatmap            content-density visualization
/context tools              tool-call graph and tool inventory
/context effective heatmap same view over the last request-ready context
```

The raw view shows current agent state. Effective views show the most recent
request-ready context after context transformation and message conversion.
Before the first model request, effective views report that no snapshot is
available.

Install it alongside the other add-ons:

```bash
cp context.lua ~/.config/pici/addons/
```

Then add it to `config.toml`:

```toml
[addons]
dir = "~/.config/pici/addons"
```

The dump is returned through the command output channel, so it follows the
active renderer instead of becoming a model prompt. `pici.log` remains useful
for add-on diagnostics. Context output can contain sensitive prompt content,
reasoning/signature data, tool schemas, and large base64 image payloads. The
effective view is provider-neutral context, not the exact wire JSON sent to a
provider.

After editing an add-on, run `/reload-addons` to reload the configured hook
files, callbacks, and registered add-on tools without restarting the session.

## Phase 1 examples

`permissions.lua` demonstrates the intended permission split. The Lua hook
owns the policy, while the core invokes it before every tool execution and
turns a block into a structured `blocked` tool outcome. Hook failures fail
closed. Load it explicitly when you want the sample policy:

```bash
pici --hooks-file addons/permissions.lua
```

`context_trim.lua` demonstrates request-local context preparation. It keeps
the first message and the recent tail when the conservative core estimate is
near the model window. It does not rewrite the durable session transcript;
more advanced summarization can be implemented in another add-on using the
same `prepare_context(ctx)` hook.

The standalone Lua tool and `pici.add_tool()` tools may call
`ctx.update(value)` from their `execute(args, ctx)` function to publish live
partial output. Existing one-argument tools continue to work unchanged.
