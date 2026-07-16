-- Request-local context trimming example.
-- This intentionally prunes instead of summarizing: durable transcript state
-- remains owned by the core session journal.

local threshold = 0.82
local tail_messages = 10

return {
  prepare_context = function(ctx)
    local messages = ctx.messages or {}
    local limit = ctx.context_window or 0
    if limit <= 0 or (ctx.estimated_tokens or 0) < limit * threshold then
      return nil
    end
    if #messages <= tail_messages + 1 then
      return nil
    end

    local kept = {messages[1]}
    local first_tail = math.max(2, #messages - tail_messages + 1)
    for i = first_tail, #messages do
      table.insert(kept, messages[i])
    end
    return {messages = kept}
  end,
}
