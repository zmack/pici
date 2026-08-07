-- root_shell.lua — bare hacker-prompt theme.
--
-- Style 4 of the compact theme set. See nfo_format.lua's header comment for
-- the shared design constraints (SGR color only, single-line blocks).

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
    local prompt = C.green .. "root@pici" .. C.reset .. C.gray .. ":" .. C.reset .. C.cyan .. "~" .. C.reset .. C.gray .. "#" .. C.reset
    local cmd = ctx.tool_name
    if summary ~= "" then
      cmd = cmd .. " " .. summary
    end
    return "\n" .. prompt .. " " .. cmd .. "\n"
  end,

  format_tool_result = function(ctx)
    local first, extra = summarize_content(ctx.content)
    local suffix = extra > 0 and (" " .. C.gray .. "(+" .. extra .. " more)" .. C.reset) or ""
    if ctx.is_error then
      return C.gray .. "⤷" .. C.reset .. " " .. C.red .. "✗ " .. first .. C.reset .. suffix
    end
    return C.gray .. "⤷" .. C.reset .. " " .. first .. suffix
  end,
}
