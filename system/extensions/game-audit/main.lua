-- Primary game observations; join acknowledgement is not whole-game or quest readiness.
extensions.on("game_opened", function(info)
  local count = tonumber(extensions.storage_get("opened") or "0") or 0
  extensions.storage_set("opened", tostring(count + 1))
  extensions.log("Game " .. info.lobby_id .. " opened: " .. info.name)
end)

extensions.on("game_closed", function(info)
  local count = tonumber(extensions.storage_get("closed") or "0") or 0
  extensions.storage_set("closed", tostring(count + 1))
  extensions.log("Game " .. info.lobby_id .. " removed: " .. info.name)
end)

extensions.on("game_join_ready", function(ctx)
  local count = tonumber(extensions.storage_get("joins-ready") or "0") or 0
  extensions.storage_set("joins-ready", tostring(count + 1))
  extensions.log("Client " .. ctx.client_id .. " acknowledged joining game " .. ctx.lobby_id)
end)
