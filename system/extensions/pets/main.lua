-- Pets: $pet <name> gives you a pet that follows you around the lobby. Caelum
-- players see it (docs/CAELUM_EXT.md, attached objects); stock clients see
-- nothing. The pet plays its idle motion while you stand still and its walk
-- motion while it follows you. The choice is remembered for your account and
-- the pet comes back in every lobby you enter, until $pet off. The server
-- removes a pet when its player leaves the lobby.
--
-- The pets are listed in pets.txt (this folder; see the comments there): one
-- line per pet with its BML, the model in it, which of its motions are the
-- idle and the walk, and who may use it (everyone, moderators or admins).
-- Edit it and run "extensions reload".
--
-- Settings:
--   choices  more pets without editing pets.txt: "name:model.bml:part:idle:
--            walk:scale_percent:who" entries separated by commas (empty fields
--            allowed, "cat:cat.bml::cat_idle.njm:::moderator"). pets.txt wins
--            a name listed in both.
--   side     units to your right (negative: left), default 9
--   behind   units behind you, default 6

local side = extensions.setting("side")
local behind = extensions.setting("behind")

-- The sandbox has no string.find or patterns: split and trim by hand.
local function split(text, sep)
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

-- Who may use a pet: the word in the list -> the permission level checked.
local WHO = {[""] = "everyone", everyone = "everyone", moderator = "moderator", moderators = "moderator",
             admin = "admin", admins = "admin"}
local WHO_TEXT = {moderator = "moderators", admin = "admins"}

local choices, names = {}, {}
local problems = {}   -- logged at package_load: initialization cannot log

-- f: name, model, part, idle, walk, scale, who ("" = not given)
local function add(f, where)
  local name = string.lower(f[1] or "")
  local scale = (f[6] or "") == "" and 100 or tonumber(f[6])
  local who = WHO[string.lower(f[7] or "")]
  if name == "" or (f[2] or "") == "" or not scale or scale <= 0 then
    problems[#problems + 1] = "Pet ignored (" .. where .. "): needs a name, a model and a positive scale"
  elseif not who then
    problems[#problems + 1] = "Pet ignored (" .. where .. "): who must be everyone, moderator or admin"
  elseif name == "off" then
    problems[#problems + 1] = "Pet ignored (" .. where .. "): \"off\" cannot be a pet's name"
  elseif choices[name] then
    problems[#problems + 1] = "Pet " .. name .. " is listed twice (" .. where .. "); the first is used"
  else
    choices[name] = {model = f[2], part = f[3] or "", motion = f[4] or "", move = f[5] or "", scale = scale / 100, who = who}
    names[#names + 1] = name
  end
end

-- pets.txt: "name model part idle walk scale who" per line, "-" for a field left out, "#" starts a comment.
local list = extensions.package_text("pets.txt") or ""
local number = 0
for _, line in ipairs(split(list, "\n")) do
  number = number + 1
  local hash = 0
  for i = 1, #line do if hash == 0 and string.sub(line, i, i) == "#" then hash = i end end
  if hash > 0 then line = string.sub(line, 1, hash - 1) end
  local f = words(line)
  if #f > 0 then
    if #f > 7 then
      problems[#problems + 1] = "pets.txt line " .. number .. ": more than 7 fields; the rest is ignored"
    end
    for i = 1, 7 do if f[i] == "-" or f[i] == nil then f[i] = "" end end
    add(f, "pets.txt line " .. number)
  end
end

for _, entry in ipairs(split(extensions.setting("choices"), ",")) do
  if trim(entry) ~= "" then
    local f = split(entry, ":")
    for i = 1, 7 do f[i] = trim(f[i] or "") end
    add(f, "setting choices: " .. entry)
  end
end

-- What the player of this callback may use: two permission checks, whatever the length of the list.
local function rights()
  return {everyone = true, moderator = extensions.has_permission("moderator"), admin = extensions.has_permission("admin")}
end

-- The pets this player may use; staff see which ones are restricted.
local function offered(can)
  local out = {}
  for _, name in ipairs(names) do
    local who = choices[name].who
    if can[who] then out[#out + 1] = WHO_TEXT[who] and (name .. " (" .. WHO_TEXT[who] .. ")") or name end
  end
  return out
end

local function key(ctx) return "pet-" .. ctx.client_id end

-- Shows the pet next to the player of this callback. Returns ok, reason.
local function place(ctx, name)
  local c = choices[name]
  if not c then return false, "unknown_pet" end
  local here = extensions.position()
  if not here or not here.lobby_number then return false, "not_in_lobby" end
  local spec = {model = c.model, attach = ctx.client_id, follow = true, x = -side, z = -behind, scale = c.scale}
  if c.part ~= "" then spec.model_part = c.part end
  if c.motion ~= "" then spec.motion = c.motion end
  if c.move ~= "" then spec.move_motion = c.move end
  return extensions.caelum_object(here.lobby_number, key(ctx), spec)
end

local function remove(ctx)
  local here = extensions.position()
  if here and here.lobby_number then extensions.caelum_remove(here.lobby_number, key(ctx)) end
end

extensions.on("package_load", function()
  for _, text in ipairs(problems) do extensions.log(text) end
end)

-- A remembered pet comes back in every lobby the player enters (while they may still use it).
extensions.on("lobby_join", function(ctx)
  if ctx.is_game or not ctx.player_key then return end
  local name = extensions.player_get(ctx.player_key, "pet")
  local c = name and choices[name]
  if c and (c.who == "everyone" or extensions.has_permission(c.who)) then
    local ok, reason = place(ctx, name)
    if not ok and reason ~= "not_in_lobby" then extensions.log("Pet " .. name .. " not shown: " .. tostring(reason)) end
  end
end)

extensions.command("pet", {cooldown_ms = 3000, help = "pet <name> | pet off | pet: list the pets",
    arguments = {{name = "choice", type = "word", max_bytes = 32, optional = true}}},
  function(ctx, raw, args)
    local choice = args.choice and string.lower(args.choice) or nil
    if choice == "off" then
      remove(ctx)
      if ctx.player_key then extensions.player_set(ctx.player_key, "pet", nil) end
      extensions.reply("Your pet went home.")
      return
    end
    local can = rights()
    local list = offered(can)
    if not choice then
      if #list == 0 then
        extensions.reply("No pets are set up on this server.")
      else
        extensions.reply("Pets: " .. table.concat(list, ", ") .. ". Use: pet <name>, pet off")
      end
      return
    end
    local c = choices[choice]
    if not c then
      extensions.reply("No pet named " .. choice .. ". Pets: " .. table.concat(list, ", "))
      return
    end
    if not can[c.who] then
      extensions.reply("The " .. choice .. " is for " .. WHO_TEXT[c.who] .. " only.")
      return
    end
    local ok, reason = place(ctx, choice)
    if ok then
      if ctx.player_key then extensions.player_set(ctx.player_key, "pet", choice) end
      extensions.reply("Your " .. choice .. " follows you now (Caelum players can see it).")
    elseif reason == "not_in_lobby" then
      extensions.reply("Pets can only come along in a lobby.")
    elseif reason == "lobby_full" then
      extensions.reply("This lobby has no room for another pet right now.")
    else
      extensions.reply("Your pet could not come along (" .. tostring(reason) .. ").")
    end
  end)
