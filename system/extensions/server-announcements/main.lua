-- Optional package: installing files alone does not enable announcements.
extensions.command("extannounce", {permission = "announce", cooldown_ms = 5000,
  help = "Send a plain-text server-wide announcement.", arguments = {
    {name = "message", type = "text", max_bytes = 200}
  }}, function(ctx, raw, args)
  local ok, reason = extensions.announce(args.message)
  extensions.reply(ok and "Announcement accepted." or "Announcement refused: " .. reason)
end)

extensions.on("game_opened", function(info)
  local message = extensions.setting("game_opened_message")
  -- Empty setting disables this automatic message; manual announcements remain.
  if message ~= "" then
    local ok, reason = extensions.announce(message)
    if not ok then extensions.log("Game announcement skipped: " .. reason) end
  end
end)
