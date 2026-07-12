-- Diagnostic command for inspecting the raw session or last effective context.
-- pici.log writes to stderr. Context dumps can contain sensitive prompts,
-- reasoning data, and large base64-encoded images.
return {
  commands = {
    {name = "context", description = "Dump raw or effective model context",
     args_hint = "[effective]"},
  },

  on_command = function(cmd, args, transcript, context)
    if cmd ~= "context" then return nil end

    local view = args == "effective" and context.effective or context.raw
    if args == "effective" and not view.available then
      return {
        handled = true,
        output = "effective context is not available until a model request is prepared",
      }
    end

    return {handled = true, output = json.encode(view)}
  end,
}
