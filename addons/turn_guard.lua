-- Diagnostic guard: allow one assistant/tool batch, but prevent mutations.
-- Use this add-on when investigating model behavior in an existing worktree.

local blocked_tools = {
  bash = true,
  edit = true,
  write = true,
}

return {
  before_tool_call = function(ctx)
    if blocked_tools[ctx.tool_name] then
      return {
        block = true,
        reason = "turn guard blocked mutating tool: " .. ctx.tool_name,
      }
    end
    return nil
  end,

  -- Return control after the first assistant response and its tool results.
  -- This is the useful boundary for inspecting why the model chose its tools.
  should_stop_after_turn = function(_)
    return true
  end,
}
