-- Use the configured prefix: $exthelp, $exthelp 2, or $extusage shout.
extensions.command("exthelp", {cooldown_ms = 1000,
  help = "List available extension commands, six per page.",
  arguments = {{name = "page", type = "integer", optional = true, min = 1, max = 86}}
}, function(ctx, raw, args)
  local commands = extensions.commands()
  if commands == nil then
    extensions.reply("Extension command information is unavailable.")
    return
  end
  local page = args.page or 1
  local pages = math.max(1, math.ceil(#commands / 6))
  if page < 1 or page % 1 ~= 0 or page > pages then
    extensions.reply("Choose a page from 1 to " .. pages .. ".")
    return
  end
  extensions.reply("Extension commands (page " .. page .. "/" .. pages .. "). Use extusage <command> for usage.")
  if #commands == 0 then extensions.reply("No extension commands are available.") end
  for index = (page - 1) * 6 + 1, math.min(page * 6, #commands) do
    extensions.reply(commands[index].name)
  end
end)

extensions.command("extusage", {cooldown_ms = 1000,
  help = "Show usage and help for an available extension command.",
  arguments = {{name = "command", type = "word", max_bytes = 64}}
}, function(ctx, raw, args)
  local commands = extensions.commands()
  if commands == nil then
    extensions.reply("Extension command information is unavailable.")
    return
  end
  for _, command in ipairs(commands) do
    if command.name == args.command then
      extensions.reply(command.usage)
      if command.help ~= "" then extensions.reply(command.help) end
      return
    end
  end
  extensions.reply("Extension command not found or unavailable.")
end)
