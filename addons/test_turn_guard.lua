-- Tests for the diagnostic turn guard add-on.
local addon = dofile("addons/turn_guard.lua")

pici.test.run("blocks mutating tools", function()
  for _, name in ipairs({"bash", "edit", "write"}) do
    local result = addon.before_tool_call({tool_name = name, args = {}})
    pici.test.ok(result ~= nil and result.block, "should block " .. name)
  end
end)

pici.test.run("allows read-only tools", function()
  for _, name in ipairs({"read", "ls", "grep", "find"}) do
    local result = addon.before_tool_call({tool_name = name, args = {}})
    pici.test.ok(result == nil, "should allow " .. name)
  end
end)

pici.test.run("stops after one tool batch", function()
  pici.test.ok(addon.should_stop_after_turn({}))
end)
