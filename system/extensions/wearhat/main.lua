-- Wear a hat: $wearhat <name> puts a cosmetic on your head. It sits on the
-- head bone, stays level and bobs with your animation. Caelum players see it
-- (docs/CAELUM_EXT.md, attached objects); stock clients see nothing. The
-- choice is remembered for your account and put back in every lobby you
-- enter, until $wearhat off. The server removes it when you leave a lobby.
--
-- The hats are listed in hats.txt (this folder; see the comments there): one
-- line per hat with its BML, the model in it, how it sits, and who may wear it
-- (everyone, moderators or admins). Edit it and run "extensions reload".
--
-- Settings:
--   choices  more hats without editing hats.txt: "name:model.bml:part:height:
--            scale_percent:tilt:who" entries separated by commas (empty fields
--            allowed, "crown:crown.bml:::::admin"). hats.txt wins a name
--            listed in both.

local function split(text, sep)   -- the sandbox has no string.find or patterns
  local parts, start = {}, 1
  for i = 1, #text + 1 do
    if i > #text or string.sub(text, i, i) == sep then
      parts[#parts + 1] = string.sub(text, start, i - 1)
      start = i + 1
    end
  end
  return parts
end

local function trim(text)
  local first, last = 1, #text
  while first <= last and string.byte(text, first) <= 32 do first = first + 1 end
  while last >= first and string.byte(text, last) <= 32 do last = last - 1 end
  return string.sub(text, first, last)
end

-- The words of a line, split at spaces, tabs (and a Windows line end's CR).
local function words(line)
  local out, word = {}, ""
  for i = 1, #line do
    local b = string.byte(line, i)
    if b <= 32 then
      if word ~= "" then out[#out + 1] = word; word = "" end
    else
      word = word .. string.sub(line, i, i)
    end
  end
  if word ~= "" then out[#out + 1] = word end
  return out
end

-- Who may wear a hat: the word in the list -> the permission level checked.
local WHO = {[""] = "everyone", everyone = "everyone", moderator = "moderator", moderators = "moderator",
             admin = "admin", admins = "admin"}
local WHO_TEXT = {moderator = "moderators", admin = "admins"}

local hats, names = {}, {}
local problems = {}   -- logged at package_load: initialization cannot log

-- f: name, model, part, height, scale, tilt, who ("" = not given)
local function add(f, where)
  local name = string.lower(f[1] or "")
  local height = (f[4] or "") == "" and 1.5 or tonumber(f[4])
  local scale = (f[5] or "") == "" and 100 or tonumber(f[5])
  local tilt = (f[6] or "") == "" and 0 or tonumber(f[6])
  local who = WHO[string.lower(f[7] or "")]
  if name == "" or (f[2] or "") == "" or not height or not scale or scale <= 0 or not tilt then
    problems[#problems + 1] = "Hat ignored (" .. where .. "): needs a name, a model and numbers for height, scale and tilt"
  elseif not who then
    problems[#problems + 1] = "Hat ignored (" .. where .. "): who must be everyone, moderator or admin"
  elseif name == "off" then
    problems[#problems + 1] = "Hat ignored (" .. where .. "): \"off\" cannot be a hat's name"
  elseif hats[name] then
    problems[#problems + 1] = "Hat " .. name .. " is listed twice (" .. where .. "); the first is used"
  else
    hats[name] = {model = f[2], part = f[3] or "", height = height, scale = scale / 100, tilt = tilt, who = who}
    names[#names + 1] = name
  end
end

-- hats.txt: "name model part height scale tilt who" per line, "-" for a field left out, "#" starts a comment.
local list = extensions.package_text("hats.txt") or ""
local number = 0
for _, line in ipairs(split(list, "\n")) do
  number = number + 1
  local hash = 0
  for i = 1, #line do if hash == 0 and string.sub(line, i, i) == "#" then hash = i end end
  if hash > 0 then line = string.sub(line, 1, hash - 1) end
  local f = words(line)
  if #f > 0 then
    if #f > 7 then
      problems[#problems + 1] = "hats.txt line " .. number .. ": more than 7 fields; the rest is ignored"
    end
    for i = 1, 7 do if f[i] == "-" or f[i] == nil then f[i] = "" end end
    add(f, "hats.txt line " .. number)
  end
end

for _, entry in ipairs(split(extensions.setting("choices"), ",")) do
  if trim(entry) ~= "" then
    local f = split(entry, ":")
    for i = 1, 7 do f[i] = trim(f[i] or "") end
    add(f, "setting choices: " .. entry)
  end
end

-- What the player of this callback may wear: two permission checks, whatever the length of the list.
local function rights()
  return {everyone = true, moderator = extensions.has_permission("moderator"), admin = extensions.has_permission("admin")}
end

-- The hats this player may wear; staff see which ones are restricted.
local function offered(can)
  local out = {}
  for _, name in ipairs(names) do
    local who = hats[name].who
    if can[who] then out[#out + 1] = WHO_TEXT[who] and (name .. " (" .. WHO_TEXT[who] .. ")") or name end
  end
  return out
end

local function key(ctx) return "hat-" .. ctx.client_id end

-- Puts the hat on the player of this callback. Returns ok, reason.
local function wear(ctx, name)
  local h = hats[name]
  if not h then return false, "unknown_hat" end
  local here = extensions.position()
  if not here or not here.lobby_number then return false, "not_in_lobby" end
  local spec = {model = h.model, attach = ctx.client_id, bone = "head", upright = true, y = h.height, scale = h.scale}
  if h.part ~= "" then spec.model_part = h.part end
  if h.tilt ~= 0 then spec.rot_x = h.tilt end
  return extensions.caelum_object(here.lobby_number, key(ctx), spec)
end

extensions.on("package_load", function()
  for _, text in ipairs(problems) do extensions.log(text) end
end)

-- A remembered hat is put back in every lobby the player enters (while they may still wear it).
extensions.on("lobby_join", function(ctx)
  if ctx.is_game or not ctx.player_key then return end
  local name = extensions.player_get(ctx.player_key, "hat")
  local h = name and hats[name]
  if h and (h.who == "everyone" or extensions.has_permission(h.who)) then
    local ok, reason = wear(ctx, name)
    if not ok and reason ~= "not_in_lobby" then extensions.log("Hat " .. name .. " not shown: " .. tostring(reason)) end
  end
end)

extensions.command("wearhat", {cooldown_ms = 3000, help = "wearhat <name> | wearhat off | wearhat: list the hats",
    arguments = {{name = "choice", type = "word", max_bytes = 32, optional = true}}},
  function(ctx, raw, args)
    local choice = args.choice and string.lower(args.choice) or nil
    if choice == "off" then
      local here = extensions.position()
      if here and here.lobby_number then extensions.caelum_remove(here.lobby_number, key(ctx)) end
      if ctx.player_key then extensions.player_set(ctx.player_key, "hat", nil) end
      extensions.reply("Hat off.")
      return
    end
    local can = rights()
    local list = offered(can)
    if not choice then
      if #list == 0 then
        extensions.reply("No hats are set up on this server.")
      else
        extensions.reply("Hats: " .. table.concat(list, ", ") .. ". Use: wearhat <name>, wearhat off")
      end
      return
    end
    local h = hats[choice]
    if not h then
      extensions.reply("No hat named " .. choice .. ". Hats: " .. table.concat(list, ", "))
      return
    end
    if not can[h.who] then
      extensions.reply("The " .. choice .. " is for " .. WHO_TEXT[h.who] .. " only.")
      return
    end
    local ok, reason = wear(ctx, choice)
    if ok then
      if ctx.player_key then extensions.player_set(ctx.player_key, "hat", choice) end
      extensions.reply("You are wearing the " .. choice .. " (Caelum players can see it).")
    elseif reason == "not_in_lobby" then
      extensions.reply("Hats can only be worn in a lobby.")
    elseif reason == "lobby_full" then
      extensions.reply("This lobby has no room for another hat right now.")
    else
      extensions.reply("The hat could not be put on (" .. tostring(reason) .. ").")
    end
  end)
