-- A shared decoration is identified by (package, lobby, key). Repeating a join
-- updates the same logical object; it does not consume another native slot.
extensions.on("lobby_join", function()
  if extensions.scene_capabilities().native_ep3_media then
    extensions.scene_spawn("welcome", "checker", "teleporter-west")
  end
end)

extensions.command("decor", {permission = "change-event", cooldown_ms = 1000}, function(_, args)
  local ok, reason
  if args == "remove" then
    ok, reason = extensions.scene_remove("welcome")
  elseif args == "show" then
    ok, reason = extensions.scene_spawn("welcome", "checker", "teleporter-west")
  else
    extensions.reply("Usage: decor show|remove (retail Episode 3 lobby)")
    return
  end
  extensions.reply(ok and "Decoration updated." or reason)
end)

extensions.command("decorcheck", {permission = "change-event", cooldown_ms = 1000,
  help = "Check this package's asset support for your client and current scene.",
  arguments = {{name = "asset", type = "word", max_bytes = 64}}
}, function(ctx, raw, args)
  local support = extensions.scene_asset_support(args.asset)
  if support.supported then
    extensions.reply("Asset supported (" .. support.variant .. "); placement and rendering are unconfirmed.")
  else
    extensions.reply("Asset unavailable: " .. support.reason)
  end
end)

-- Expiration is owned by the registry, so it still happens after the caller leaves.
extensions.command("decortimed", {permission = "change-event", cooldown_ms = 1000,
  help = "Show the welcome decoration for a bounded number of seconds.",
  arguments = {{name = "seconds", type = "integer", min = 1, max = 3600}}
}, function(ctx, raw, args)
  local ok, reason = extensions.scene_spawn("welcome", "checker", "teleporter-west", args.seconds * 1000)
  extensions.reply(ok and "Temporary decoration updated." or reason)
end)

extensions.command("season", {permission = "change-event", cooldown_ms = 1000}, function(_, args)
  local event = nil
  if args ~= "release" then
    event = tonumber(args)
    if event == nil or event % 1 ~= 0 or event < 0 or event > 14 then
      extensions.reply("Usage: season 0..14|release (event 2 is unused)")
      return
    end
  end
  local ok, reason = extensions.scene_event(event)
  extensions.reply(ok and "Lobby event updated." or reason)
end)
