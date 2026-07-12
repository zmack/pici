# pici add-ons

Lua plugins loaded via `--hooks-file`, `--hooks-dir`, or `config.toml [addons]`.

## costline.lua — show last cost in prompt

Displays token cost / usage of the last turn in the REPL prompt line.

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

| Situation | Prompt |
|-----------|--------|
| First turn (no usage yet) | `> ` |
| Model with pricing | `[$0.0042 sess:$0.01] > ` |
| Model without pricing | `[1.0K tok] > ` or `[803→123] >` |
| With cache hits | `[$0.001 cache:627] >` |
| With session total | `[$0.002 sess:$0.015] >` |

### How it works

Uses the `prompt_line(ctx)` hook. `ctx` contains:

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

Returns `nil` before first turn → falls back to default `> `. Return a string to replace the prompt.

### Testing

```bash
./build/pi-cli --test addons/test_costline.lua
```

See `test_costline.lua` for `pici.test` examples: cost formatting, cache, session, nil handling.

### Customising

Edit `costline.lua` — e.g. to show only cost, no session:

```lua
return {
  prompt_line = function(ctx)
    if not ctx.last or ctx.last.cost.total == 0 then return nil end
    return string.format("[$%.4f] > ", ctx.last.cost.total)
  end
}
```

Or with ANSI colour:

```lua
return {
  prompt_line = function(ctx)
    local c = ctx.last.cost.total
    if c == 0 then return nil end
    return string.format("[\27[32m$%.4f\27[0m] > ", c)
  end
}
```

## context.lua — inspect model context

`context.lua` adds `/context` and `/context effective`. The first command displays
the current raw agent state; the second displays the most recent request-ready
context after context transformation and message conversion. Before the first
model request, `/context effective` reports that no snapshot is available.

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
