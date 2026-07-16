-- Example Phase 1 permission policy.
-- Load this explicitly with --hooks-file or from your configured add-on dir.
-- Policies live in Lua; the core guarantees the before_tool_call decision is
-- made before execution and fails closed if the hook itself errors.

local blocked_tools = {
  edit = true,
  write = true,
}

local blocked_commands = {
  "rm%s+%-[%w-]*r[%w-]*f",
  "git%s+reset%s+%-%-hard",
  "git%s+clean%s+%-[%w-]*f",
  "mkfs",
}

local function command_is_blocked(command)
  for _, pattern in ipairs(blocked_commands) do
    if command:match(pattern) then
      return true
    end
  end
  return false
end

return {
  before_tool_call = function(ctx)
    if blocked_tools[ctx.tool_name] then
      return {
        block = true,
        reason = "permission policy blocked mutating tool: " .. ctx.tool_name,
      }
    end

    if ctx.tool_name == "bash" then
      local command = (ctx.args and ctx.args.command) or ""
      if command_is_blocked(command) then
        return {
          block = true,
          reason = "permission policy blocked dangerous bash command",
        }
      end
    end

    return nil
  end,
}
