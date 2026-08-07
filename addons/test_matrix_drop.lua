-- Tests for matrix_drop addon via pici --test harness.
local addon = dofile("addons/matrix_drop.lua")

pici.test.run("format_tool_call shows glyph prefix and tool name", function()
  local s = addon.format_tool_call({tool_name = "bash", call_id = "c1", args = {command = "echo hi"}})
  pici.test.ok(s:find("bash", 1, true) ~= nil)
  pici.test.ok(s:find("echo hi", 1, true) ~= nil)
  pici.test.ok(s:find("▸", 1, true) ~= nil)
end)

pici.test.run("format_tool_result uses 01 marker on success, 0x on error", function()
  local ok_s = addon.format_tool_result({tool_name = "bash", call_id = "c1", args = {}, content = "fine", is_error = false})
  local err_s = addon.format_tool_result({tool_name = "bash", call_id = "c1", args = {}, content = "boom", is_error = true})
  pici.test.ok(ok_s:find("01", 1, true) ~= nil)
  pici.test.ok(err_s:find("0x", 1, true) ~= nil)
  pici.test.ok(err_s:find("\27[31m", 1, true) ~= nil, "error should be red")
end)

pici.test.run("format_tool_result stays single line", function()
  local s = addon.format_tool_result({tool_name = "bash", call_id = "c1", args = {}, content = "a\nb\nc\nd\ne", is_error = false})
  pici.test.ok(not s:find("\n", 1, true))
  pici.test.ok(s:find("(+4)", 1, true) ~= nil, tostring(s))
end)

pici.test.run("format_tool_call handles empty args without crashing", function()
  local s = addon.format_tool_call({tool_name = "read", call_id = "c2", args = {}})
  pici.test.ok(s ~= nil and s:find("read", 1, true) ~= nil)
end)

pici.test.run("covers non-bash tools too", function()
  local s = addon.format_tool_call({tool_name = "custom_tool", call_id = "c3", args = {n = 1}})
  pici.test.ok(s:find("custom_tool", 1, true) ~= nil)
  pici.test.ok(s:find("n=1", 1, true) ~= nil)
end)
