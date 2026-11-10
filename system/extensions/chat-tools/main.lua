-- These use the server's configured prefix: $shout by default, /shout if changed.
-- Version zero includes existing stores created before package data versioning.
extensions.migrate(function(old_version, target_version)
  assert(old_version == 0 and target_version == 1)
  if extensions.storage_get("total-visits") == nil then
    extensions.storage_set("total-visits", "0")
  end
end)

extensions.command("shout", {help = "Broadcast a short message in your current lobby or game.", cooldown_ms = extensions.setting("shout_cooldown_ms"), arguments = {
  {name = "message", type = "text", max_bytes = 350}
}}, function(ctx, raw, args)
  extensions.broadcast(ctx.player_name .. ": " .. args.message)
end)

-- IDs refer to connected sessions, not Guild Card numbers. $extid shows yours.
extensions.command("extid", {cooldown_ms = 1000}, function(ctx)
  extensions.reply("Your extension client ID: " .. ctx.client_id)
end)

extensions.command("whisper", {help = "Whisper to a connected session in the same lobby or game.", cooldown_ms = 1000, arguments = {
  {name = "target", type = "client_id", max_bytes = 20},
  {name = "message", type = "text", max_bytes = 300}
}}, function(ctx, raw, args)
  local sent = extensions.whisper(args.target,
    "[Whisper from " .. ctx.player_name .. "] " .. args.message)
  extensions.reply(sent and "Whisper sent." or "Client ID not found in this lobby.")
end)

extensions.command("remind", {cooldown_ms = 1000}, function()
  local timer = extensions.after(5000, function()
    extensions.reply("Your five-second reminder.")
  end)
  extensions.reply(timer and "Reminder scheduled; leaving the lobby cancels it." or "Too many reminders pending; try again later.")
end)

extensions.command("repeatremind", {cooldown_ms = 1000,
  help = "Schedule a few repeating reminders; leaving the lobby cancels them.", arguments = {
    {name = "count", type = "integer", min = 1, max = 5},
    {name = "seconds", type = "integer", min = 1, max = 60}
}}, function(ctx, raw, args)
  local sent = 0
  local timer
  timer = extensions.every(args.seconds * 1000, function()
    sent = sent + 1
    extensions.reply("Repeating reminder " .. sent .. "/" .. args.count .. ".")
    if sent >= args.count then extensions.cancel(timer) end
  end)
  extensions.reply(timer and "Repeating reminders scheduled; leaving the lobby cancels them." or "Too many reminders pending; try again later.")
end)

extensions.on("lobby_join", function(ctx)
  local count = tonumber(extensions.storage_get("total-visits") or "0") or 0
  extensions.storage_set("total-visits", tostring(count + 1))
  -- No welcome packet is sent automatically; enable your own reply if desired.
end)

extensions.command("extvisits", {permission = "announce", cooldown_ms = 1000}, function()
  extensions.reply("Lobby joins counted by this extension: " .. (extensions.storage_get("total-visits") or "0"))
end)
