-- tool_format.lua — minimal, heavily-documented tool-call formatting add-on.
--
-- Purpose
--   Demonstrates the two display-only hooks that customize the human-readable
--   blocks VerboseRenderer prints for every tool call:
--
--     format_tool_call(ctx)   → string | nil   (fired once at call start)
--     format_tool_result(ctx) → string | nil   (fired once when result ready)
--
--   This file is intentionally minimal: it customizes `bash` and leaves every
--   other tool on the built-in formatting. Copy it, tweak the `if` branches,
--   or delete one of the two functions — either hook is optional. The long
--   comment below is the actual guide; the code at the bottom is the runnable
--   example.
--
-- What you are replacing
--   The default CLI prints, via VerboseRenderer (src/main.cpp), exactly:
--
--     [tool: name(<raw JSON args>)]          -- on_tool_start
--       [name] -> <truncated result>         -- on_tool_end
--
--   `->` content is truncated to 5 lines (first 2 + "… +N lines omitted" +
--   last 2). That truncation lives in `truncate_tool_result()` (src/core/
--   terminal.cpp) and is now exposed to Lua as `pici.truncate_tool_result()`.
--   No other renderer prints these blocks: RawStreamRenderer/DiffMarkdownRenderer
--   have no on_tool_* override, ViewportRenderer only tracks active tools for
--   its status line (already customizable via `status_line`), and the wire
--   renderers (AcpSse/Sync) are out of scope.
--
-- Hook signatures (authoritative: see src/core/lua_tool.h)
--
--   format_tool_call(ctx) → string | nil
--     ctx = {
--       tool_name: string,          -- e.g. "bash", "read", "edit"
--       call_id:   string,          -- correlates start ↔ end; required for
--                                   -- parallel calls that interleave
--       args:      table,           -- *decoded* arguments table via
--                                   -- json_to_lua, NOT a JSON string.
--                                   -- Index directly: ctx.args.command
--                                   -- (bash), ctx.args.file_path (read),
--                                   -- etc. — same shape as
--                                   -- before_tool_call/after_tool_call.
--                                   -- If the model's JSON was malformed,
--                                   -- you get an empty table {} and the turn
--                                   -- still completes (no throw in renderer).
--     }
--     Return the *entire* text block to print for this call (ANSI SGR color
--     codes allowed, see Sanitization). Include your own leading "\n" if you
--     want one — the example does. Return nil to fall back to the built-in
--     "[tool: name(args)]" line, byte-for-byte unchanged. Raising a Lua error
--     also falls back (silently, no crash, tool still runs).
--
--   format_tool_result(ctx) → string | nil
--     ctx = {
--       tool_name: string,
--       call_id:   string,
--       args:      table,           -- decoded args *for this call*. The
--                                   -- underlying Renderer::on_tool_end does
--                                   -- NOT carry args, so VerboseRenderer keeps
--                                   -- a call_id → args cache populated in
--                                   -- on_tool_start and erased in on_tool_end.
--                                   -- Correct even with parallel tool calls.
--                                   -- Missing call_id → {}.
--       content:   string,          -- *full, untruncated* result text.
--                                   -- You own truncation — call
--                                   -- pici.truncate_tool_result(content) if
--                                   -- you only want the built-in 5-line rule
--                                   -- plus your own colors/wrapper.
--       is_error:  boolean,
--     }
--     Return the full result block. Nil / error → built-in gray
--     "  [name] -> ..." with truncation.
--
--   Both hooks may stall streaming while they run (they hold the same Lua
--   mutex_ as before_tool_call). No timeout is applied. Keep them fast and
--   side-effect free — they are pure formatters, they cannot block or rewrite
--   the call/result (use before_tool_call/after_tool_call for that).
--
-- Sanitization (required, not optional)
--   Your returned string is written to std::cout and, under ViewportRenderer,
--   into an alt-screen viewport that repaints with cursor-position escapes.
--   Before printing, pici runs sanitize_tool_output() (src/core/terminal.cpp):
--     - Strips invalid UTF-8 bytes (same fix as sanitize_terminal_title, 6ecda63).
--     - Drops every ANSI/VT escape *except* SGR color codes (CSI … m). So
--       "\27[31m" / "\27[0m" survive, but "\27[2J", "\27[H", "\27[?1049h",
--       OSC, DCS, etc. are removed. This lets you colorize without letting a
--       hook clear the screen or move the cursor.
--   You do not need to sanitize yourself; just avoid emitting those sequences.
--
-- Truncation helper
--   pici.truncate_tool_result(content: string) → string
--     Returns pici's built-in display string for a result, including the
--     leading " -> " / "    " prefixes. Useful when you only want different
--     colors:
--
--       return "\27[32m" .. pici.truncate_tool_result(ctx.content) .. "\27[0m"
--
--   Available both in hooks add-ons and in `pici --test` files.
--
-- Composition & hot-reload
--   compose_hooks() rule for both hooks: **last non-nil wins** (same as
--   status_line/tab_title). Load two add-ons that both define
--   format_tool_call — the later-loaded one's non-nil result is used.
--   VerboseRenderer snapshots HookRuntime's shared_ptr<LuaHooks> *once per
--   tool-call event*, so `/reload-addons` picks up your edited file on the
--   very next tool call without restarting pici.
--
-- Interaction with other hooks
--   before_tool_call / after_tool_call still control execution (block/rewrite).
--   These two hooks are display-only and never see `turn` (the counter that
--   before/after get) — VerboseRenderer has no AgentContext to derive it.
--   Child/subagent calls (AgentTaskManager) null all hooks and never reach
--   VerboseRenderer anyway.
--
-- Install / try it
--   Per-run:   pici --hooks-file addons/tool_format.lua
--   Via config (~/.config/pici/config.toml):
--     [addons]
--     files = ["~/.config/pici/addons/tool_format.lua"]
--   Dir mode:  pici --hooks-dir addons/   (then /reload-addons after edits)
--   Smoke test: mix `bash` and `read` calls — bash should show the custom
--   block below, read should look stock.
--
--   Tests:  ./build/pi-cli --test addons/test_tool_format.lua
--           (also runs as the `test-addons` CTest target)
--
-- Minimal runnable example ----------------------------------------------------
-- Customize only `bash`; everything else falls back to pici's default.

local C = {
  reset = "\27[0m",
  dim = "\27[2m",
  cyan = "\27[36m",
  yellow = "\27[33m",
  red = "\27[31m",
  gray = "\27[90m",
}

local function one_line(s, limit)
  s = (s or ""):gsub("%s+", " "):gsub("^%s+", ""):gsub("%s+$", "")
  if limit and #s > limit then
    return s:sub(1, limit - 3) .. "..."
  end
  return s
end

return {
  -- Called at tool start. Return nil for any tool you don't want to theme.
  format_tool_call = function(ctx)
    if ctx.tool_name ~= "bash" then
      return nil -- default "[tool: name(args)]" for read/edit/etc.
    end
    -- ctx.args is already a Lua table — no json.decode needed.
    local cmd = one_line(ctx.args.command, 88)
    if cmd == "" then
      cmd = "(no command)"
    end
    -- Leading "\n" matches the built-in layout; SGR colors survive sanitization.
    -- Any cursor/screen escapes here would be stripped — only SGR (…m) is kept.
    return string.format("\n%s[bash]%s %s%s%s\n", C.cyan, C.reset, C.yellow, cmd, C.reset)
  end,

  -- Called when the result is fully available. `content` is the *full* text,
  -- not the truncated preview. Use pici.truncate_tool_result() to reuse the
  -- built-in 5-line / first-2-last-2 rule and just add color.
  format_tool_result = function(ctx)
    if ctx.tool_name ~= "bash" then
      return nil -- default gray "  [name] -> ..." for other tools
    end
    local body = pici.truncate_tool_result(ctx.content)
    -- pici.truncate_tool_result already includes " -> " prefixes; we just
    -- tint the block (red for errors, dim for success) while preserving those
    -- prefixes. SGR codes are kept, screen-clearing escapes would be dropped.
    if ctx.is_error then
      return C.red .. body .. C.reset
    end
    return C.dim .. body .. C.reset
  end,
}
