-- Tests for costline addon via pici --test harness.
local addon = dofile("addons/costline.lua")

local function make_usage(t)
  t = t or {}
  t.input = t.input or 100
  t.output = t.output or 50
  t.cache_read = t.cache_read or 0
  t.cache_write = t.cache_write or 0
  t.total_tokens = t.total_tokens or (t.input + t.output)
  t.cost = t.cost or {input=0, output=0, cache_read=0, cache_write=0, total=0}
  return t
end

pici.test.run("shows $ cost when available", function()
  local ctx = {
    turn=1, model="gpt-4o", tools=7,
    last = make_usage{cost={total=0.0042, input=0, output=0, cache_read=0, cache_write=0}},
    session = make_usage{cost={total=0.01}},
  }
  local r = addon.prompt_line(ctx)
  pici.test.ok(r ~= nil, "should return custom prompt")
  pici.test.ok(r:find("%$") ~= nil, "should contain dollar: " .. tostring(r))
  pici.test.ok(r:find("> $?") ~= nil, "should end with > : " .. tostring(r))
end)

pici.test.run("nil when no usage yet", function()
  local ctx = {
    turn=0, model="gpt-4o", tools=7,
    last = make_usage{input=0, output=0, total_tokens=0, cost={total=0}},
    session = make_usage{input=0, output=0, total_tokens=0, cost={total=0}},
  }
  local r = addon.prompt_line(ctx)
  pici.test.ok(r == nil, "should be nil, got " .. tostring(r))
end)

pici.test.run("shows tok count when no pricing", function()
  local ctx = {
    turn=1, model="local", tools=7,
    last = make_usage{input=800, output=200, total_tokens=1000, cost={total=0}},
    session = make_usage{total_tokens=0, cost={total=0}},
  }
  local r = addon.prompt_line(ctx)
  pici.test.ok(r ~= nil)
  pici.test.ok(r:find("tok") ~= nil, "should contain tok: " .. tostring(r))
end)

pici.test.run("shows cache_read when present", function()
  local ctx = {
    turn=2, model="claude", tools=7,
    last = make_usage{
      cache_read=5000,
      cost={total=0.001, input=0, output=0, cache_read=0, cache_write=0},
    },
    session = make_usage{cost={total=0}},
  }
  local r = addon.prompt_line(ctx)
  pici.test.ok(r:find("cache") ~= nil, "should show cache: " .. tostring(r))
end)

pici.test.run("shows session total", function()
  local ctx = {
    turn=3, model="gpt-4o", tools=7,
    last = make_usage{cost={total=0.002}},
    session = make_usage{cost={total=0.015}},
  }
  local r = addon.prompt_line(ctx)
  pici.test.ok(r:find("sess:") ~= nil, "should contain sess: " .. tostring(r))
end)

pici.test.run("handles nil ctx.last gracefully", function()
  local ctx = {turn=0, model="m", tools=0, last=nil, session=nil}
  -- assert no error, returns nil
  local ok, result = pcall(function() return addon.prompt_line(ctx) end)
  pici.test.ok(ok, "should not throw")
  pici.test.ok(result == nil, "nil usage -> nil prompt")
end)
