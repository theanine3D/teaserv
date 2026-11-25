-- Player of the Week / Month: counts Episode 3 battle wins per period. When a
-- period ends, the leader is announced and receives the prize (at once when
-- online, otherwise at the next login).
--
-- Settings: period ("week" = Monday to Sunday, or "month"), prize,
-- count_com_wins (wins against computer-only teams), min_rounds (shorter
-- battles do not count), max_wins_per_day (per player), title.

local period = extensions.setting("period")
local prize = extensions.setting("prize")
local count_com = extensions.setting("count_com_wins")
local min_rounds = extensions.setting("min_rounds")
local max_per_day = extensions.setting("max_wins_per_day")
local title = extensions.setting("title")

if period ~= "week" and period ~= "month" then
  error("setting period must be \"week\" or \"month\"")
end

local function period_id(now)
  if period == "month" then return now.year .. "-" .. now.month end
  return now.iso_year .. "-W" .. now.iso_week
end

-- Close every finished period. Safe to call often and after a restart: the
-- stored period name and the grant ID stop a second payout.
local function settle()
  local current = period_id(extensions.now())
  local stored = extensions.storage_get("period")
  if stored == current then return end
  if stored then
    local top = extensions.board_top("wins", 1)
    local winner = top[1]
    if winner and winner.score > 0 then
      local text = title .. ": " .. winner.name .. " (" .. winner.score .. " wins)"
      if prize > 0 then
        local ok, status = extensions.meseta_grant(winner.player_key, prize, "prize-" .. stored, title .. " prize")
        if ok then text = text .. " wins " .. prize .. " Meseta!" end
        extensions.log("Period " .. stored .. " prize for " .. winner.player_key .. ": " .. status)
      end
      extensions.storage_set("last_winner", text)
      local sent, reason = extensions.announce(text)
      if not sent then extensions.log("Winner announcement not sent: " .. reason) end
    else
      extensions.log("Period " .. stored .. " closed without a winner")
    end
    extensions.board_reset("wins")
  end
  extensions.storage_set("period", current)
end

extensions.on("package_load", function() settle() end)
extensions.schedule("period-check", {minute = 0}, function() settle() end)

extensions.on("ep3_battle_ended", function(info)
  settle()
  if info.rounds < min_rounds then return end
  local winners, losers
  for _, team in ipairs(info.teams) do
    if team.won then winners = team.players else losers = team.players end
  end
  if not winners or not losers then return end
  if not count_com then
    local human_opponent = false
    for _, p in ipairs(losers) do
      if not p.is_com and p.player_key then human_opponent = true end
    end
    if not human_opponent then return end
  end
  local today = tostring(extensions.now().day_number)
  for _, p in ipairs(winners) do
    if not p.is_com and p.player_key then
      local wins_today = 0
      if extensions.player_get(p.player_key, "day") == today then
        wins_today = tonumber(extensions.player_get(p.player_key, "wins") or "") or 0
      end
      if wins_today < max_per_day then
        extensions.player_set(p.player_key, "day", today)
        extensions.player_set(p.player_key, "wins", tostring(wins_today + 1))
        extensions.board_add("wins", p.player_key, 1, p.name)
      end
    end
  end
end)

extensions.command("ranking", {cooldown_ms = 5000, arguments = {}, help = "Show this period's battle win leaders"},
  function(ctx)
    local top = extensions.board_top("wins", 5)
    if #top == 0 then
      extensions.reply(title .. ": no wins counted yet.")
    else
      local text = title .. " standings"
      for _, entry in ipairs(top) do
        text = text .. "\n" .. entry.rank .. ". " .. entry.name .. " - " .. entry.score
      end
      extensions.reply(text)
    end
    local last = extensions.storage_get("last_winner")
    if last then extensions.reply("Last: " .. last) end
  end)
