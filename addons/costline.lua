return {
  prompt_line = function(ctx)
    local last = ctx.last
    if not last or (last.total_tokens == 0 and last.cost.total == 0) then
      return nil
    end

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

    local parts = {}
    local ct = fmt_cost(last.cost.total)
    if ct then
      table.insert(parts, ct)
    elseif last.total_tokens > 0 then
      table.insert(parts, fmt_tok(last.total_tokens) .. " tok")
    else
      table.insert(parts, fmt_tok(last.input) .. "→" .. fmt_tok(last.output))
    end

    if last.cache_read > 0 then
      table.insert(parts, "cache:" .. fmt_tok(last.cache_read))
    end

    if ctx.session and ctx.session.cost.total > 0 then
      local st = fmt_cost(ctx.session.cost.total)
      if st then
        table.insert(parts, "sess:" .. st)
      end
    end

    return "[" .. table.concat(parts, " ") .. "] > "
  end,
}
