return {
  prompt_line = function(ctx)
    local last = ctx.last
    if not last or (last.total_tokens == 0 and last.cost.total == 0) then
      return nil
    end

    local C = {
      reset = "\27[0m",
      bold = "\27[1m",
      dim = "\27[2m",
      gray = "\27[90m",
      green = "\27[32m",
      yellow = "\27[33m",
      cyan = "\27[36m",
      red = "\27[31m",
    }

    local function fmt_tok(n)
      if n >= 1000000 then
        return string.format("%.1fM", n / 1000000)
      elseif n >= 1000 then
        return string.format("%.1fK", n / 1000)
      else
        return tostring(n)
      end
    end

    local function fmt_cost(c)
      if c == 0 then
        return nil
      elseif c < 0.01 then
        return string.format("$%.4g", c)
      elseif c < 1.0 then
        return string.format("$%.4f", c)
      else
        return string.format("$%.2f", c)
      end
    end

    local function cost_color(c)
      if c < 0.01 then
        return C.green
      elseif c < 0.10 then
        return C.yellow
      else
        return C.red
      end
    end

    local parts = {}
    local ct = fmt_cost(last.cost.total)
    if ct then
      table.insert(parts, C.bold .. cost_color(last.cost.total) .. ct .. C.reset)
    elseif last.total_tokens > 0 then
      table.insert(parts, C.bold .. C.cyan .. fmt_tok(last.total_tokens) .. " tok" .. C.reset)
    else
      table.insert(parts, C.bold .. C.cyan .. fmt_tok(last.input) .. "→" .. fmt_tok(last.output) .. C.reset)
    end

    if last.cache_read > 0 then
      table.insert(parts, C.yellow .. "cache:" .. fmt_tok(last.cache_read) .. C.reset)
    end

    if ctx.session and ctx.session.cost.total > 0 then
      local st = fmt_cost(ctx.session.cost.total)
      if st then
        table.insert(parts, C.gray .. "sess:" .. st .. C.reset)
      end
    end

    return C.gray .. "[" .. C.reset .. table.concat(parts, " ") .. C.gray .. "]" .. C.reset .. " > "
  end,
}
