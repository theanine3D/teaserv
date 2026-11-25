// Caelum extension channel: the lobby object registry, the snapshot text read
// back by the client's own parser (src/CaelumExt.h is the shared header), and
// the Lua API through the real runtime with a synthetic host.

#include "Runtime.hh"

#include "../../src/CaelumExt.h"

#include <cmath>
#include <fstream>
#include <algorithm>
#include <iostream>
#include <iterator>
#include <stdexcept>

using namespace teaserv::extensions;

static void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

static CextSnapshot parse(const std::string& text) {
  static CextSnapshot snapshot;
  check(cext_parse_snapshot(text.data(), text.size(), &snapshot), "the client parser refused a server snapshot");
  return snapshot;
}

static bool refused(const std::string& text) {
  static CextSnapshot snapshot;
  return !cext_parse_snapshot(text.data(), text.size(), &snapshot);
}

static std::string read_file(const char* path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error(std::string("cannot read ") + path);
  return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

static Setting setting(int64_t value) { return {"integer", SettingValue{value}}; }
static Setting setting(int value) { return setting(static_cast<int64_t>(value)); }
static Setting setting(const char* value) { return {"string", SettingValue{std::string(value)}}; }

struct TestHost : Host {
  CaelumScenes scenes;
  std::vector<uint8_t> synced;
  std::vector<std::string> logs, replies, announcements;
  int64_t utc = 0;
  ClockTime now() const override { return clock_time(utc, 0); }
  AnnouncementResult announce(const std::string&, const std::string& text) override {
    announcements.emplace_back(text);
    return {true, nullptr};
  }
  // strict: permission levels other than "everyone" need an entry in levels (client ID -> levels held).
  bool strict = false;
  std::map<uint64_t, std::set<std::string>> levels;
  bool authorized(const Context& context) const override {
    if (!context.allow_player_actions) return false;
    if (!strict || context.permission == "everyone") return true;
    auto it = levels.find(context.client_id);
    return it != levels.end() && it->second.count(context.permission);
  }
  void reply(const Context&, const std::string& text) override { replies.emplace_back(text); }
  void broadcast(const Context&, const std::string&) override {}
  bool whisper(const Context&, uint64_t, const std::string&) override { return false; }
  void log(const std::string&, const std::string& text) override { logs.emplace_back(text); }
  bool command_reserved(const std::string&) const override { return false; }
  CaelumResult caelum_object(const std::string& owner, uint8_t lobby, const std::string& key, const CaelumObjectSpec& spec) override {
    if (lobby != 20) return {false, "no_such_lobby"};
    auto copy = spec;
    if (!copy.start_ms) copy.start_ms = 1000;
    auto result = scenes.set_object(owner, lobby, key, copy);
    if (result.ok) synced.emplace_back(lobby);
    return result;
  }
  bool caelum_remove(const std::string& owner, uint8_t lobby, const std::string& key) override {
    return scenes.remove_object(owner, lobby, key);
  }
  CaelumResult caelum_music(const std::string& owner, uint8_t lobby, const CaelumMusicSpec* spec) override {
    if (!spec) return {scenes.stop_music(owner, lobby), nullptr};
    return scenes.set_music(owner, lobby, *spec);
  }
  bool caelum_clear(const std::string& owner, uint8_t lobby) override { return scenes.clear(owner, lobby); }
  int caelum_listeners(uint8_t lobby) const override { return lobby == 20 ? 3 : lobby == 4 ? 0 : -1; }
  // Two lobbies: stock lobby 4 "Lobby 05" and Caelum lobby 20 "Stage".
  uint8_t caelum_lobby_named(const std::string& name) const override {
    return name == "stage" || name == "Stage" ? 20 : name == "LOBBY 05" ? 4 : CaelumScenes::NO_LOBBY;
  }
  std::optional<std::string> caelum_lobby_name(uint8_t lobby) const override {
    if (lobby == 20) return "Stage";
    if (lobby == 4) return "Lobby 05";
    return std::nullopt;
  }
  PlayerData players;
  PlayerData* player_data(const std::string&) override { return &players; }
  // Clients 42 and 44-59 stand in lobby 20 "Stage"; 43 is nowhere (in a game, say).
  std::optional<Position> player_position(const Context& context) const override {
    if (context.client_id != 42 && (context.client_id < 44 || context.client_id > 59)) return std::nullopt;
    Position where;
    where.x = 12.34;
    where.y = -0.06;
    where.z = -60.24;
    where.floor = 0x0F;
    where.lobby_number = 20;
    where.lobby_name = "Stage";
    return where;
  }
};

int main(int argc, char** argv) {
  try {
    // --- Registry and wire text ---
    {
      CaelumScenes scenes;
      CaelumObjectSpec singer;
      singer.model = "concert_singer.bml";
      singer.part = "singer_body.gj";
      singer.texture = "concert_singer.gvm";
      singer.motion = "concert_sing.njm";
      singer.x = 12.5;
      singer.y = 0;
      singer.z = -40.25;
      singer.rot_y = 180;
      singer.scale = 1.5;
      singer.start_ms = 1795000000123LL;
      check(scenes.set_object("concert", 20, "singer", singer).ok, "valid object refused");
      CaelumObjectSpec prop;
      prop.model = "stage_light.bml";
      prop.visible = false;
      prop.loop = false;
      check(scenes.set_object("decor", 20, "light", prop).ok, "second owner refused");
      check(scenes.set_music("concert", 20, {"concert_song.adx", 1795000000000LL, false}).ok, "music refused");
      check(std::string(scenes.set_music("decor", 20, {"other.adx", 1, true}).reason) == "music_owned", "music ownership not enforced");

      auto text = scenes.snapshot(20, "L3403677716", 1795000005000LL);
      check(text.size() < CEXT_MAX_SNAPSHOT, "snapshot too large");
      auto s = parse(text);
      check(s.version == 1 && std::string(s.room) == "L3403677716" && s.now_ms == 1795000005000LL && s.count == 2, "header wrong");
      const auto& o = s.objects[0];
      check(std::string(o.id) == "concert:singer" && std::string(o.model) == "concert_singer.bml" &&
              std::string(o.tex) == "concert_singer.gvm" && std::string(o.motion) == "concert_sing.njm" &&
              std::string(o.part) == "singer_body.gj" && !s.objects[1].part[0], "object names wrong");
      check(o.x == 12.5f && o.y == 0.0f && o.z == -40.25f && o.ry == 180.0f && o.scale == 1.5f &&
              o.start_ms == 1795000000123LL && o.loop == 1 && o.show == 1, "object values wrong");
      check(std::string(s.objects[1].id) == "decor:light" && s.objects[1].show == 0 && s.objects[1].loop == 0 &&
              !s.objects[1].tex[0] && !s.objects[1].motion[0] && s.objects[1].scale == 1.0f, "second object wrong");
      check(s.has_music && std::string(s.music) == "concert_song.adx" && s.music_start_ms == 1795000000000LL && !s.music_loop,
          "music wrong");

      // Replacing keeps the slot; an empty or unknown lobby is an empty room.
      singer.x = 1;
      check(scenes.set_object("concert", 20, "singer", singer).ok && scenes.object_count() == 2, "replace changed the count");
      check(parse(scenes.snapshot(20, "L1", 5)).objects[0].x == 1.0f, "replacement not visible");
      auto empty = parse(scenes.snapshot(CaelumScenes::NO_LOBBY, "G77", 9));
      check(empty.count == 0 && !empty.has_music && std::string(empty.room) == "G77", "empty room wrong");

      // Refusals.
      check(scenes.set_object("concert", 0, "stock", singer).ok && scenes.remove_object("concert", 0, "stock"), "stock lobby 0 refused");
      check(std::string(scenes.set_object("concert", CaelumScenes::NO_LOBBY, "a", singer).reason) == "invalid_lobby", "NO_LOBBY accepted");
      check(std::string(scenes.set_object("concert", 64, "a", singer).reason) == "invalid_lobby", "lobby 64 accepted");
      check(std::string(scenes.set_object("concert", 20, "Bad Key", singer).reason) == "invalid_key", "bad key accepted");
      for (const char* bad : {"", "../x.bml", "a/b.bml", "a b.bml", ".hidden", "x=y.bml", "a\\b.bml",
               "0123456789012345678901234567890123456789012345678.bml"}) {
        auto spec = singer;
        spec.model = bad;
        check(std::string(scenes.set_object("concert", 20, "a", spec).reason) == "invalid_file", "bad file name accepted");
      }
      auto spec = singer;
      spec.scale = 0;
      check(std::string(scenes.set_object("concert", 20, "a", spec).reason) == "invalid_value", "zero scale accepted");
      spec = singer;
      spec.x = std::nan("");
      check(std::string(scenes.set_object("concert", 20, "a", spec).reason) == "invalid_value", "NaN accepted");
      spec = singer;
      spec.z = INFINITY;
      check(std::string(scenes.set_object("concert", 20, "a", spec).reason) == "invalid_value", "infinity accepted");

      // Quotas: 64 per lobby (CEXT_MAX_OBJECTS), 256 per package.
      static_assert(CaelumScenes::MAX_OBJECTS_PER_LOBBY == CEXT_MAX_OBJECTS && CaelumScenes::LEGACY_OBJECTS == CEXT_LEGACY_OBJECTS);
      for (int z = 0; z < 62; z++) check(scenes.set_object("concert", 20, "k" + std::to_string(z), singer).ok, "below lobby quota refused");
      check(std::string(scenes.set_object("concert", 20, "one-more", singer).reason) == "lobby_full", "lobby quota not enforced");
      auto full = parse(scenes.snapshot(20, "L1", 5));
      check(full.count == 64, "full lobby does not round-trip");
      // A client from before "objects=" gets the first 16 placed.
      auto legacy = parse(scenes.snapshot(20, "L1", 5, CaelumScenes::LEGACY_OBJECTS));
      check(legacy.count == 16 && std::string(legacy.objects[0].id) == std::string(full.objects[0].id) &&
              std::string(legacy.objects[15].id) == std::string(full.objects[15].id) && legacy.has_music, "legacy snapshot wrong");
      for (int lobby = 21; lobby <= 23; lobby++) {
        for (int z = 0; z < 64; z++) scenes.set_object("concert", lobby, "k" + std::to_string(z), singer);
      }
      check(scenes.object_count() == 256, "setup count wrong");
      check(scenes.set_object("concert", 24, "x", singer).ok, "256th package object refused");
      check(std::string(scenes.set_object("concert", 24, "y", singer).reason) == "package_quota", "package quota not enforced");

      // Owner cleanup reports the changed lobbies and leaves other owners alone.
      auto changed = scenes.remove_owner("concert");
      check(changed == std::set<uint8_t>({20, 21, 22, 23, 24}) && scenes.object_count() == 1 && !scenes.empty(20), "owner cleanup wrong");
      auto after = parse(scenes.snapshot(20, "L1", 5));
      check(after.count == 1 && std::string(after.objects[0].id) == "decor:light" && !after.has_music, "cleanup left the owner's music");
      check(scenes.remove_object("decor", 20, "light") && !scenes.remove_object("decor", 20, "light") && scenes.empty(20), "remove wrong");
    }

    // --- The client parser refuses malformed text and skips later-version additions ---
    {
      const std::string head = "v=1\nroom=L5\nnow=10\n";
      check(!refused(head + "obj a:b model=m.bml future=1 x=1\nlater line\nmusic s.adx start=3 loop=1 volume=9\n"),
          "later-version fields or lines refused");
      check(refused("room=L5\nnow=10\n") && refused("v=2\nroom=L5\nnow=10\n") && refused("v=1\nnow=10\n") && refused("v=1\nroom=L5\n"),
          "missing header accepted");
      check(refused(head + "obj a:b x=1\n"), "object without model accepted");
      check(refused(head + "obj a:b model=../m.bml\n"), "path in model accepted");
      check(refused(head + "obj a:b model=m.bml x=abc\n") && refused(head + "obj a:b model=m.bml x=nan\n") &&
              refused(head + "obj a:b model=m.bml scale=0\n") && refused(head + "obj a:b model=m.bml x=1e30\n"), "bad number accepted");
      check(refused(head + "obj a:b model=m.bml\nobj a:b model=n.bml\n"), "duplicate id accepted");
      check(refused(head + "obj a:b model=m.bml  x=1\n") && refused(head + "obj a:b model\n"), "malformed field accepted");
      check(refused(head + "music a.adx\nmusic b.adx\n") && refused(head + "music a/b.adx\n"), "bad music accepted");
      std::string many = head;
      for (int z = 0; z <= CEXT_MAX_OBJECTS; z++) many += "obj a:k" + std::to_string(z) + " model=m.bml\n";
      check(refused(many), "65 objects accepted");
      check(cext_parse_objects("v=1\nobjects=64\n", 15) == 64 && cext_parse_objects("v=1\n", 4) == 16, "objects= wrong");
      check(refused(std::string(CEXT_MAX_SNAPSHOT + 1, 'x')), "oversized snapshot accepted");
      // Zero padding after the text (game packets are padded to 4 bytes) is fine.
      std::string padded = head + "obj a:b model=m.bml\n";
      padded.append(3, '\0');
      check(!refused(padded), "zero padding refused");
      check(cext_parse_caps("v=1\ncaps=scene.model,audio.music,future.thing\n", 44) == (CEXT_CAP_MODEL | CEXT_CAP_MUSIC), "caps wrong");
      check(cext_parse_caps("caps=scene.model\n", 17) == 0 && cext_parse_caps("v=1\ncaps=\n", 10) == 0, "caps without version accepted");
    }

    // --- Lua API ---
    {
      TestHost host;
      Context alice{42, 1, "Alice", "everyone", "GC_EP3", false, true, ""};
      const char* source = R"lua(
        extensions.command("show", {cooldown_ms = 0}, function(ctx)
          local ok, reason = extensions.caelum_object(20, "singer",
            {model = "singer.bml", model_part = "body.gj", texture = "singer.gvm", motion = "sing.njm", x = 1.5, y = 0, z = -3, rot_y = 90, scale = 2,
             loop = false, visible = true, start_utc_ms = 5000})
          extensions.log("object " .. tostring(ok) .. " " .. tostring(reason))
          ok, reason = extensions.caelum_object(21, "singer", {model = "singer.bml"})
          extensions.log("other lobby " .. tostring(ok) .. " " .. tostring(reason))
          ok, reason = extensions.caelum_music(20, "song.adx", {loop = false, start_utc_ms = 7000})
          extensions.log("music " .. tostring(ok) .. " listeners " .. tostring(extensions.caelum_listeners(20)) ..
            " " .. tostring(extensions.caelum_listeners(21)))
        end)
        extensions.command("hide", {cooldown_ms = 0}, function(ctx)
          extensions.log("remove " .. tostring(extensions.caelum_remove(20, "singer")) .. " " ..
            tostring(extensions.caelum_remove(20, "singer")))
          extensions.log("stop " .. tostring(extensions.caelum_music(20, nil)))
        end)
        extensions.command("bad1", {cooldown_ms = 0}, function(ctx) extensions.caelum_object(64, "a", {model = "m.bml"}) end)
        extensions.command("bad2", {cooldown_ms = 0}, function(ctx) extensions.caelum_object(20, "a", {model = "m.bml", bogus = 1}) end)
        extensions.command("bad3", {cooldown_ms = 0}, function(ctx) extensions.caelum_object(20, "a", {x = 1}) end)
        extensions.command("bad4", {cooldown_ms = 0}, function(ctx) extensions.caelum_object(20, "a", {model = "m.bml", x = "1"}) end)
        extensions.schedule("show-start", {minute = 0}, function(info)
          local ok = extensions.caelum_object(20, "auto", {model = "auto.bml", visible = false})
          extensions.log("scheduled object " .. tostring(ok))
        end)
      )lua";
      Runtime runtime(host, {"concert", "1", source, {"commands", "schedule", "caelum.scene"}, {}});
      runtime.dispatch("show", "", alice);
      check(host.logs.size() == 3 && host.logs[0] == "object true nil" && host.logs[1] == "other lobby false no_such_lobby" &&
              host.logs[2] == "music true listeners 3 nil", "Lua scene calls wrong");
      auto s = parse(host.scenes.snapshot(20, "L1", 1));
      check(s.count == 1 && std::string(s.objects[0].id) == "concert:singer" && std::string(s.objects[0].part) == "body.gj" && s.objects[0].x == 1.5f && s.objects[0].z == -3.0f &&
              s.objects[0].ry == 90.0f && s.objects[0].scale == 2.0f && !s.objects[0].loop && s.objects[0].start_ms == 5000 &&
              s.has_music && s.music_start_ms == 7000 && !s.music_loop, "Lua spec not carried");
      // Schedules have no player but may place objects (the concert start).
      runtime.run_schedules(clock_time(3600, 0));
      check(host.logs.back() == "scheduled object true" && parse(host.scenes.snapshot(20, "L1", 1)).count == 2, "schedule could not place an object");
      runtime.dispatch("hide", "", alice);
      check(host.logs[host.logs.size() - 2] == "remove true false" && host.logs.back() == "stop true", "Lua remove/stop wrong");
      size_t errors = runtime.diagnostics().callback_errors;
      runtime.dispatch("bad1", "", alice);
      runtime.dispatch("bad2", "", alice);
      check(runtime.diagnostics().callback_errors == errors + 2, "invalid lobby or unknown field accepted");
    }
    {
      TestHost host;
      Context alice{42, 1, "Alice", "everyone", "GC_EP3", false, true, ""};
      Runtime runtime(host, {"t", "1", R"lua(
        extensions.command("bad3", {cooldown_ms = 0}, function(ctx) extensions.caelum_object(20, "a", {x = 1}) end)
        extensions.command("bad4", {cooldown_ms = 0}, function(ctx) extensions.caelum_object(20, "a", {model = "m.bml", x = "1"}) end)
      )lua", {"commands", "caelum.scene"}, {}});
      runtime.dispatch("bad3", "", alice);
      runtime.dispatch("bad4", "", alice);
      check(runtime.diagnostics().callback_errors == 2 && host.scenes.object_count() == 0, "missing model or string number accepted");
      Runtime nogrant(host, {"u", "1", R"lua(
        extensions.command("try", {cooldown_ms = 0}, function(ctx) extensions.caelum_object(20, "a", {model = "m.bml"}) end)
      )lua", {"commands"}, {}});
      nogrant.dispatch("try", "", alice);
      check(nogrant.diagnostics().callback_errors == 1 && host.scenes.object_count() == 0, "scene call without its grant accepted");
    }

    {
      TestHost host;
      Context alice{42, 1, "Alice", "everyone", "GC_EP3", false, true, ""};
      Context bob{43, 1, "Bob", "everyone", "GC_EP3", false, true, ""};
      Runtime runtime(host, {"t", "1", R"lua(
        local function show(found)
          if not found then return "nil" end
          return found.number .. "/" .. found.name .. "/" .. found.listeners
        end
        extensions.command("lobbies", {cooldown_ms = 0}, function(ctx)
          extensions.log(show(extensions.caelum_lobby("stage")) .. " " .. show(extensions.caelum_lobby(4)) .. " " ..
            show(extensions.caelum_lobby("Nowhere")) .. " " .. show(extensions.caelum_lobby(5)))
          local ok, reason = extensions.caelum_object("Stage", "a", {model = "m.bml"})
          local ok2, reason2 = extensions.caelum_object("Nowhere", "b", {model = "m.bml"})
          extensions.log(tostring(ok) .. " " .. tostring(reason) .. " " .. tostring(ok2) .. " " .. tostring(reason2))
        end)
        extensions.command("where", {cooldown_ms = 0}, function(ctx)
          local p = extensions.position()
          if not p then extensions.log("nowhere") return end
          extensions.log(string.format("%.1f %.1f %.1f", p.x, p.y, p.z) .. " " .. p.floor .. " " .. tostring(p.is_game) ..
            " " .. p.lobby_number .. " " .. p.lobby_name)
        end)
        extensions.command("bad", {cooldown_ms = 0}, function(ctx) extensions.caelum_lobby(1.5) end)
      )lua", {"commands", "caelum.scene", "lobby.read"}, {}});
      runtime.dispatch("lobbies", "", alice);
      check(host.logs.size() == 2 && host.logs[0] == "20/Stage/3 4/Lobby 05/0 nil nil" && host.logs[1] == "true nil false no_such_lobby",
          "lobby by name wrong");
      runtime.dispatch("where", "", alice);
      runtime.dispatch("where", "", bob);
      check(host.logs.size() == 4 && host.logs[2] == "12.3 -0.1 -60.2 15 false 20 Stage" && host.logs[3] == "nowhere",
          "position wrong");
      runtime.dispatch("bad", "", alice);
      check(runtime.diagnostics().callback_errors == 1, "fractional lobby number accepted");
      Runtime noread(host, {"u", "1", R"lua(
        extensions.command("try", {cooldown_ms = 0}, function(ctx) extensions.position() end)
      )lua", {"commands"}, {}});
      noread.dispatch("try", "", alice);
      check(noread.diagnostics().callback_errors == 1, "position without lobby.read accepted");
    }

    // --- Objects attached to players ---
    {
      CaelumScenes scenes;
      CaelumObjectSpec hat;
      hat.model = "hat.bml";
      hat.attach = 77;
      hat.bone = 58;
      hat.upright = true;
      hat.y = 1.5;
      hat.rot_x = -10;
      CaelumObjectSpec pet;
      pet.model = "pet.bml";
      pet.motion = "idle.njm";
      pet.move_motion = "walk.njm";
      pet.attach = 78;
      pet.follow = true;
      pet.x = -8;
      pet.z = -6;
      CaelumObjectSpec sign;
      sign.model = "sign.bml";
      sign.rot_z = 15;
      check(scenes.set_object("wardrobe", 5, "hat-77", hat).ok && scenes.set_object("pets", 5, "pet-78", pet).ok &&
              scenes.set_object("decor", 5, "sign", sign).ok, "attached objects refused");
      // Refusals: attachment fields without a player, upright on the root, follow on a bone, move without follow.
      auto bad = sign;
      bad.bone = 58;
      check(std::string(scenes.set_object("x", 5, "a", bad).reason) == "invalid_value", "bone without a player accepted");
      bad = hat;
      bad.bone = -1;
      check(std::string(scenes.set_object("x", 5, "a", bad).reason) == "invalid_value", "upright on the root accepted");
      bad = pet;
      bad.bone = 58;
      check(std::string(scenes.set_object("x", 5, "a", bad).reason) == "invalid_value", "follow on a bone accepted");
      bad = hat;
      bad.move_motion = "walk.njm";
      check(std::string(scenes.set_object("x", 5, "a", bad).reason) == "invalid_value", "move_motion without follow accepted");
      bad = hat;
      bad.bone = 64;
      check(std::string(scenes.set_object("x", 5, "a", bad).reason) == "invalid_value", "bone 64 accepted");
      bad = pet;
      bad.move_motion = "../w.njm";
      check(std::string(scenes.set_object("x", 5, "a", bad).reason) == "invalid_file", "bad move_motion accepted");
      // Who sees what: session 77 is in slot 3, 78 is not in this room.
      auto slot_of = [](uint64_t session) { return session == 77 ? 3 : -1; };
      auto both = parse(scenes.snapshot(5, "L5", 1, 64, slot_of, true));
      check(both.count == 2 && std::string(both.objects[0].id) == "wardrobe:hat-77" && both.objects[0].attach == 3 &&
              both.objects[0].bone == 58 && both.objects[0].upright && both.objects[0].rx == -10.0f && both.objects[0].y == 1.5f,
          "attached hat wrong");
      check(std::string(both.objects[1].id) == "decor:sign" && both.objects[1].attach == -1 && both.objects[1].rz == 15.0f,
          "room object next to an attached one wrong");
      auto everyone = parse(scenes.snapshot(5, "L5", 1, 64, [](uint64_t) { return 7; }, true));
      check(everyone.count == 3 && everyone.objects[1].follow && everyone.objects[1].attach == 7 && everyone.objects[1].bone == -1 &&
              std::string(everyone.objects[1].move) == "walk.njm", "pet wrong");
      auto old_client = parse(scenes.snapshot(5, "L5", 1, 64, slot_of, false));
      check(old_client.count == 1 && std::string(old_client.objects[0].id) == "decor:sign", "attached objects sent to a client without scene.attach");
      // The player leaves: every package's objects attached to them go.
      check(scenes.remove_attached(77) == std::set<uint8_t>({5}) && scenes.remove_attached(77).empty() && scenes.object_count() == 2,
          "remove_attached wrong");
      // A room never grows past CEXT_MAX_SNAPSHOT: 64 objects with long names are cut to what fits.
      CaelumScenes big;
      CaelumObjectSpec longest;
      longest.model = std::string(43, 'm') + ".bml";
      longest.part = std::string(44, 'p') + ".gj";
      longest.motion = std::string(43, 'o') + ".njm";
      longest.move_motion = std::string(43, 'w') + ".njm";
      longest.attach = 1;
      longest.follow = true;
      longest.x = longest.y = longest.z = -999999.999;
      longest.rot_x = longest.rot_y = longest.rot_z = -35999.999;
      for (int z = 0; z < 64; z++) {
        check(big.set_object(std::string(64, 'k'), 9, std::string(29, 'x') + std::to_string(100 + z), longest).ok, "long object refused");
      }
      check(big.set_music(std::string(64, 'k'), 9, {std::string(43, 's') + ".adx", 1795000000000LL, true}).ok, "music refused");
      auto text = big.snapshot(9, "L4294967295", 1795000000000LL, 64, [](uint64_t) { return 11; }, true);
      auto cut = parse(text);
      check(text.size() <= CEXT_MAX_SNAPSHOT && cut.count > 16 && cut.count < 64 && cut.has_music, "long room not cut to the byte limit");
    }
    {
      TestHost host;
      Context alice{42, 1, "Alice", "everyone", "GC_EP3", false, true, ""};
      Runtime runtime(host, {"wardrobe", "1", R"lua(
        extensions.command("hat", {cooldown_ms = 0}, function(ctx)
          local ok, reason = extensions.caelum_object(20, "hat-" .. ctx.client_id, {model = "hat.bml", attach = ctx.client_id,
            bone = "head", upright = true, y = 1.5, rot_x = -10})
          local ok2 = extensions.caelum_object(20, "pet-" .. ctx.client_id, {model = "pet.bml", motion = "idle.njm",
            move_motion = "walk.njm", attach = ctx.client_id, follow = true, x = -8, z = -6})
          extensions.log(tostring(ok) .. " " .. tostring(reason) .. " " .. tostring(ok2))
        end)
        extensions.command("bad1", {cooldown_ms = 0}, function(ctx) extensions.caelum_object(20, "a", {model = "a.bml", attach = "me"}) end)
        extensions.command("bad2", {cooldown_ms = 0}, function(ctx) extensions.caelum_object(20, "a", {model = "a.bml", attach = ctx.client_id, bone = "tail"}) end)
        extensions.command("bad3", {cooldown_ms = 0}, function(ctx) extensions.caelum_object(20, "a", {model = "a.bml", attach = ctx.client_id, follow = 1}) end)
      )lua", {"commands", "caelum.scene"}, {}});
      runtime.dispatch("hat", "", alice);
      check(host.logs.size() == 1 && host.logs[0] == "true nil true", "Lua attach refused");
      auto s = parse(host.scenes.snapshot(20, "L1", 1, 64, [](uint64_t session) { return session == 42 ? 2 : -1; }, true));
      check(s.count == 2 && std::string(s.objects[0].id) == "wardrobe:hat-42" && s.objects[0].attach == 2 && s.objects[0].bone == 58 &&
              s.objects[0].upright && s.objects[0].rx == -10.0f && s.objects[1].follow && std::string(s.objects[1].move) == "walk.njm",
          "Lua attach fields not carried");
      runtime.dispatch("bad1", "", alice);
      runtime.dispatch("bad2", "", alice);
      runtime.dispatch("bad3", "", alice);
      check(runtime.diagnostics().callback_errors == 3, "bad attach, bone or follow accepted");
    }

    // --- Shipped concert package ---
    if (argc > 1) {
      TestHost host;
      host.utc = civil_to_seconds(2026, 11, 28, 19, 50, 0); // Saturday
      Settings settings{{"lobby", setting("stage")}, {"weekday", setting(6)}, {"hour", setting(20)}, {"minute", setting(0)},
          {"duration_minutes", setting(12)}, {"prefetch_minutes", setting(5)}, {"warn_first_minutes", setting(15)},
          {"warn_second_minutes", setting(5)}, {"model", setting("singer.bml")},
          {"model_part", setting("singer_body.gj")}, {"motion", setting("sing.njm")}, {"music", setting("song.adx")},
          {"x", setting(10)}, {"y", setting(0)}, {"z", setting(-60)}, {"rot_y", setting(180)}, {"scale_percent", setting(150)},
          {"announce", setting("Concert time!")}};
      std::set<std::string> grants{"commands", "events", "schedule", "storage", "caelum.scene", "chat.announce", "chat.reply"};
      std::string source = read_file(argv[1]);
      Runtime runtime(host, {"concert", "0.1.0", source, grants, {}, 0, 0, settings});
      runtime.activate("startup");
      check(runtime.schedule_count() == 4 && host.scenes.empty(20), "concert schedules not registered or a show at load");
      auto at = [&](int h, int m, int s) {
        host.utc = civil_to_seconds(2026, 11, 28, h, m, s);
        runtime.run_schedules(clock_time(host.utc, 0));
      };
      at(19, 45, 0); // 15 minutes before: the first reminder
      check(host.announcements.size() == 1 && host.announcements[0] == "A concert starts in 15 minutes in lobby Stage!" &&
              host.scenes.empty(20), "first reminder wrong");
      at(19, 54, 0);
      check(host.scenes.empty(20) && host.announcements.size() == 1, "prefetch or reminder too early");
      at(19, 55, 0); // 5 minutes before: clients load the model, nothing is shown
      auto s = parse(host.scenes.snapshot(20, "L1", 1));
      check(s.count == 1 && std::string(s.objects[0].id) == "concert:performer" && !s.objects[0].show && !s.has_music &&
              std::string(s.objects[0].model) == "singer.bml" && std::string(s.objects[0].part) == "singer_body.gj" &&
              std::string(s.objects[0].motion) == "sing.njm", "prefetch object wrong");
      check(host.announcements.size() == 2 && host.announcements[1] == "A concert starts in 5 minutes in lobby Stage!", "second reminder wrong");
      at(20, 0, 0); // start
      int64_t start_ms = host.utc * 1000;
      s = parse(host.scenes.snapshot(20, "L1", 1));
      check(s.count == 1 && s.objects[0].show && s.objects[0].start_ms == start_ms && s.objects[0].x == 10.0f &&
              s.objects[0].z == -60.0f && s.objects[0].ry == 180.0f && s.objects[0].scale == 1.5f, "performer wrong at the start");
      check(s.has_music && std::string(s.music) == "song.adx" && s.music_start_ms == start_ms && !s.music_loop, "song wrong at the start");
      check(host.announcements.size() == 3 && host.announcements[2] == "Concert time!" && runtime.timer_count() == 1 &&
              runtime.storage().at("show_start") == std::to_string(host.utc), "start bookkeeping wrong");
      // A restart 4 minutes in: the show comes back from its original start and ends on time.
      host.utc += 240;
      host.scenes.remove_owner("concert");
      Runtime restarted(host, {"concert", "0.1.0", source, grants, runtime.storage(), 0, 0, settings});
      restarted.activate("reload");
      s = parse(host.scenes.snapshot(20, "L1", 1));
      check(s.count == 1 && s.objects[0].show && s.objects[0].start_ms == start_ms && s.music_start_ms == start_ms &&
              restarted.timer_count() == 1 && host.announcements.size() == 3, "show not restored after a restart");
      // The weekly rule does not fire on another day; a moderator can start and stop by hand.
      host.utc = civil_to_seconds(2026, 11, 29, 20, 0, 0);
      Runtime sunday(host, {"concert", "0.1.0", source, grants, {}, 0, 0, settings});
      host.scenes.remove_owner("concert");
      sunday.run_schedules(clock_time(host.utc, 0));
      check(host.scenes.empty(20), "show started on the wrong weekday");
      Context mod{7, 1, "Mod", "everyone", "GC_EP3", false, true, ""};
      sunday.dispatch("concert", "start", mod);
      check(!host.scenes.empty(20) && host.replies.back() == "Concert started.", "manual start failed");
      Context mod2 = mod, mod3 = mod; // Other sessions: the command cooldown is per session.
      mod2.client_id = 8;
      mod3.client_id = 9;
      sunday.dispatch("concert", "start", mod2);
      check(host.replies.back().find("could not start") != std::string::npos, "second start accepted");
      sunday.dispatch("concert", "stop", mod3);
      check(host.scenes.empty(20) && host.replies.back() == "Concert stopped." && !sunday.storage().count("show_start"), "manual stop failed");
      check(runtime.diagnostics().callback_errors == 0 && restarted.diagnostics().callback_errors == 0 &&
              sunday.diagnostics().callback_errors == 0, "concert package faulted");
    }

    // --- Package data files (manifest "data_files") ---
    {
      TestHost host;
      Runtime runtime(host, {"t", "1", R"lua(
        local text = extensions.package_text("list.txt")
        extensions.command("show", {cooldown_ms = 0}, function(ctx)
          extensions.log(text .. "|" .. tostring(extensions.package_text("other.txt")) .. "|" .. tostring(extensions.package_text("../x.txt")))
        end)
      )lua", {"commands"}, {}, 0, 0, {}, {{"list.txt", "a b\nc"}}});
      runtime.dispatch("show", "", Context{42, 1, "A", "everyone", "GC_EP3", false, true, ""});
      check(host.logs.size() == 1 && host.logs[0] == "a b\nc|nil|nil", "package_text wrong");
      check(Runtime::valid_data_file_name("pets.txt") && Runtime::valid_data_file_name("my-list_2.txt") &&
              !Runtime::valid_data_file_name("Pets.txt") && !Runtime::valid_data_file_name("../x.txt") &&
              !Runtime::valid_data_file_name("a/b.txt") && !Runtime::valid_data_file_name(".txt") &&
              !Runtime::valid_data_file_name("x.lua") && !Runtime::valid_data_file_name(".hidden.txt"), "data file names wrong");
      Runtime fine(host, {"t", "1", "local x = 1", {}, {}, 0, 0, {}, {{"a.txt", "ok"}}});   // the control: a good file loads
      for (const auto& bad : std::vector<std::map<std::string, std::string>>{
               {{"../x.txt", ""}}, {{"a.txt", std::string("x\0y", 3)}}, {{"a.txt", std::string(64 * 1024 + 1, 'x')}}}) {
        bool refused = false;
        try {
          Runtime r(host, {"t", "1", "local x = 1", {}, {}, 0, 0, {}, bad});
        } catch (const std::exception&) {
          refused = true;
        }
        check(refused, "bad data file accepted");
      }
    }

    // --- Shipped pets and wearhat packages ---
    if (argc > 3) {
      const std::string key = "p0123456789abcdef01234567";
      auto ctx = [&](uint64_t id) { return Context{id, 1, "Alice", "everyone", "GC_EP3", false, true, key}; };
      auto any_slot = [](uint64_t) { return 0; };
      std::set<std::string> grants{"commands", "chat.reply", "events", "lobby.read", "caelum.scene", "players.store"};
      {
        TestHost host;
        Settings settings{{"choices", setting("Sonic : s.bml : body.nj : idle.njm : walk.njm : 60, bad:, dog:d.bml")},
            {"side", setting(9)}, {"behind", setting(6)}};
        // pets.txt as a server admin writes it: comments, CRLF line ends, tabs, "-" for a field left out, a bad line.
        std::map<std::string, std::string> files{{"pets.txt",
            "# name model part idle walk scale\r\n"
            "booma\tbm_ene_booma.bml  booma_body.nj  booma_stand.njm  booma_move.njm  50  # a comment\r\n"
            "\r\n"
            "Rappy  bm_ene_rappy.bml  -  rappy_idle.njm\r\n"
            "broken\r\n"
            "booma  other.bml\r\n"}};
        Runtime pets(host, {"pets", "0.2.0", read_file(argv[2]), grants, {}, 0, 0, settings, files});
        pets.activate("startup");
        auto logged = [&](const std::string& text) {
          return std::any_of(host.logs.begin(), host.logs.end(), [&](const std::string& l) { return l.find(text) != std::string::npos; });
        };
        check(logged("pets.txt line 5") && logged("Pet booma is listed twice (pets.txt line 6)") && logged("setting choices:  bad:"),
            "bad pet lines not logged");
        pets.dispatch("pet", "", ctx(44));
        check(host.replies.back() == "Pets: booma, rappy, sonic, dog. Use: pet <name>, pet off", "pet list wrong");
        pets.dispatch("pet", "booma", ctx(49));
        auto b = parse(host.scenes.snapshot(20, "L1", 1, 64, any_slot, true));
        check(b.count == 1 && std::string(b.objects[0].model) == "bm_ene_booma.bml" && std::string(b.objects[0].part) == "booma_body.nj" &&
                std::string(b.objects[0].motion) == "booma_stand.njm" && std::string(b.objects[0].move) == "booma_move.njm" &&
                b.objects[0].scale == 0.5f, "pet from pets.txt wrong");
        host.scenes.remove_attached(49);
        pets.dispatch("pet", "rappy", ctx(48));
        b = parse(host.scenes.snapshot(20, "L1", 1, 64, any_slot, true));
        check(b.count == 1 && !b.objects[0].part[0] && std::string(b.objects[0].motion) == "rappy_idle.njm" && !b.objects[0].move[0] &&
                b.objects[0].scale == 1.0f, "pets.txt defaults wrong");
        host.scenes.remove_attached(48);
        pets.dispatch("pet", "SONIC", ctx(45));
        auto s = parse(host.scenes.snapshot(20, "L1", 1, 64, any_slot, true));
        check(s.count == 1 && std::string(s.objects[0].id) == "pets:pet-45" && s.objects[0].follow && s.objects[0].x == -9.0f &&
                s.objects[0].z == -6.0f && s.objects[0].scale == 0.6f && std::string(s.objects[0].part) == "body.nj" &&
                std::string(s.objects[0].motion) == "idle.njm" && std::string(s.objects[0].move) == "walk.njm",
            "pet object wrong");
        check(host.replies.back() == "Your sonic follows you now (Caelum players can see it)." &&
                host.players.players[key].values["pet"] == "sonic", "pet not remembered");
        pets.dispatch("pet", "cat", ctx(46));
        check(host.replies.back() == "No pet named cat. Pets: booma, rappy, sonic, dog", "unknown pet reply wrong");
        pets.dispatch("pet", "dog", ctx(43));
        check(host.replies.back() == "Pets can only come along in a lobby.", "pet outside a lobby accepted");
        // The player leaves (the server drops the pet) and enters again as a new session: the pet comes back.
        host.scenes.remove_attached(45);
        pets.emit("lobby_join", ctx(47));
        s = parse(host.scenes.snapshot(20, "L1", 1, 64, any_slot, true));
        check(s.count == 1 && std::string(s.objects[0].id) == "pets:pet-47", "remembered pet not back after lobby_join");
        pets.dispatch("pet", "off", ctx(47));
        check(host.scenes.empty(20) && host.replies.back() == "Your pet went home." && !host.players.players[key].values.count("pet"),
            "pet off wrong");
        pets.emit("lobby_join", ctx(48));
        check(host.scenes.empty(20) && pets.diagnostics().callback_errors == 0, "pets package faulted or brought a pet back");
      }
      {
        TestHost host;
        Settings settings{{"choices", setting("tophat:hat_top.bml::2:80:-10,crown:crown.bml:crown.gj,broken:b.bml::x")}};
        Runtime hats(host, {"wearhat", "0.2.0", read_file(argv[3]), grants, {}, 0, 0, settings});
        hats.activate("startup");
        check(!host.logs.empty() && host.logs[0].find("Hat ignored") != std::string::npos, "bad hat choice not logged");
        hats.dispatch("wearhat", "", ctx(44));
        check(host.replies.back() == "Hats: tophat, crown. Use: wearhat <name>, wearhat off", "hat list wrong");
        hats.dispatch("wearhat", "tophat", ctx(45));
        auto s = parse(host.scenes.snapshot(20, "L1", 1, 64, any_slot, true));
        check(s.count == 1 && std::string(s.objects[0].id) == "wearhat:hat-45" && s.objects[0].bone == CEXT_HEAD_BONE &&
                s.objects[0].upright && !s.objects[0].follow && s.objects[0].y == 2.0f && s.objects[0].scale == 0.8f &&
                s.objects[0].rx == -10.0f && !s.objects[0].part[0], "hat object wrong");
        hats.dispatch("wearhat", "crown", ctx(46));   // another hat replaces nothing of the first player's
        s = parse(host.scenes.snapshot(20, "L1", 1, 64, any_slot, true));
        check(s.count == 2 && s.objects[1].y == 1.5f && s.objects[1].scale == 1.0f && s.objects[1].rx == 0.0f &&
                std::string(s.objects[1].part) == "crown.gj", "hat defaults wrong");
        host.scenes.remove_attached(45);
        host.scenes.remove_attached(46);
        hats.emit("lobby_join", ctx(47));   // the account's last choice: crown
        s = parse(host.scenes.snapshot(20, "L1", 1, 64, any_slot, true));
        check(s.count == 1 && std::string(s.objects[0].id) == "wearhat:hat-47" && std::string(s.objects[0].model) == "crown.bml",
            "remembered hat not back after lobby_join");
        hats.dispatch("wearhat", "off", ctx(47));
        check(host.scenes.empty(20) && host.replies.back() == "Hat off." && hats.diagnostics().callback_errors == 0, "wearhat off wrong");
      }
    }

    // --- The "admin" permission level ---
    {
      TestHost host;
      host.strict = true;
      host.levels[51] = {"moderator", "admin"};
      Runtime admin(host, {"t", "1", R"lua(
        extensions.command("adm", {permission = "admin", cooldown_ms = 0}, function(ctx) extensions.reply("ok") end)
      )lua", {"commands", "chat.reply"}, {}});
      admin.dispatch("adm", "", Context{50, 1, "P", "everyone", "GC_EP3", false, true, ""});
      check(host.replies.size() == 1 && host.replies[0].find("do not have permission") != std::string::npos, "admin command ran for a player");
      admin.dispatch("adm", "", Context{51, 1, "A", "everyone", "GC_EP3", false, true, ""});
      check(host.replies.size() == 2 && host.replies[1] == "ok", "admin command refused to an admin");
    }

    // --- Who may use a pet or a hat (everyone, moderators, admins) ---
    if (argc > 3) {
      const std::string key = "p0123456789abcdef01234567";
      auto ctx = [&](uint64_t id) { return Context{id, 1, "Alice", "everyone", "GC_EP3", false, true, key}; };
      auto any_slot = [](uint64_t) { return 0; };
      std::set<std::string> grants{"commands", "chat.reply", "events", "lobby.read", "caelum.scene", "players.store"};
      auto staffed = [](TestHost& host) {
        host.strict = true;
        host.levels[46] = {"moderator"};
        host.levels[47] = {"moderator", "admin"};
        host.levels[48] = {"moderator", "admin"};
        host.levels[50] = {"moderator"};   // fresh sessions: a command's cooldown is per session
        host.levels[51] = {"moderator", "admin"};
      };
      {
        TestHost host;
        staffed(host);
        std::map<std::string, std::string> files{{"pets.txt",
            "dog d.bml - - - - everyone\nbird b.bml - - - - Moderator\nkitty k.bml - - - - admins\npup p.bml - - - - mods\n"}};
        Settings settings{{"choices", setting("fox:f.bml::::")}, {"side", setting(9)}, {"behind", setting(6)}};
        Runtime pets(host, {"pets", "0.3.0", read_file(argv[2]), grants, {}, 0, 0, settings, files});
        pets.activate("startup");
        check(std::any_of(host.logs.begin(), host.logs.end(), [](const std::string& l) {
          return l.find("pets.txt line 4") != std::string::npos && l.find("who must be") != std::string::npos;
        }), "bad who not logged");
        pets.dispatch("pet", "", ctx(44));
        check(host.replies.back() == "Pets: dog, fox. Use: pet <name>, pet off", "player sees staff pets");
        pets.dispatch("pet", "", ctx(46));
        check(host.replies.back() == "Pets: dog, bird (moderators), fox. Use: pet <name>, pet off", "moderator list wrong");
        pets.dispatch("pet", "", ctx(47));
        check(host.replies.back() == "Pets: dog, bird (moderators), kitty (admins), fox. Use: pet <name>, pet off", "admin list wrong");
        pets.dispatch("pet", "kitty", ctx(45));
        check(host.replies.back() == "The kitty is for admins only." && host.scenes.empty(20), "admin pet given to a player");
        pets.dispatch("pet", "kitty", ctx(50));
        check(host.replies.back() == "The kitty is for admins only." && host.scenes.empty(20), "admin pet given to a moderator");
        pets.dispatch("pet", "kitty", ctx(51));
        check(!host.scenes.empty(20) && host.players.players[key].values["pet"] == "kitty", "admin pet refused to an admin");
        // The account comes back: as a player session no admin pet, as an admin session it is back.
        host.scenes.remove_attached(51);
        pets.emit("lobby_join", ctx(44));
        check(host.scenes.empty(20), "admin pet shown to a session without the right");
        pets.emit("lobby_join", ctx(48));
        auto s = parse(host.scenes.snapshot(20, "L1", 1, 64, any_slot, true));
        check(s.count == 1 && std::string(s.objects[0].model) == "k.bml", "admin pet not back for an admin");
        check(pets.diagnostics().callback_errors == 0, "pets faulted on permissions");
      }
      {
        TestHost host;
        staffed(host);
        std::map<std::string, std::string> files{{"hats.txt",
            "# hats\r\ncap c.bml - - - - -\r\ncrown cr.bml crown.gj 1.2 80 -5 admin\r\nhalo h.bml - 3 - - moderator\r\nbad x.bml - high\r\n"}};
        Runtime hats(host, {"wearhat", "0.2.0", read_file(argv[3]), grants, {}, 0, 0, {{"choices", setting("")}}, files});
        hats.activate("startup");
        check(std::any_of(host.logs.begin(), host.logs.end(), [](const std::string& l) { return l.find("hats.txt line 5") != std::string::npos; }),
            "bad hat line not logged");
        hats.dispatch("wearhat", "", ctx(44));
        check(host.replies.back() == "Hats: cap. Use: wearhat <name>, wearhat off", "player sees staff hats");
        hats.dispatch("wearhat", "", ctx(47));
        check(host.replies.back() == "Hats: cap, crown (admins), halo (moderators). Use: wearhat <name>, wearhat off", "admin hat list wrong");
        hats.dispatch("wearhat", "halo", ctx(45));
        check(host.replies.back() == "The halo is for moderators only." && host.scenes.empty(20), "moderator hat given to a player");
        hats.dispatch("wearhat", "halo", ctx(46));
        auto s = parse(host.scenes.snapshot(20, "L1", 1, 64, any_slot, true));
        check(s.count == 1 && s.objects[0].y == 3.0f && s.objects[0].upright && s.objects[0].bone == CEXT_HEAD_BONE, "moderator hat wrong");
        hats.dispatch("wearhat", "crown", ctx(51));
        s = parse(host.scenes.snapshot(20, "L1", 1, 64, any_slot, true));
        check(s.count == 2 && std::string(s.objects[1].part) == "crown.gj" && s.objects[1].y == 1.2f && s.objects[1].scale == 0.8f &&
                s.objects[1].rx == -5.0f, "hat from hats.txt wrong");
        check(hats.diagnostics().callback_errors == 0, "wearhat faulted on permissions");
      }
    }

    std::cout << "extension caelum-scene tests passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "extension caelum-scene tests failed: " << e.what() << "\n";
    return 1;
  }
}
