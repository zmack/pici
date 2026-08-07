-- minimal_dot.lua — clean, distinguished tool-call display: a single colored
-- dot as the only ornament, everything else is plain typographic hierarchy.
--
-- Distinct in structure (not just palette) from nfo_format.lua and the ten
-- compact themes: no brackets, no [ok]/[err] badges, no glyph clusters. The
-- dot's color identifies the *tool category* (bash, read, write/edit,
-- search, other) so different tools are visually distinguishable at a
-- glance even before you read the tool name — this can only be decided at
-- format_tool_call time, since the outcome isn't known yet. The result
-- line's "↳" carries the actual outcome color (dim/gray on success, red on
-- error), since that's the only point ctx.is_error is available.
--
-- See nfo_format.lua's header comment for the shared design constraints
-- (SGR color only survives the sanitizer — no cursor movement, no boxes).

local C = {
  reset = "\27[0m",
  bold = "\27[1m",
  dim = "\27[2m",
  gray = "\27[90m",
  red = "\27[31m",
  green = "\27[32m",
  yellow = "\27[33m",
  cyan = "\27[36m",
  magenta = "\27[35m",
}

-- Dot color by tool category — purely cosmetic grouping, not a security
-- boundary. Unrecognized tools fall back to gray rather than guessing.
local TOOL_DOT_COLOR = {
  bash = C.magenta,
  read = C.cyan,
  glob = C.green,
  grep = C.green,
  write = C.yellow,
  edit = C.yellow,
}

local function one_line(s, limit)
  s = (s or ""):gsub("%s+", " "):gsub("^%s+", ""):gsub("%s+$", "")
  if limit and #s > limit then
    return s:sub(1, limit - 1) .. "…"
  end
  return s
end

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
    local dot_color = TOOL_DOT_COLOR[ctx.tool_name] or C.gray
    local head = dot_color .. "●" .. C.reset .. " " .. C.bold .. ctx.tool_name .. C.reset
    if summary == "" then
      return "\n" .. head .. "\n"
    end
    return "\n" .. head .. "  " .. summary .. "\n"
  end,

  format_tool_result = function(ctx)
    local first, extra = summarize_content(ctx.content)
    local color = ctx.is_error and C.red or C.dim
    local arrow_color = ctx.is_error and C.red or C.gray
    local suffix = extra > 0 and (" " .. C.gray .. "(+" .. extra .. " more)" .. C.reset) or ""
    return "  " .. arrow_color .. "↳" .. C.reset .. " " .. color .. first .. C.reset .. suffix
  end,
}
