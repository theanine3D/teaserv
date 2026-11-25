-- Scheduled concert: at the set time a performer model with its motion and a
-- song appear in one lobby, for duration_minutes, then the lobby is normal
-- again. Only Caelum players see and hear it; players who enter during the
-- show see and hear it from the right moment. The files come from a server
-- modpack (see docs/CAELUM_EXT.md).
--
-- Settings: lobby (a lobby name, any case: LOBBY1 .. LOBBY20 or a name from
-- caelum-lobbies.txt; or its number: 0-19 the stock lobbies, 20-63 the Caelum
-- extra lobbies), weekday (1 = Monday ..
-- 7 = Sunday, 0 = every day), hour, minute, duration_minutes,
-- prefetch_minutes (clients load the model this long before the start),
-- warn_first_minutes and warn_second_minutes (server-wide announcements this
-- long before the start, 0 = none), model (BML file), model_part, motion,
-- music (ADX file, may be empty), x, y, z, rot_y, scale_percent, announce.
-- Stand where the performer should be and type $xyz to read x, y, z.

-- "20" is lobby number 20; anything else is a lobby name.
local lobby = extensions.setting("lobby")
lobby = math.tointeger(tonumber(lobby)) or lobby
local weekday = extensions.setting("weekday")
local hour = extensions.setting("hour")
local minute = extensions.setting("minute")
local duration = extensions.setting("duration_minutes")
local prefetch = extensions.setting("prefetch_minutes")
local model = extensions.setting("model")
local part = extensions.setting("model_part")
local motion = extensions.setting("motion")
local music = extensions.setting("music")
local announce = extensions.setting("announce")

-- The lobby's menu name for messages, or the setting as written when the server has no such lobby.
local function lobby_name()
  local found = extensions.caelum_lobby(lobby)
  return found and found.name or tostring(lobby)
end

local function object(visible, start_ms)
  local spec = {
    model = model,
    x = extensions.setting("x"), y = extensions.setting("y"), z = extensions.setting("z"),
    rot_y = extensions.setting("rot_y"),
    scale = extensions.setting("scale_percent") / 100,
    visible = visible,
  }
  if part ~= "" then spec.model_part = part end
  if motion ~= "" then spec.motion = motion end
  if start_ms then spec.start_utc_ms = start_ms end
  local ok, reason = extensions.caelum_object(lobby, "performer", spec)
  if not ok then extensions.log("Performer not placed in lobby " .. lobby_name() .. ": " .. reason) end
  return ok
end

local function stop()
  extensions.caelum_clear(lobby)
  extensions.storage_set("show_start", nil)
  extensions.log("Concert ended in lobby " .. lobby_name())
end

-- Show the performer and start the song as of start_utc (Unix seconds); end after the rest of the show.
local function show(start_utc, remaining_ms)
  local start_ms = start_utc * 1000
  if not object(true, start_ms) then return false end
  if music ~= "" then
    local ok, reason = extensions.caelum_music(lobby, music, {loop = false, start_utc_ms = start_ms})
    if not ok then extensions.log("Concert music not started: " .. reason) end
  end
  extensions.service_after(remaining_ms, function() stop() end)
  return true
end

local function start()
  if extensions.storage_get("show_start") then return false end
  local now = extensions.now().utc
  if not show(now, duration * 60000) then return false end
  extensions.storage_set("show_start", tostring(now))
  extensions.log("Concert started in lobby " .. lobby_name())
  if announce ~= "" then
    local sent, reason = extensions.announce(announce)
    if not sent then extensions.log("Concert announcement not sent: " .. reason) end
  end
  return true
end

-- The schedule rule for `minutes` before the start (the day before when that crosses midnight).
local function before(minutes)
  local at = hour * 60 + minute - minutes
  local day = weekday
  while at < 0 do
    at = at + 1440
    if day ~= 0 then day = (day == 1) and 7 or (day - 1) end
  end
  local rule = {minute = at % 60, hour = at // 60}
  if day ~= 0 then rule.weekday = day end
  return rule
end

-- The start time, and the earlier time at which clients load the model.
extensions.schedule("concert-start", before(0), function() start() end)

if prefetch > 0 then
  extensions.schedule("concert-prefetch", before(prefetch), function()
    if not extensions.storage_get("show_start") then object(false, nil) end
  end)
end

-- Server-wide reminders before the start.
local function warn(minutes)
  if extensions.storage_get("show_start") then return end
  local text = "A concert starts in " .. minutes .. (minutes == 1 and " minute" or " minutes") ..
    " in lobby " .. lobby_name() .. "!"
  local sent, reason = extensions.announce(text)
  if not sent then extensions.log("Concert reminder not sent: " .. reason) end
end
for index, name in ipairs({"warn_first_minutes", "warn_second_minutes"}) do
  local minutes = extensions.setting(name)
  if minutes > 0 then
    extensions.schedule("concert-warn-" .. index, before(minutes), function() warn(minutes) end)
  end
end

-- After a restart or reload during a show: put it back from its original start.
extensions.on("package_load", function()
  local stored = tonumber(extensions.storage_get("show_start") or "")
  if not stored then return end
  local left = (stored + duration * 60 - extensions.now().utc) * 1000
  if left < 1000 or not show(stored, left) then
    extensions.storage_set("show_start", nil)
  end
end)

extensions.command("concert", {permission = "moderator", cooldown_ms = 2000,
    arguments = {{name = "action", type = "word", max_bytes = 8}},
    help = "start or stop the concert now"},
  function(ctx, raw, args)
    if args.action == "start" then
      extensions.reply(start() and "Concert started." or "The concert could not start (already running, or see the server log).")
    elseif args.action == "stop" then
      if extensions.storage_get("show_start") then
        stop()
        extensions.reply("Concert stopped.")
      else
        extensions.reply("No concert is running.")
      end
    else
      extensions.reply("Usage: concert start | concert stop")
    end
  end)
