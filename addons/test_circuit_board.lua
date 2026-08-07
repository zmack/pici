-- Tests for circuit_board addon via pici --test harness.
local addon = dofile("addons/circuit_board.lua")

pici.test.run("format_tool_call wraps tool name in trace-line brackets", function()
  local s = addon.format_tool_call({tool_name = "bash", call_id = "c1", args = {command = "echo hi"}})
  pici.test.ok(s:find("┤", 1, true) ~= nil and s:find("├", 1, true) ~= nil)
  pici.test.ok(s:find("bash", 1, true) ~= nil)
  pici.test.ok(s:find("echo hi", 1, true) ~= nil)
end)

pici.test.run("format_tool_result uses a green dot on success, red on error", function()
  local ok_s = addon.format_tool_result({tool_name = "bash", call_id = "c1", args = {}, content = "fine", is_error = false})
  local err_s = addon.format_tool_result({tool_name = "bash", call_id = "c1", args = {}, content = "boom", is_error = true})
  pici.test.ok(ok_s:find("\27[32m", 1, true) ~= nil, "success should be green")
  pici.test.ok(err_s:find("\27[31m", 1, true) ~= nil, "error should be red")
  pici.test.ok(ok_s:find("●", 1, true) ~= nil and err_s:find("●", 1, true) ~= nil)
end)

pici.test.run("format_tool_result stays single line", function()
  local s = addon.format_tool_result({tool_name = "bash", call_id = "c1", args = {}, content = "a\nb\nc\nd", is_error = false})
  pici.test.ok(not s:find("\n", 1, true))
  pici.test.ok(s:find("(+3)", 1, true) ~= nil, tostring(s))
end)

pici.test.run("format_tool_call falls back to key=value for unknown fields", function()
  local s = addon.format_tool_call({tool_name = "custom", call_id = "c2", args = {n = 42}})
  pici.test.ok(s:find("n=42", 1, true) ~= nil)
end)

pici.test.run("format_tool_result handles empty content", function()
  local s = addon.format_tool_result({tool_name = "bash", call_id = "c1", args = {}, content = "", is_error = false})
  pici.test.ok(s:find("(empty)", 1, true) ~= nil)
end)
