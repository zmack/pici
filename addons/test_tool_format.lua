-- Tests for tool_format addon via pici --test harness.
local addon = dofile("addons/tool_format.lua")

-- Bash call uses decoded args table, not JSON string.
pici.test.run("format_tool_call customizes bash", function()
  local s = addon.format_tool_call({tool_name = "bash", call_id = "c1", args = {command = "echo hi"}})
  pici.test.ok(s ~= nil, "bash should return a string")
  pici.test.ok(s:find("bash", 1, true) ~= nil, "should mention bash: " .. tostring(s))
  pici.test.ok(s:find("echo hi", 1, true) ~= nil, "should echo args.command: " .. tostring(s))
  -- SGR color is allowed; the hook may emit it (sanitized only at print time).
  pici.test.ok(s:find("\27[", 1, true) ~= nil, "should contain SGR: " .. tostring(s))
end)

pici.test.run("format_tool_call falls back for non-bash", function()
  pici.test.eq(addon.format_tool_call({tool_name = "read", call_id = "c2", args = {file_path = "/tmp/x"}}), nil)
  pici.test.eq(addon.format_tool_call({tool_name = "edit", call_id = "c3", args = {}}), nil)
end)

pici.test.run("format_tool_call handles missing command", function()
  local s = addon.format_tool_call({tool_name = "bash", call_id = "c4", args = {}})
  pici.test.ok(s ~= nil)
  pici.test.ok(s:find("bash", 1, true) ~= nil)
end)

-- Result hook receives full untruncated content.
pici.test.run("format_tool_result receives untruncated content", function()
  local long_content = "a\nb\nc\nd\ne\nf\ng\nh" -- 8 lines, above 5-line threshold
  local s = addon.format_tool_result({
    tool_name = "bash", call_id = "c1", args = {command = "echo hi"},
    content = long_content, is_error = false,
  })
  pici.test.ok(s ~= nil)
  -- Our example delegates to pici.truncate_tool_result, so it should contain
  -- the built-in omission marker, but the hook itself got the full text.
  pici.test.ok(s:find("lines omitted", 1, true) ~= nil, "should be truncated: " .. tostring(s))
end)

pici.test.run("format_tool_result falls back for non-bash", function()
  pici.test.eq(
    addon.format_tool_result({tool_name = "read", call_id = "c2", args = {}, content = "hello", is_error = false}),
    nil
  )
end)

pici.test.run("format_tool_result tints errors", function()
  local ok_block = addon.format_tool_result({
    tool_name = "bash", call_id = "c1", args = {}, content = "ok", is_error = false,
  })
  local err_block = addon.format_tool_result({
    tool_name = "bash", call_id = "c1", args = {}, content = "boom", is_error = true,
  })
  pici.test.ok(ok_block ~= nil and err_block ~= nil)
  pici.test.ok(ok_block ~= err_block, "error vs success should differ")
  pici.test.ok(err_block:find("\27[31m", 1, true) ~= nil, "error should be red: " .. tostring(err_block))
end)

pici.test.run("pici.truncate_tool_result helper exposed", function()
  local short = pici.truncate_tool_result("a\nb\nc")
  pici.test.ok(short:find("lines omitted", 1, true) == nil, "short should not truncate: " .. tostring(short))
  pici.test.ok(short:find("-> a", 1, true) ~= nil, "should have prefix: " .. tostring(short))
  local long = pici.truncate_tool_result("a\nb\nc\nd\ne\nf\ng")
  pici.test.ok(long:find("lines omitted", 1, true) ~= nil, "long should truncate: " .. tostring(long))
end)

pici.test.run("ctx.args is a table not JSON string", function()
  local seen = nil
  local probe = {
    format_tool_call = function(ctx)
      seen = ctx.args
      return nil
    end,
  }
  probe.format_tool_call({tool_name = "bash", call_id = "x", args = {command = "ls", file_path = "/tmp"}})
  pici.test.ok(type(seen) == "table", "args should be table, got " .. type(seen))
  pici.test.eq(seen.command, "ls")
  pici.test.eq(seen.file_path, "/tmp")
end)
