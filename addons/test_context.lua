-- Tests for context.lua via the built-in pici --test harness.
local addon = dofile("addons/context.lua")

local function make_context()
  local raw = {
    system_prompt = "system prompt",
    model = {
      id = "test-model", provider = "test", api = "test-api",
      baseUrl = "http://example.test",
    },
    messages = {
      {
        index = 1, role = "user",
        content = {{type = "text", text = "hello"}},
      },
      {
        index = 2, role = "assistant", stopReason = "toolUse",
        content = {
          {type = "thinking", thinking = "planning"},
          {type = "toolCall", id = "call_1", name = "search",
           arguments = {query = "pici"}},
        },
      },
      {
        index = 3, role = "toolResult", toolCallId = "call_1",
        toolName = "search", isError = false,
        content = {{type = "text", text = "result"}},
      },
    },
    tools = {
      {
        name = "search", description = "Searches things",
        source_path = "test.lua", schema_valid = true,
        input_schema = {
          type = "object",
          properties = {query = {type = "string"}},
        },
      },
    },
  }
  return {raw = raw, effective = {available = true}}
end

local function invoke(args, context)
  local result = addon.on_command("context", args, context.raw.messages, context)
  pici.test.ok(result ~= nil and result.handled, "command should be handled")
  pici.test.ok(result.output ~= nil, "command should return output")
  return result.output
end

pici.test.run("genome shows message shape", function()
  local output = invoke("genome", make_context())
  pici.test.ok(output:find("Prompt genome") ~= nil)
  pici.test.ok(output:find("user") ~= nil)
  pici.test.ok(output:find("assistant") ~= nil)
  pici.test.ok(output:find("toolResult") ~= nil)
end)

pici.test.run("heatmap shows density columns", function()
  local output = invoke("heatmap", make_context())
  pici.test.ok(output:find("Context heatmap") ~= nil)
  pici.test.ok(output:find("Text") ~= nil)
  pici.test.ok(output:find("Think") ~= nil)
  pici.test.ok(output:find("#") ~= nil)
end)

pici.test.run("tools shows calls and registered schemas", function()
  local output = invoke("tools", make_context())
  pici.test.ok(output:find("Tool%-call graph") ~= nil)
  pici.test.ok(output:find("call_1") ~= nil)
  pici.test.ok(output:find("result: ok") ~= nil)
  pici.test.ok(output:find("Registered tools: 1") ~= nil)
  pici.test.ok(output:find("query:string") ~= nil)
end)

pici.test.run("effective view reports unavailable", function()
  local context = make_context()
  context.effective = {available = false}
  local output = invoke("effective heatmap", context)
  pici.test.ok(output:find("not available") ~= nil)
end)

pici.test.run("effective visualization selects effective view", function()
  local context = make_context()
  context.effective = context.raw
  context.effective.available = true
  local output = invoke("effective genome", context)
  pici.test.ok(output:find("Prompt genome") ~= nil)
  pici.test.ok(output:find("System prompt:") ~= nil)
end)

pici.test.run("default mode remains JSON", function()
  local decoded = json.decode(invoke("", make_context()))
  pici.test.eq(decoded.messages[2].content[2].type, "toolCall")
  pici.test.eq(decoded.messages[3].toolCallId, "call_1")
end)

pici.test.run("unknown mode returns usage", function()
  local output = invoke("unknown", make_context())
  pici.test.ok(output:find("usage: /context") ~= nil)
end)
