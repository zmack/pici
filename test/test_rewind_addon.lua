-- Tests for the rewind add-on pattern.
-- The add-on is defined inline here since we don't have a rewind.lua yet.
-- In real use: local addon = dofile("rewind.lua")

local snapshots = {}

local addon = {
  before_tool_call = function(ctx)
    -- record a fake git hash per turn
    snapshots[ctx.turn] = "sha-" .. ctx.turn
  end,

  on_command = function(cmd, args, transcript)
    if cmd ~= "rewind" then return {handled=false} end

    local n = tonumber(args) or 1
    local hash = snapshots[n]
    if not hash then
      return {handled=true}
    end

    -- find message index for end of turn n
    local turns, idx = 0, 0
    for i, msg in ipairs(transcript) do
      if msg.role == "assistant" then
        turns = turns + 1
        if turns >= n then idx = i; break end
      end
    end

    -- in a real add-on: os.execute("git reset --hard " .. hash)
    return {handled=true, truncate_to=idx}
  end,
}

-- ── helpers ────────────────────────────────────────────────────────────────

local function make_transcript(n_turns)
  local t = {}
  for i = 1, n_turns do
    table.insert(t, {role="user",      content="msg "..i, index=#t+1})
    table.insert(t, {role="assistant", content="resp "..i, index=#t+1, turn=i})
  end
  return t
end

-- ── tests ──────────────────────────────────────────────────────────────────

pici.test.run("before_tool_call records snapshot per turn", function()
  snapshots = {}
  local ctx = {turn=1, tool_name="bash", call_id="x",
               args={command="ls"}}
  addon.before_tool_call(ctx)
  pici.test.eq(snapshots[1], "sha-1")
end)

pici.test.run("rewind to turn 1 truncates correctly", function()
  snapshots = {[1]="sha-1", [2]="sha-2"}
  local t = make_transcript(2)  -- 4 messages: u1 a1 u2 a2
  local r = addon.on_command("rewind", "1", t)
  pici.test.ok(r.handled, "should be handled")
  pici.test.eq(r.truncate_to, 2, "keep through first assistant (index 2)")
end)

pici.test.run("rewind to turn 2 keeps all 4 messages", function()
  snapshots = {[1]="sha-1", [2]="sha-2"}
  local t = make_transcript(2)
  local r = addon.on_command("rewind", "2", t)
  pici.test.ok(r.handled)
  pici.test.eq(r.truncate_to, 4)
end)

pici.test.run("rewind with missing snapshot still handles", function()
  snapshots = {}
  local t = make_transcript(1)
  local r = addon.on_command("rewind", "1", t)
  pici.test.ok(r.handled, "handled even without snapshot")
  pici.test.ok(not r.truncate_to, "no truncate_to when no snapshot")
end)

pici.test.run("unknown command falls through", function()
  local r = addon.on_command("help", "", {})
  pici.test.ok(not r.handled)
end)

pici.test.run("pici.mock_run_agent replaces run_agent", function()
  pici.mock_run_agent(function(cfg)
    return {text="mock:" .. cfg.prompt, error=nil}
  end)
  local r = pici.run_agent({prompt="test"})
  pici.test.eq(r.text, "mock:test")
  -- restore stub
  pici.mock_run_agent(function() return {text="", error="not available"} end)
end)
