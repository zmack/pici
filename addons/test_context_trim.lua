local addon = dofile("addons/context_trim.lua")

local function messages(count)
  local result = {}
  for i = 1, count do
    table.insert(result, {role = "user", content = {{type = "text", text = tostring(i)}}})
  end
  return result
end

pici.test.run("does not trim below the threshold", function()
  local result = addon.prepare_context({
    context_window = 1000,
    estimated_tokens = 100,
    messages = messages(20),
  })
  pici.test.ok(result == nil)
end)

pici.test.run("keeps the first and recent messages", function()
  local result = addon.prepare_context({
    context_window = 1000,
    estimated_tokens = 900,
    messages = messages(20),
  })
  pici.test.ok(result ~= nil)
  pici.test.eq(#result.messages, 11)
  pici.test.eq(result.messages[1].content[1].text, "1")
  pici.test.eq(result.messages[11].content[1].text, "20")
end)
