-- Core-backed asynchronous child-agent orchestration.
-- Load explicitly with --hooks-file or from an add-on directory.

local agents = pici.agents

local function encoded(value)
  return json.encode(value or {})
end

local function tool(name, description, schema, fn)
  pici.add_tool({
    name = name,
    description = description,
    schema = encoded(schema),
    execute = function(args)
      local value, err = fn(args or {})
      if not value then
        return {content = err or "agent operation failed", is_error = true}
      end
      return {content = encoded(value)}
    end,
  })
end

tool("spawn_agent", "Start a child agent. Children are read-only by default; write tools require agents.write_tools = core/all and explicit allow_write_tools. There is a fixed cap on live direct children per parent; once it's hit, spawn_agent fails with 'parent child limit reached (N/limit)' until you wait_agent or close_agent an existing child to free a slot -- don't just retry with a new task_name.", {
  type = "object",
  properties = {
    task_name = {type = "string"},
    message = {type = "string"},
    context = {type = "object"},
    model = {type = "string"},
    tools = {type = "array", items = {type = "string"}},
    allow_write_tools = {type = "boolean"},
  },
  required = {"task_name", "message"},
  additionalProperties = false,
}, function(args)
  return agents.spawn(args)
end)

tool("list_agents", "List the root task and retained child-agent statuses.", {
  type = "object", properties = {path_prefix = {type = "string"}},
  additionalProperties = false,
}, function(args)
  return agents.list(args)
end)

tool("wait_agent", "Wait for a child status change with a bounded timeout.", {
  type = "object",
  properties = {
    targets = {type = "array", items = {type = "string"}, description = "Child agent ids to wait on (also accepted as 'ids')"},
    ids = {type = "array", items = {type = "string"}},
    after_generation = {type = "integer"},
    timeout_ms = {type = "integer"},
  },
  additionalProperties = false,
}, function(args)
  local targets = args.targets or args.ids
  if not targets then
    return nil, "wait_agent requires targets: [child agent ids]"
  end
  args.targets = targets
  args.ids = nil
  return agents.wait(args)
end)

tool("send_agent_message", "Queue information for a child without starting a new turn.", {
  type = "object", properties = {target = {type = "string"}, message = {type = "string"}},
  required = {"target", "message"}, additionalProperties = false,
}, function(args)
  return agents.send(args.target, args.message)
end)

tool("follow_up_agent", "Queue a serialized follow-up turn for an idle or running child.", {
  type = "object", properties = {target = {type = "string"}, message = {type = "string"}},
  required = {"target", "message"}, additionalProperties = false,
}, function(args)
  return agents.follow_up(args.target, args.message)
end)

tool("interrupt_agent", "Interrupt a child turn while preserving its transcript and identity.", {
  type = "object", properties = {target = {type = "string"}, reason = {type = "string"}},
  required = {"target"}, additionalProperties = false,
}, function(args)
  return agents.interrupt(args.target, args.reason or "parent")
end)

tool("close_agent", "Close a child and all of its descendants permanently.", {
  type = "object", properties = {target = {type = "string"}},
  required = {"target"}, additionalProperties = false,
}, function(args)
  return agents.close(args.target)
end)

local function list_output()
  local value, err = agents.list({})
  if not value then return err end
  local lines = {}
  for _, item in ipairs(value) do
    local ctx = ""
    if item.context then
      local window = item.context.context_window
      if window and window > 0 then
        ctx = string.format("  ctx=%.1fk/%.0fk (%d%%)",
          item.context.total_tokens / 1024, window / 1024,
          math.floor(100 * item.context.total_tokens /
            math.max(window, 1)))
      else
        ctx = string.format("  ctx=%dk", item.context.total_tokens // 1024)
      end
    end
    table.insert(lines, string.format("%s  %s  %s%s", item.id, item.status,
      item.task_path, ctx))
  end
  return #lines > 0 and table.concat(lines, "\n") or "no agents"
end

return {
  commands = {
    {name = "agents", description = "List child agents"},
    {name = "agent", description = "Inspect an agent", args_hint = "<id-or-path>"},
    {name = "delegate", description = "Spawn a child agent", args_hint = "<name> <prompt>"},
    {name = "interrupt", description = "Interrupt a child agent", args_hint = "<id-or-path>"},
    {name = "close-agent", description = "Close a child agent", args_hint = "<id-or-path>"},
  },

  on_command = function(command, args)
    if command == "agents" then
      return {handled = true, output = list_output()}
    elseif command == "agent" then
      local value, err = agents.get(args)
      return {handled = true, output = value and encoded(value) or err}
    elseif command == "delegate" then
      local name, prompt = args:match("^(%S+)%s+(.+)$")
      if not name or not prompt then
        return {handled = true, output = "usage: /delegate <name> <prompt>"}
      end
      local value, err = agents.spawn({task_name = name, message = prompt})
      return {handled = true, output = value and encoded(value) or err}
    elseif command == "interrupt" then
      local value, err = agents.interrupt(args, "user")
      return {handled = true, output = value and encoded(value) or err}
    elseif command == "close-agent" then
      local value, err = agents.close(args)
      return {handled = true, output = value and encoded(value) or err}
    end
    return {handled = false}
  end,
}
