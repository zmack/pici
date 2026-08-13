-- Intention-level coordination tools backed by the native mailbox bridge.
-- Delivery claims, acknowledgements, leases, and generations stay native.

local mailbox = pici.mailbox
local agents = pici.agents

local function encoded(value)
  return json.encode(value or {})
end

local function invalid(message)
  return nil, "invalid_message: " .. message
end

local function nonempty_string(value, name)
  if type(value) ~= "string" or value == "" then
    return invalid(name .. " must be a non-empty string")
  end
  return value
end

local function target(args)
  if type(args) ~= "table" then
    return invalid("arguments must be an object")
  end
  local has_agent = args.agent_id ~= nil
  local has_session = args.session_id ~= nil
  if has_agent == has_session then
    return invalid("exactly one of agent_id or session_id is required")
  end
  local name = has_agent and "agent_id" or "session_id"
  local value, err = nonempty_string(args[name], name)
  if not value then return nil, err end
  if has_agent then return {agent_id = value} end
  return {session_id = value}
end

local function text(args)
  local value, err = nonempty_string(args.text, "text")
  if not value then return nil, err end
  if #value > 65536 then
    return invalid("text must be at most 65536 bytes")
  end
  return value
end

local function optional_integer(args, name, default, maximum)
  local value = args[name]
  if value == nil then return default end
  if type(value) ~= "number" or value % 1 ~= 0 then
    return invalid(name .. " must be an integer")
  end
  if value < 0 or value > maximum then
    return invalid(name .. " must be between 0 and " .. tostring(maximum))
  end
  return value
end

local function call(fn, args)
  local value, err = fn(args)
  if value == nil then
    return nil, err or "mailbox operation failed"
  end
  return value
end

local function tool(name, description, schema, fn)
  pici.add_tool({
    name = name,
    description = description,
    schema = encoded(schema),
    execute = function(args)
      local value, err = fn(args or {})
      if value == nil then
        return {content = err or "mailbox operation failed", is_error = true}
      end
      -- agents_inbox returns an already serialized value so it can acknowledge
      -- claimed messages only after the model-visible result was encoded.
      if type(value) == "string" then return value end
      return encoded(value)
    end,
  })
end

local target_properties = {
  agent_id = {
    type = "string",
    description = "Live activation ID; use this for one currently running endpoint.",
  },
  session_id = {
    type = "string",
    description = "Durable conversation ID; use this to address a session across activations.",
  },
}

tool("agents_list", "List live root and subagent endpoints visible in this workspace. agent_id identifies a live activation; session_id identifies a durable conversation.", {
  type = "object",
  properties = {
    limit = {type = "integer", minimum = 1, maximum = 100},
  },
  additionalProperties = false,
}, function(args)
  if type(args) ~= "table" then return invalid("arguments must be an object") end
  local limit, err = optional_integer(args, "limit", 100, 100)
  if not limit then return nil, err end
  return call(mailbox.list, {include_self = true, limit = limit})
end)

tool("agents_send", "Send a note or steer message to exactly one endpoint. agent_id targets a live activation; session_id targets a durable conversation.", {
  type = "object",
  properties = {
    agent_id = target_properties.agent_id,
    session_id = target_properties.session_id,
    text = {type = "string", minLength = 1, maxLength = 65536},
    kind = {type = "string", enum = {"note", "steer"}},
    reply_to = {type = "string", minLength = 1},
  },
  required = {"text"},
  additionalProperties = false,
}, function(args)
  local address, err = target(args)
  if not address then return nil, err end
  local body, body_err = text(args)
  if not body then return nil, body_err end
  local kind = args.kind or "note"
  if kind ~= "note" and kind ~= "steer" then
    return invalid("kind must be note or steer")
  end
  local request = {target = address, kind = kind, text = body}
  if args.reply_to ~= nil then
    local reply_to, reply_err = nonempty_string(args.reply_to, "reply_to")
    if not reply_to then return nil, reply_err end
    request.reply_to = reply_to
  end
  return call(mailbox.send, request)
end)

tool("agents_request", "Send a request to exactly one endpoint and wait up to 60 seconds for its correlated reply. A timeout returns a durable request_id. agent_id targets a live activation; session_id targets a durable conversation.", {
  type = "object",
  properties = {
    agent_id = target_properties.agent_id,
    session_id = target_properties.session_id,
    text = {type = "string", minLength = 1, maxLength = 65536},
    timeout_ms = {type = "integer", minimum = 0, maximum = 60000},
  },
  required = {"text"},
  additionalProperties = false,
}, function(args)
  local address, err = target(args)
  if not address then return nil, err end
  local body, body_err = text(args)
  if not body then return nil, body_err end
  local timeout, timeout_err = optional_integer(args, "timeout_ms", 30000, 60000)
  if not timeout then return nil, timeout_err end
  return call(mailbox.request, {target = address, text = body, timeout_ms = timeout})
end)

tool("agents_reply", "Reply to one inbound request by message_id. The original sender activation is preferred, with durable session delivery as a fallback.", {
  type = "object",
  properties = {
    message_id = {type = "string", minLength = 1},
    text = {type = "string", minLength = 1, maxLength = 65536},
  },
  required = {"message_id", "text"},
  additionalProperties = false,
}, function(args)
  local message_id, id_err = nonempty_string(args.message_id, "message_id")
  if not message_id then return nil, id_err end
  local body, body_err = text(args)
  if not body then return nil, body_err end
  return call(mailbox.reply, {message_id = message_id, text = body})
end)

tool("agents_inbox", "Inspect pending notes, requests, and replies. Delivery bookkeeping is handled internally; returned messages include message_id for follow-up replies.", {
  type = "object",
  properties = {
    kinds = {type = "array", items = {type = "string", enum = {"note", "request", "reply"}}},
    limit = {type = "integer", minimum = 1, maximum = 50},
  },
  additionalProperties = false,
}, function(args)
  if type(args) ~= "table" then return invalid("arguments must be an object") end
  local limit, limit_err = optional_integer(args, "limit", 50, 50)
  if not limit then return nil, limit_err end
  local kinds = args.kinds
  if kinds ~= nil then
    if type(kinds) ~= "table" then return invalid("kinds must be an array") end
    for _, kind in ipairs(kinds) do
      if kind ~= "note" and kind ~= "request" and kind ~= "reply" then
        return invalid("kinds contains an unsupported message kind")
      end
    end
  end
  local request = {claim = true, limit = limit}
  if kinds ~= nil then request.kinds = kinds end
  local claimed, claim_err = call(mailbox.inbox, request)
  if not claimed then return nil, claim_err end
  local output = {}
  local claimed_tokens = {}
  for _, message in ipairs(claimed) do
    local visible = {
      message_id = message.message_id,
      sender_agent_id = message.sender_agent_id,
      sender_session_id = message.sender_session_id,
      recipient_session_id = message.recipient_session_id,
      kind = message.kind,
      text = message.text,
      created_at_ms = message.created_at_ms,
    }
    if message.recipient_agent_id ~= nil then
      visible.recipient_agent_id = message.recipient_agent_id
    end
    if message.reply_to ~= nil then visible.reply_to = message.reply_to end
    table.insert(output, visible)
    table.insert(claimed_tokens, {
      message_id = message.message_id,
      claim_token = message.claim_token,
    })
  end
  local serialized = json.encode(output)
  for _, claimed_message in ipairs(claimed_tokens) do
    local _, ack_err = call(mailbox.ack, claimed_message)
    if ack_err then break end
  end
  return serialized
end)

tool("agents_close", "Close one locally owned subagent and its descendants. Roots, remote process endpoints, and unknown IDs are rejected; this never closes a remote agent.", {
  type = "object",
  properties = {
    agent_id = {
      type = "string",
      minLength = 1,
      description = "Live local subagent activation ID, not a durable session_id.",
    },
  },
  required = {"agent_id"},
  additionalProperties = false,
}, function(args)
  local agent_id, id_err = nonempty_string(args.agent_id, "agent_id")
  if not agent_id then return nil, id_err end
  local self, self_err = call(mailbox.self, {})
  if not self then return nil, self_err end
  local listed, list_err = call(mailbox.list, {include_self = true, limit = 100})
  if not listed then return nil, list_err end
  local target_agent
  for _, agent in ipairs(listed) do
    if agent.agent_id == agent_id then target_agent = agent; break end
  end
  if not target_agent then return invalid("agent_id is unknown or not live") end
  if target_agent.agent_id == self.agent_id or target_agent.kind == "root" then
    return invalid("agents_close may only close a locally owned subagent")
  end
  if target_agent.kind ~= "subagent" or target_agent.process_id ~= self.process_id or
      target_agent.owner_agent_id == nil then
    return invalid("agents_close may only close a locally owned subagent")
  end
  if not target_agent.task_id or target_agent.task_id == "" then
    return invalid("agent_id has no local task endpoint")
  end
  return call(agents.close, target_agent.task_id)
end)

return {}
