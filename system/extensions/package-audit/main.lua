-- Optional lifecycle observer. No player context or client messages are used.
extensions.on("package_load", function(info)
  local count = tonumber(extensions.storage_get("activations") or "0") or 0
  extensions.storage_set("activations", tostring(count + 1))
  extensions.log(info.id .. " loaded: " .. info.reason .. ", activation " .. (count + 1))
end)

extensions.on("package_unload", function(info)
  -- Unload storage is read-only; a staged replacement already owns its copy.
  extensions.log(info.id .. " unloaded: " .. info.reason ..
    ", activations " .. (extensions.storage_get("activations") or "0"))
end)
