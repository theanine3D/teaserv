-- Session IDs here can be passed to chat-tools' whisper command.
-- The server's configured prefix applies: $who by default, /who if changed.
extensions.command("who", {help = "List connected players and session IDs in your lobby or game.",
  cooldown_ms = 5000, arguments = {}
}, function()
  local lobby = extensions.lobby()
  if not lobby then
    extensions.reply("Lobby information is unavailable.")
    return
  end
  extensions.reply(lobby.name .. " (" .. #lobby.players .. "/" .. lobby.capacity .. ")")
  for _, player in ipairs(lobby.players) do
    -- Put the complete ID first, ahead of any legacy message truncation.
    extensions.reply(player.client_id .. ": " .. player.player_name ..
      " [" .. player.client_version .. "]" .. (player.is_self and " (you)" or ""))
  end
end)
