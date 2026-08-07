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
./build/pi-cli --test addons/test_tool_format.lua
./build/pi-cli --test addons/test_nfo_format.lua
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

## tool_format.lua — customize tool-call display (documented example)

The default CLI prints two human-readable blocks per tool via `VerboseRenderer` (`src/main.cpp`):

```text
[tool: name(<raw JSON args>)]       ← at call start
  [name] -> <truncated result>       ← when result is ready (5-line truncation)
```

`tool_format.lua` is a **minimal, heavily-commented example plugin** that shows how to override both blocks. It themes only `bash` and leaves every other tool on the default formatting — copy it and tweak the `if ctx.tool_name == "bash"` branches, or delete one of the two hooks (either is optional). The comments in the file itself are the full implementation guide.

### Install

```bash
pici --hooks-file addons/tool_format.lua
# or dir mode:
mkdir -p ~/.config/pici/addons && cp addons/tool_format.lua ~/.config/pici/addons/
# ~/.config/pici/config.toml: [addons] files = ["~/.config/pici/addons/tool_format.lua"]
```

Smoke test: run a session mixing `bash` and `read` — only `bash` should show the custom block, `read` keeps the stock look. Then edit the file and run `/reload-addons` — the next tool call picks up the change with no restart.

### Hook signatures (authoritative: `src/core/lua_tool.h`)

```lua
format_tool_call(ctx) → string | nil
  ctx = { tool_name: string, call_id: string, args: table }
  -- args is the *decoded* Lua table (ctx.args.command for bash,
  -- ctx.args.file_path for read, …), not a JSON string. Same shape as
  -- before_tool_call. return the entire block to print (include a leading
  -- "\n" to match the default layout). return nil for default. Lua error →
  -- silent fallback, tool still runs. Last non-nil wins if multiple add-ons define it.

format_tool_result(ctx) → string | nil
  ctx = { tool_name: string, call_id: string, args: table,
          content: string, is_error: boolean }
  -- content is the *full untruncated* result; nil → built-in gray
  -- "  [name] -> …" with 5-line truncation (first 2 + "… +N lines omitted" + last 2).
  -- args here comes from a call_id → args cache populated at on_tool_start
  -- (correct for parallel interleaved calls; missing → {}); you never need to
  -- re-parse JSON. Same composition & error semantics as format_tool_call.
```

### Sanitization & truncation

* **Sanitization:** `sanitize_tool_output()` (`src/core/terminal.cpp`) runs on your returned string before it reaches `std::cout` / the viewport. Invalid UTF-8 bytes are stripped; every ANSI/VT escape *except* SGR color (`CSI … m`) is dropped — so `"\27[31m"` / `"\27[0m"` survive, but `"\27[2J"`, `"\27[H"`, `"\27[?1049h"`, OSC, etc. are removed. You can colorize freely; you cannot clear the screen or move the cursor. Do not sanitize yourself.
* **Truncation helper:** `pici.truncate_tool_result(content)` returns the built-in display string (with `" -> "` / `"    "` prefixes). Use it when you only want different colors:

  ```lua
  format_tool_result = function(ctx)
    return "\27[32m" .. pici.truncate_tool_result(ctx.content) .. "\27[0m"
  end
  ```

  Also exposed in `pici --test` files for unit tests.

### Behavior notes

* Pure display — cannot block or rewrite the call (`before_tool_call` / `after_tool_call` do that) and never sees `turn` (no `AgentContext` at that site).
* May stall streaming while it runs (holds the shared Lua `mutex_`); keep it fast.
* `Renderer::on_tool_end` carries no args — `VerboseRenderer` caches them by `call_id`; malformed JSON → empty table, no throw.
* Child/subagent calls never reach `VerboseRenderer` (`AgentTaskManager` nulls all hooks); nothing to wire there.
* Other renderers (`RawStream`, `DiffMarkdown`, `Viewport` alt-screen, ACP/Sync wire formats) do not print these blocks.

### Example (from `tool_format.lua`)

```lua
return {
  format_tool_call = function(ctx)
    if ctx.tool_name ~= "bash" then return nil end
    local cmd = (ctx.args.command or ""):gsub("%s+", " ")
    return string.format("\n\27[36m[bash]\27[0m \27[33m%s\27[0m\n", cmd:sub(1, 88))
  end,
  format_tool_result = function(ctx)
    if ctx.tool_name ~= "bash" then return nil end
    local body = pici.truncate_tool_result(ctx.content)
    return (ctx.is_error and "\27[31m" or "\27[2m") .. body .. "\27[0m"
  end,
}
```

See `addons/test_tool_format.lua` for `pici.test` examples (decoded `args` indexing, full vs truncated content, `nil` fallback, SGR / error tinting) — also runs via `ctest -R test-addons`.

## nfo_format.lua — compact warez-nfo / hacker theme (ready to use)

Unlike `tool_format.lua` (the bash-only documented example above), this is an
opinionated theme that themes **every** tool, not just `bash`, and keeps both
blocks to a single line each:

```text
[bash] ls -la /var/log
  » total 48 (+9 more) [ok]
```

* **Call line:** `[tool_name]` tag, then a best-effort one-line summary of
  the args table — tries `command`, `file_path`, `path`, `pattern`, `url`,
  `prompt`, `query` in that order (covers `bash`/`read`/`write`/`edit`/
  `glob`/`grep`/etc. without special-casing each tool), then falls back to
  the first scalar field as `key=value` so an unrecognized tool still shows
  *something* instead of a bare tag.
* **Result line:** first non-blank line of the *full* result content, colored
  dim (or red on error), with a `(+N more)` suffix if there were additional
  lines and a trailing `[ok]` / `[err]` badge. It does its own one-line
  summarization rather than calling `pici.truncate_tool_result()`, since that
  helper's built-in truncation still spans up to 5 lines.

### Install

```bash
pici --hooks-file addons/nfo_format.lua
# or dir mode:
mkdir -p ~/.config/pici/addons && cp addons/nfo_format.lua ~/.config/pici/addons/
```

`~/.config/pici/config.toml`:

```toml
[addons]
files = ["~/.config/pici/addons/nfo_format.lua"]
```

If you also load `tool_format.lua`, remember composition is "last non-nil
wins" per hook — list `nfo_format.lua` last if you want it to take
precedence, since `tool_format.lua`'s hooks return non-nil for `bash` too.

Smoke test: run a session mixing a few different tools (`bash`, `read`,
`grep`) — every one should show the themed one-line call/result, not just
`bash`. Edit the file and run `/reload-addons` to pick up changes without
restarting.

See `addons/test_nfo_format.lua` for `pici.test` coverage (per-tool arg-field
selection, unknown-tool fallback, single-line truncation with line counting,
empty content, error tinting) — also runs via `ctest -R test-addons`.

## Compact tool-call themes (10 variants)

Ten more single-purpose themes, each in its own file, following the same
structure as `nfo_format.lua`: every tool (not just `bash`) gets a themed
one-line call + one-line result. They differ only in the tag characters,
symbols, and colors used — pick one, or use it as a starting point for your
own.

| File | Look |
|------|------|
| `nfo_classic.lua` | `╣bash╠ ls -la` / `» total 48 (+9 more) [ok]` |
| `scene_release.lua` | `[PICI] bash· ls -la` / `» total 48, +9 lines` |
| `matrix_drop.lua` | `ﾊﾐﾋ▸bash ls -la` / `01▸ total 48 (+9)` |
| `root_shell.lua` | `root@pici:~# bash ls -la` / `⤷ total 48 (+9 more)` |
| `circuit_board.lua` | `┤bash├ ls -la` / `● total 48 (+9)` |
| `glitch_tag.lua` | `▓▒░bash ls -la` / `>> total 48 (+9)` |
| `ascii_skull.lua` | `☠bash ls -la` / `✓ total 48 (+9)` |
| `trade_stats.lua` | `[bash] ls -la` / `total 48 (+9 more) │ EXIT 0` |
| `hud_brackets.lua` | `⟦bash⟧ ls -la` / `│ total 48 (+9) ⌊OK⌋` |
| `block_meter.lua` | `[▓▓▓] bash ls -la` / `total 48 (+9)  [▓▓▓]` |

Errors are always tinted red across every theme (`[err]`/`0x`/`✗`/`!!`/
`☠`/`EXIT 1`/`⌊FAIL⌋`/red meter, depending on the file), and success is
green/dim, so the failure mode reads the same regardless of which theme is
active.

`trade_stats.lua`'s `EXIT 0`/`EXIT 1` is derived from `ctx.is_error` — the
hook context carries no real duration or process exit code, so nothing here
fabricates one.

### Install

Same pattern as `nfo_format.lua`, just swap the filename:

```bash
pici --hooks-file addons/circuit_board.lua
# or:
mkdir -p ~/.config/pici/addons && cp addons/circuit_board.lua ~/.config/pici/addons/
```

```toml
# ~/.config/pici/config.toml
[addons]
files = ["~/.config/pici/addons/circuit_board.lua"]
```

Only load one of these at a time (or list your preferred one last) — since
composition is "last non-nil wins" per hook, and every theme here returns
non-nil for every tool, whichever loads last wins outright rather than
blending.

### Testing

```bash
./build/pi-cli --test addons/test_nfo_classic.lua
./build/pi-cli --test addons/test_scene_release.lua
./build/pi-cli --test addons/test_matrix_drop.lua
./build/pi-cli --test addons/test_root_shell.lua
./build/pi-cli --test addons/test_circuit_board.lua
./build/pi-cli --test addons/test_glitch_tag.lua
./build/pi-cli --test addons/test_ascii_skull.lua
./build/pi-cli --test addons/test_trade_stats.lua
./build/pi-cli --test addons/test_hud_brackets.lua
./build/pi-cli --test addons/test_block_meter.lua
```

All ten also run via `ctest -R test-addons`.

## minimal_dot.lua — clean, distinguished theme (no brackets, no badges)

The ten themes above all share one skeleton — `<glyph>tag<glyph> args` then
`<glyph> content <badge>` — just reskinned with different brackets and
colors. `minimal_dot.lua` is structurally different: a single colored dot is
the only ornament, everything else is plain typographic hierarchy.

```text
● bash  ls -la /var/log
  ↳ total 48, drwxr-xr-x … +9 lines

● bash  cat missing.txt
  ↳ cat: missing.txt: No such file or directory
```

The dot's color identifies the **tool category** (`bash` magenta, `read`
cyan, `glob`/`grep` green, `write`/`edit` yellow, anything else gray) — that
has to be decided at `format_tool_call` time, before the tool has run, so it
can't reflect success/failure. The `↳` on the result line carries the actual
outcome color instead (gray/dim on success, red on error), since `is_error`
is only available in `format_tool_result`. No `[ok]`/`[err]` badges — status
lives entirely in that one color.

### Install

```bash
pici --hooks-file addons/minimal_dot.lua
# or:
mkdir -p ~/.config/pici/addons && cp addons/minimal_dot.lua ~/.config/pici/addons/
```

```toml
# ~/.config/pici/config.toml
[addons]
files = ["~/.config/pici/addons/minimal_dot.lua"]
```

### Testing

```bash
./build/pi-cli --test addons/test_minimal_dot.lua
```

Also runs via `ctest -R test-addons`.

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
