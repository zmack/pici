-- Tests for scene_release addon via pici --test harness.
local addon = dofile("addons/scene_release.lua")

pici.test.run("format_tool_call shows PICI tag and tool name", function()
  local s = addon.format_tool_call({tool_name = "bash", call_id = "c1", args = {command = "echo hi"}})
  pici.test.ok(s:find("PICI", 1, true) ~= nil)
  pici.test.ok(s:find("bash", 1, true) ~= nil)
  pici.test.ok(s:find("echo hi", 1, true) ~= nil)
end)

pici.test.run("format_tool_call picks known arg field per tool", function()
  local s = addon.format_tool_call({tool_name = "grep", call_id = "c2", args = {pattern = "TODO"}})
  pici.test.ok(s:find("TODO", 1, true) ~= nil)
end)

pici.test.run("format_tool_result colors arrow green on success, red on error", function()
  local ok_s = addon.format_tool_result({tool_name = "bash", call_id = "c1", args = {}, content = "fine", is_error = false})
  local err_s = addon.format_tool_result({tool_name = "bash", call_id = "c1", args = {}, content = "boom", is_error = true})
  pici.test.ok(ok_s:find("\27[32m", 1, true) ~= nil, "success arrow should be green")
  pici.test.ok(err_s:find("\27[31m", 1, true) ~= nil, "error arrow should be red")
end)

pici.test.run("format_tool_result reports extra line count", function()
  local s = addon.format_tool_result({tool_name = "bash", call_id = "c1", args = {}, content = "a\nb\nc", is_error = false})
  pici.test.ok(s:find("+2 lines", 1, true) ~= nil, tostring(s))
end)

pici.test.run("format_tool_call always returns a string, never nil", function()
  local s = addon.format_tool_call({tool_name = "anything", call_id = "c3", args = {}})
  pici.test.ok(type(s) == "string")
end)
