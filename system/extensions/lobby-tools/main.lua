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

local function tenth(value)
  return string.format("%.1f", value)
end

-- $xyz: where you stand, for placing things (the concert's x, y, z and lobby settings). The server
-- knows a player's height once they stop walking; facing is not tracked.
extensions.command("xyz", {help = "Show your X, Y, Z position and lobby (for concert and scene setup).",
  cooldown_ms = 2000, arguments = {}
}, function()
  local here = extensions.position()
  if not here then
    extensions.reply("Your position is unavailable.")
    return
  end
  extensions.reply("X " .. tenth(here.x) .. "  Y " .. tenth(here.y) .. "  Z " .. tenth(here.z))
  if here.lobby_number then
    extensions.reply("Lobby " .. here.lobby_name .. " (number " .. here.lobby_number .. ")")
  elseif here.is_game then
    extensions.reply("In a game, floor " .. here.floor)
  end
end)

