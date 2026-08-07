-- Tests for root_shell addon via pici --test harness.
local addon = dofile("addons/root_shell.lua")

pici.test.run("format_tool_call renders a root@pici prompt with tool + args", function()
  local s = addon.format_tool_call({tool_name = "bash", call_id = "c1", args = {command = "echo hi"}})
  pici.test.ok(s:find("root@pici", 1, true) ~= nil)
  pici.test.ok(s:find("bash", 1, true) ~= nil)
  pici.test.ok(s:find("echo hi", 1, true) ~= nil)
end)

pici.test.run("format_tool_call omits trailing space when no summary", function()
  local s = addon.format_tool_call({tool_name = "bash", call_id = "c1", args = {}})
  pici.test.ok(s ~= nil and s:find("bash", 1, true) ~= nil)
end)

pici.test.run("format_tool_result marks errors with a red cross", function()
  local ok_s = addon.format_tool_result({tool_name = "bash", call_id = "c1", args = {}, content = "fine", is_error = false})
  local err_s = addon.format_tool_result({tool_name = "bash", call_id = "c1", args = {}, content = "boom", is_error = true})
  pici.test.ok(ok_s:find("✗", 1, true) == nil, "success should not show cross")
  pici.test.ok(err_s:find("✗", 1, true) ~= nil, "error should show cross")
  pici.test.ok(err_s:find("\27[31m", 1, true) ~= nil)
end)

pici.test.run("format_tool_result reports remaining line count", function()
  local s = addon.format_tool_result({tool_name = "bash", call_id = "c1", args = {}, content = "a\nb\nc", is_error = false})
  pici.test.ok(s:find("(+2 more)", 1, true) ~= nil, tostring(s))
end)

pici.test.run("works for any tool name, not just bash", function()
  local s = addon.format_tool_call({tool_name = "grep", call_id = "c2", args = {pattern = "TODO"}})
  pici.test.ok(s:find("grep", 1, true) ~= nil)
  pici.test.ok(s:find("TODO", 1, true) ~= nil)
end)
