-- Daily login reward: Episode 3 Meseta once per server day, given when the
-- player first enters a lobby that day. Settings: meseta_per_day, streak_bonus
-- (extra Meseta for each consecutive day after the first, up to
-- streak_max_days), message.

local per_day = extensions.setting("meseta_per_day")
local streak_bonus = extensions.setting("streak_bonus")
local streak_max = extensions.setting("streak_max_days")
local message = extensions.setting("message")

extensions.on("lobby_join", function(ctx)
  -- Meseta is an Episode 3 account balance; games and other versions are skipped.
  if ctx.is_game or not ctx.player_key or ctx.client_version ~= "GC_EP3" then return end
  local key = ctx.player_key
  local today = extensions.now().day_number
  local last = tonumber(extensions.player_get(key, "last_day") or "")
  if last == today then return end

  local streak = 1
  if last == today - 1 then
    streak = (tonumber(extensions.player_get(key, "streak") or "") or 1) + 1
    if streak > streak_max then streak = streak_max end
  end
  local amount = per_day + streak_bonus * (streak - 1)

  -- The grant ID makes a repeat for the same day pay nothing, even if the
  -- player record below was lost before it was saved.
  local ok, status = extensions.meseta_grant("self", amount, "daily-" .. today, "daily login reward")
  if not ok then return end
  extensions.player_set(key, "last_day", tostring(today))
  extensions.player_set(key, "streak", tostring(streak))
  if status == "granted" then
    local text = message .. " " .. amount .. " Meseta"
    if streak > 1 and streak_bonus > 0 then text = text .. " (day " .. streak .. ")" end
    extensions.reply(text)
  end
end)
