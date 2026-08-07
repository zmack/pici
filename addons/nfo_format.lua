-- nfo_format.lua — compact warez-nfo / hacker-style tool-call display.
--
-- A separate, ready-to-use theme (as opposed to tool_format.lua, which is
-- the minimal documented example covering only `bash`). This one covers
-- every tool via a small field-priority guess at "the interesting argument",
-- and keeps both the call line and the result line to a single line each —
-- see plans/lua-tool-call-formatting.md for why only SGR color codes survive
-- the sanitizer (no cursor movement / screen clears), which is why this
-- avoids boxes and multi-line frames.
--
-- Install: see addons/README.md "nfo_format.lua" section.

local C = {
  reset = "\27[0m",
  bold = "\27[1m",
  dim = "\27[2m",
  gray = "\27[90m",
  cyan = "\27[36m",
  green = "\27[32m",
  red = "\27[31m",
}

local function one_line(s, limit)
  s = (s or ""):gsub("%s+", " "):gsub("^%s+", ""):gsub("%s+$", "")
  if limit and #s > limit then
    return s:sub(1, limit - 1) .. "…"
  end
  return s
end

-- Best-effort one-line summary of a tool's args table: each tool names its
-- "main" argument differently (bash: command, read/write/edit: file_path,
-- glob/grep: pattern, ...), so try known field names first, then fall back
-- to the first scalar field present rather than showing nothing.
local ARG_FIELDS = { "command", "file_path", "path", "pattern", "url", "prompt", "query" }

local function summarize_args(args)
  for _, key in ipairs(ARG_FIELDS) do
    local v = args[key]
    if type(v) == "string" and v ~= "" then
      return one_line(v, 72)
    end
  end
  for k, v in pairs(args) do
    if type(v) == "string" or type(v) == "number" or type(v) == "boolean" then
      return one_line(k .. "=" .. tostring(v), 72)
    end
  end
  return ""
end

-- First non-blank line of content plus a count of any remaining lines, so
-- the result stays one line regardless of how much output a tool produced.
local function summarize_content(content)
  content = (content or ""):gsub("%s+$", "")
  if content == "" then
    return "(empty)", 0
  end
  local first, rest = content:match("^([^\n]*)\n?(.*)$")
  local extra = 0
  if rest ~= "" then
    local _, count = rest:gsub("\n", "\n")
    extra = count + 1
  end
  return one_line(first, 72), extra
end

return {
  format_tool_call = function(ctx)
    local summary = summarize_args(ctx.args or {})
    local tag = C.gray .. "[" .. C.reset .. C.bold .. C.green .. ctx.tool_name .. C.reset .. C.gray .. "]" .. C.reset
    if summary == "" then
      return "\n" .. tag .. "\n"
    end
    return "\n" .. tag .. " " .. C.cyan .. summary .. C.reset .. "\n"
  end,

  format_tool_result = function(ctx)
    local first, extra = summarize_content(ctx.content)
    local color = ctx.is_error and C.red or C.dim
    local badge = ctx.is_error and (C.red .. C.bold .. "[err]" .. C.reset) or (C.green .. C.bold .. "[ok]" .. C.reset)
    local suffix = ""
    if extra > 0 then
      suffix = " " .. C.gray .. "(+" .. extra .. " more)" .. C.reset
    end
    return "  " .. C.gray .. "»" .. C.reset .. " " .. color .. first .. C.reset .. suffix .. " " .. badge
  end,
}
