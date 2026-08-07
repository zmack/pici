-- Tests for nfo_format addon via pici --test harness.
local addon = dofile("addons/nfo_format.lua")

pici.test.run("format_tool_call covers every tool, not just bash", function()
  local s = addon.format_tool_call({tool_name = "bash", call_id = "c1", args = {command = "echo hi"}})
  pici.test.ok(s ~= nil)
  pici.test.ok(s:find("bash", 1, true) ~= nil, "should mention tool name: " .. tostring(s))
  pici.test.ok(s:find("echo hi", 1, true) ~= nil, "should show command: " .. tostring(s))
end)

pici.test.run("format_tool_call picks known arg fields per tool", function()
  local read_s = addon.format_tool_call({tool_name = "read", call_id = "c2", args = {file_path = "/tmp/x"}})
  pici.test.ok(read_s:find("/tmp/x", 1, true) ~= nil, "read should show file_path: " .. tostring(read_s))

  local grep_s = addon.format_tool_call({tool_name = "grep", call_id = "c3", args = {pattern = "TODO"}})
  pici.test.ok(grep_s:find("TODO", 1, true) ~= nil, "grep should show pattern: " .. tostring(grep_s))
end)

pici.test.run("format_tool_call falls back to key=value for unknown fields", function()
  local s = addon.format_tool_call({tool_name = "custom", call_id = "c4", args = {n = 42}})
  pici.test.ok(s:find("n=42", 1, true) ~= nil, "should show fallback key=value: " .. tostring(s))
end)

pici.test.run("format_tool_call handles empty args without crashing", function()
  local s = addon.format_tool_call({tool_name = "bash", call_id = "c5", args = {}})
  pici.test.ok(s ~= nil)
  pici.test.ok(s:find("bash", 1, true) ~= nil)
end)

pici.test.run("format_tool_call always returns a string (never falls back to nil)", function()
  for _, name in ipairs({"bash", "read", "edit", "write", "glob", "grep", "anything"}) do
    local s = addon.format_tool_call({tool_name = name, call_id = "c", args = {}})
    pici.test.ok(type(s) == "string", name .. " should get a themed block, not nil")
  end
end)

pici.test.run("format_tool_result stays a single line regardless of content length", function()
  local long_content = "a\nb\nc\nd\ne\nf\ng\nh"
  local s = addon.format_tool_result({
    tool_name = "bash", call_id = "c1", args = {}, content = long_content, is_error = false,
  })
  pici.test.ok(s ~= nil)
  pici.test.ok(not s:find("\n", 1, true), "result should be one line: " .. tostring(s))
  pici.test.ok(s:find("(+7 more)", 1, true) ~= nil, "should count remaining lines: " .. tostring(s))
end)

pici.test.run("format_tool_result shows first line verbatim when short", function()
  local s = addon.format_tool_result({
    tool_name = "bash", call_id = "c1", args = {}, content = "hello", is_error = false,
  })
  pici.test.ok(s:find("hello", 1, true) ~= nil)
  pici.test.ok(s:find("more)", 1, true) == nil, "no remaining-lines suffix for single-line content")
end)

pici.test.run("format_tool_result handles empty content", function()
  local s = addon.format_tool_result({tool_name = "bash", call_id = "c1", args = {}, content = "", is_error = false})
  pici.test.ok(s:find("(empty)", 1, true) ~= nil, "should show placeholder: " .. tostring(s))
end)

pici.test.run("format_tool_result tints errors red with [err] badge", function()
  local ok_block = addon.format_tool_result({
    tool_name = "bash", call_id = "c1", args = {}, content = "ok", is_error = false,
  })
  local err_block = addon.format_tool_result({
    tool_name = "bash", call_id = "c1", args = {}, content = "boom", is_error = true,
  })
  pici.test.ok(ok_block:find("\27[31m", 1, true) == nil, "success should not be red")
  pici.test.ok(err_block:find("\27[31m", 1, true) ~= nil, "error should be red: " .. tostring(err_block))
  pici.test.ok(ok_block:find("[ok]", 1, true) ~= nil, "success should carry [ok] badge")
  pici.test.ok(err_block:find("[err]", 1, true) ~= nil, "error should carry [err] badge")
end)

pici.test.run("format_tool_call and format_tool_result never return nil (always themed)", function()
  local call_s = addon.format_tool_call({tool_name = "weird_tool", call_id = "c9", args = {}})
  local result_s = addon.format_tool_result({tool_name = "weird_tool", call_id = "c9", args = {}, content = "x", is_error = false})
  pici.test.ok(call_s ~= nil and result_s ~= nil, "this theme opts every tool in, unlike tool_format.lua's bash-only example")
end)
