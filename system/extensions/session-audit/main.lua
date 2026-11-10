-- Optional local-session telemetry. These are observations, not online counts:
-- reloads, proxy handoffs, and shutdowns can leave unmatched notifications.
extensions.migrate(function(old_version, target_version)
  assert(old_version == 0 and target_version == 1)
  for _, key in ipairs({"logins", "disconnects"}) do
    if extensions.storage_get(key) == nil then extensions.storage_set(key, "0") end
  end
end)

extensions.on("client_login", function(ctx)
  assert(not ctx.can_act)
  local count = tonumber(extensions.storage_get("logins") or "0") or 0
  extensions.storage_set("logins", tostring(count + 1))
  extensions.log("Local login: session " .. ctx.client_id .. " (" .. ctx.client_version .. ")")
end)

extensions.on("client_disconnect", function(ctx)
  assert(not ctx.can_act)
  local count = tonumber(extensions.storage_get("disconnects") or "0") or 0
  extensions.storage_set("disconnects", tostring(count + 1))
  extensions.log("Local disconnect: session " .. ctx.client_id .. ", last lobby " .. ctx.lobby_id)
end)
