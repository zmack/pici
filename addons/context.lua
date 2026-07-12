-- Static context visualizations. pici.log writes diagnostics to stderr;
-- command output is rendered in the active TUI by returning `output`.
-- These views intentionally show image/reasoning sizes and metadata rather
-- than dumping large image payloads into the terminal.

local function format_bytes(n)
  if n >= 1024 * 1024 then
    return string.format("%.1f MiB", n / (1024 * 1024))
  elseif n >= 1024 then
    return string.format("%.1f KiB", n / 1024)
  end
  return tostring(n) .. " B"
end

local function one_line(value)
  return tostring(value or ""):gsub("%s+", " ")
end

local function sorted_keys(table_value)
  local keys = {}
  for key in pairs(table_value or {}) do
    table.insert(keys, key)
  end
  table.sort(keys)
  return keys
end

local function block_kinds(message)
  local kinds = {}
  for _, block in ipairs(message.content or {}) do
    local kind = block.type or "other"
    if kind == "toolCall" then kind = "call" end
    table.insert(kinds, kind)
  end
  if #kinds == 0 and message.role == "toolResult" then
    table.insert(kinds, "result")
  end
  return table.concat(kinds, ",")
end

local function message_stats(message)
  local stats = {text = 0, thinking = 0, image = 0, tools = 0, total = 0}
  for _, block in ipairs(message.content or {}) do
    local size = #json.encode(block)
    if block.type == "text" then
      stats.text = stats.text + size
    elseif block.type == "thinking" then
      stats.thinking = stats.thinking + size
    elseif block.type == "image" then
      stats.image = stats.image + size
    elseif block.type == "toolCall" then
      stats.tools = stats.tools + size
    end
  end
  if message.role == "toolResult" then
    stats.tools = stats.tools + #json.encode(message)
  end
  stats.total = #json.encode(message)
  return stats
end

local function bar(value, maximum, width)
  if value <= 0 or maximum <= 0 then
    return string.rep(" ", width)
  end
  local count = math.max(1, math.floor(value / maximum * width + 0.5))
  count = math.min(width, count)
  return string.rep("#", count) .. string.rep(" ", width - count)
end

local function render_genome(view)
  local messages = view.messages or {}
  local maximum = 1
  local stats = {}
  for _, message in ipairs(messages) do
    local current = message_stats(message)
    table.insert(stats, current)
    maximum = math.max(maximum, current.total)
  end

  local segments = {}
  for i, message in ipairs(messages) do
    local glyph = "?"
    if message.role == "user" then glyph = "U" end
    if message.role == "assistant" then glyph = "A" end
    if message.role == "toolResult" then glyph = "R" end
    local width = math.max(1, math.floor(stats[i].total / maximum * 18 + 0.5))
    table.insert(segments, string.rep(glyph, width))
  end

  local lines = {
    "Prompt genome",
    "System prompt: " .. format_bytes(#(view.system_prompt or "")),
    table.concat(segments, "|"),
    "",
    "Legend: U=user  A=assistant  R=tool result",
    "Segment width is relative to the largest message",
    "",
  }
  for i, message in ipairs(messages) do
    table.insert(lines, string.format("%2d  %-10s %-28s %s", i,
      message.role or "?", block_kinds(message), format_bytes(stats[i].total)))
  end
  return table.concat(lines, "\n")
end

local function render_heatmap(view)
  local messages = view.messages or {}
  local rows = {}
  local maxima = {text = 1, thinking = 1, image = 1, tools = 1}
  for _, message in ipairs(messages) do
    local stats = message_stats(message)
    table.insert(rows, {message = message, stats = stats})
    maxima.text = math.max(maxima.text, stats.text)
    maxima.thinking = math.max(maxima.thinking, stats.thinking)
    maxima.image = math.max(maxima.image, stats.image)
    maxima.tools = math.max(maxima.tools, stats.tools)
  end

  local lines = {
    "Context heatmap (each bar is relative to its column maximum)",
    "#   Role       Text       Think      Image      Tools      Total",
  }
  for i, row in ipairs(rows) do
    local stats = row.stats
    table.insert(lines, string.format("%2d  %-10s %-10s %-10s %-10s %-10s %s",
      i, row.message.role or "?", bar(stats.text, maxima.text, 8),
      bar(stats.thinking, maxima.thinking, 8),
      bar(stats.image, maxima.image, 8),
      bar(stats.tools, maxima.tools, 8), format_bytes(stats.total)))
  end
  table.insert(lines, "")
  table.insert(lines, "# = content density; image and thinking bytes are included")
  return table.concat(lines, "\n")
end

local function tool_result_index(messages)
  local results = {}
  for _, message in ipairs(messages) do
    if message.role == "toolResult" and message.toolCallId then
      results[message.toolCallId] = message
    end
  end
  return results
end

local function render_tools(view)
  local messages = view.messages or {}
  local results = tool_result_index(messages)
  local lines = {"Tool-call graph", ""}
  local calls = 0

  for _, message in ipairs(messages) do
    if message.role == "assistant" then
      local assistant_calls = {}
      for _, block in ipairs(message.content or {}) do
        if block.type == "toolCall" then
          table.insert(assistant_calls, block)
        end
      end
      if #assistant_calls > 0 then
        table.insert(lines, "assistant (message " .. tostring(message.index or "?") .. ")")
        for i, call in ipairs(assistant_calls) do
          calls = calls + 1
          local branch = i == #assistant_calls and "`-- " or "|-- "
          local arguments = one_line(json.encode(call.arguments or {}))
          if #arguments > 64 then arguments = arguments:sub(1, 61) .. "..." end
          table.insert(lines, branch .. (call.id or "?") .. "  " ..
            (call.name or "?") .. "(" .. arguments .. ")")
          local result = results[call.id]
          if result then
            local status = result.isError and "error" or "ok"
            local result_branch = i == #assistant_calls and "    " or "|   "
            table.insert(lines, result_branch .. "`-- result: " .. status ..
              ", " .. format_bytes(#json.encode(result)))
          else
            local result_branch = i == #assistant_calls and "    " or "|   "
            table.insert(lines, result_branch .. "`-- result: unavailable")
          end
        end
        table.insert(lines, "")
      end
    end
  end

  if calls == 0 then
    table.insert(lines, "(no tool calls in this context)")
  else
    table.insert(lines, tostring(calls) .. " tool call(s)")
  end

  table.insert(lines, "")
  table.insert(lines, "Registered tools: " .. tostring(#(view.tools or {})))
  for i, tool in ipairs(view.tools or {}) do
    local schema = tool.input_schema or {}
    local properties = {}
    for _, key in ipairs(sorted_keys(schema.properties)) do
      local property = schema.properties[key]
      table.insert(properties, key .. ":" .. tostring(property.type or "?"))
    end
    local schema_text = #properties > 0 and table.concat(properties, ", ") or
      tostring(schema.type or "object")
    if not tool.schema_valid then schema_text = "invalid schema" end
    table.insert(lines, string.format("%2d. %-18s %s", i,
      one_line(tool.name), schema_text))
  end
  return table.concat(lines, "\n")
end

local function select_view(args, context)
  local mode = (args or ""):gsub("^%s+", ""):gsub("%s+$", "")
  local view = context.raw
  local wants_effective = mode == "effective" or mode:sub(1, 10) == "effective "
  if wants_effective then
    view = context.effective
    if mode == "effective" then
      mode = "json"
    else
      mode = mode:sub(11)
    end
  elseif mode == "" then
    mode = "json"
  end
  return mode, view, wants_effective
end

return {
  commands = {
    {name = "context", description = "Inspect raw or effective model context",
     args_hint = "[effective|genome|heatmap|tools]"},
  },

  on_command = function(cmd, args, transcript, context)
    if cmd ~= "context" then return nil end

    local mode, view, wants_effective = select_view(args, context)
    if wants_effective and not view.available then
      return {
        handled = true,
        output = "effective context is not available until a model request is prepared",
      }
    end

    local output
    if mode == "genome" then
      output = render_genome(view)
    elseif mode == "heatmap" then
      output = render_heatmap(view)
    elseif mode == "tools" then
      output = render_tools(view)
    elseif mode == "json" then
      output = json.encode(view)
    else
      output = "usage: /context [effective] [genome|heatmap|tools]"
    end
    return {handled = true, output = output}
  end,
}
