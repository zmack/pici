local addon = dofile("addons/permissions.lua")

pici.test.run("blocks mutating tools", function()
  for _, name in ipairs({"edit", "write"}) do
    local result = addon.before_tool_call({tool_name = name, args = {}})
    pici.test.ok(result ~= nil and result.block, "should block " .. name)
  end
end)

pici.test.run("blocks dangerous bash commands", function()
  local result = addon.before_tool_call({
    tool_name = "bash",
    args = {command = "git reset --hard HEAD"},
  })
  pici.test.ok(result ~= nil and result.block)
end)

pici.test.run("allows ordinary read-only work", function()
  pici.test.ok(addon.before_tool_call({
    tool_name = "bash", args = {command = "git status"},
  }) == nil)
  pici.test.ok(addon.before_tool_call({tool_name = "read", args = {}}) == nil)
end)
