-- Server preparation and client loading observations, not quest completion.
extensions.on("quest_assigned", function(info)
  local count = tonumber(extensions.storage_get("assigned") or "0") or 0
  extensions.storage_set("assigned", tostring(count + 1))
  extensions.storage_set("last-assigned-quest", tostring(info.quest_number))
  extensions.log("Game " .. info.lobby_id .. " assigned quest " .. info.quest_number)
end)

extensions.on("quest_join_acknowledged", function(info)
  local count = tonumber(extensions.storage_get("joins-acknowledged") or "0") or 0
  extensions.storage_set("joins-acknowledged", tostring(count + 1))
  extensions.storage_set("last-join-quest", tostring(info.quest_number))
  extensions.log("Client " .. info.client_id .. " acknowledged joining quest " .. info.quest_number .. " (" .. info.reason .. ")")
end)

extensions.on("quest_load_acknowledged", function(info)
  local count = tonumber(extensions.storage_get("loads-acknowledged") or "0") or 0
  extensions.storage_set("loads-acknowledged", tostring(count + 1))
  extensions.storage_set("last-acknowledged-quest", tostring(info.quest_number))
  extensions.log("Client " .. info.client_id .. " acknowledged quest " .. info.quest_number .. " in game " .. info.lobby_id)
end)

extensions.on("quest_map_loaded", function(info)
  local count = tonumber(extensions.storage_get("maps-loaded") or "0") or 0
  extensions.storage_set("maps-loaded", tostring(count + 1))
  extensions.storage_set("last-map-quest", tostring(info.quest_number))
  extensions.log("Game " .. info.lobby_id .. " prepared quest map " .. info.quest_number)
end)
