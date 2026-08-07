-- Tests for nfo_classic addon via pici --test harness.
local addon = dofile("addons/nfo_classic.lua")

pici.test.run("format_tool_call themes every tool", function()
  local s = addon.format_tool_call({tool_name = "bash", call_id = "c1", args = {command = "echo hi"}})
  pici.test.ok(s:find("bash", 1, true) ~= nil)
  pici.test.ok(s:find("echo hi", 1, true) ~= nil)
  pici.test.ok(s:find("╣", 1, true) ~= nil and s:find("╠", 1, true) ~= nil, "should have nfo-tag brackets")
end)

pici.test.run("format_tool_call handles empty args", function()
  local s = addon.format_tool_call({tool_name = "read", call_id = "c2", args = {}})
  pici.test.ok(s ~= nil and s:find("read", 1, true) ~= nil)
end)

pici.test.run("format_tool_result stays single line and counts remaining lines", function()
  local s = addon.format_tool_result({tool_name = "bash", call_id = "c1", args = {}, content = "a\nb\nc\nd", is_error = false})
  pici.test.ok(not s:find("\n", 1, true))
  pici.test.ok(s:find("(+3 more)", 1, true) ~= nil, tostring(s))
end)

pici.test.run("format_tool_result badges ok vs err", function()
  local ok_s = addon.format_tool_result({tool_name = "bash", call_id = "c1", args = {}, content = "fine", is_error = false})
  local err_s = addon.format_tool_result({tool_name = "bash", call_id = "c1", args = {}, content = "boom", is_error = true})
  pici.test.ok(ok_s:find("[ok]", 1, true) ~= nil)
  pici.test.ok(err_s:find("[err]", 1, true) ~= nil)
  pici.test.ok(err_s:find("\27[31m", 1, true) ~= nil, "error should be red")
end)

pici.test.run("format_tool_result handles empty content", function()
  local s = addon.format_tool_result({tool_name = "bash", call_id = "c1", args = {}, content = "", is_error = false})
  pici.test.ok(s:find("(empty)", 1, true) ~= nil)
end)
