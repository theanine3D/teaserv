// Clock, schedules, per-player data, boards, Meseta grants and the Episode 3
// battle event, through the real Lua runtime with a synthetic host. Also runs
// the shipped daily-reward and player-of-the-period packages.

#include "Runtime.hh"

#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <thread>

using namespace teaserv::extensions;

static void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

template <typename Fn>
static void rejects(Fn fn, const char* message) {
  bool rejected = false;
  try {
    fn();
  } catch (const std::exception&) {
    rejected = true;
  }
  check(rejected, message);
}

static std::string key_for(int n) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "p%024x", n);
  return buffer;
}

struct Grant {
  std::string owner, key, id, reason;
  int64_t amount;
};

struct TestHost : Host {
  bool live = true;
  int64_t utc = 0;
  PlayerData data;
  std::set<std::string> online;
  std::set<std::string> known;
  std::vector<Grant> grants;
  std::vector<std::string> replies, announcements, logs;
  bool authorized(const Context& context) const override { return live && context.allow_player_actions; }
  void reply(const Context&, const std::string& text) override { replies.emplace_back(text); }
  void broadcast(const Context&, const std::string&) override {}
  bool whisper(const Context&, uint64_t, const std::string&) override { return false; }
  void log(const std::string&, const std::string& text) override { logs.emplace_back(text); }
  bool command_reserved(const std::string&) const override { return false; }
  AnnouncementResult announce(const std::string&, const std::string& text) override {
    announcements.emplace_back(text);
    return {true, nullptr};
  }
  ClockTime now() const override { return clock_time(utc, 0); }
  PlayerData* player_data(const std::string&) override { return &data; }
  MesetaResult meseta_grant(const std::string& owner, const std::string& key, int64_t amount, const std::string& id,
      const std::string& reason) override {
    if (!known.count(key)) return {false, "not_ep3"};
    if (data.grant_id_used(key, id)) return {true, "duplicate"};
    data.remember_grant_id(key, id);
    grants.emplace_back(Grant{owner, key, id, reason, amount});
    return online.count(key) ? MesetaResult{true, "granted", amount} : MesetaResult{true, "queued"};
  }
};

static std::string read_file(const char* path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error(std::string("cannot read ") + path);
  return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

static Setting setting(int64_t value) { return {"integer", SettingValue{value}}; }
static Setting setting(const char* value) { return {"string", SettingValue{std::string(value)}}; }
static Setting setting_bool(bool value) { return {"boolean", SettingValue{value}}; }

static InfoTable battle(int winner, const std::vector<std::pair<int, std::string>>& players, int64_t rounds) {
  // players: (team, key or "" for COM)
  std::vector<InfoTable> teams(2);
  std::vector<std::vector<InfoTable>> members(2);
  int slot = 0;
  for (const auto& [team, key] : players) {
    InfoTable p;
    p.add("slot", InfoValue{static_cast<int64_t>(slot++)});
    p.add("is_com", InfoValue{key.empty()});
    p.add("name", InfoValue{key.empty() ? std::string("COM") : "Name" + key.substr(22)});
    if (!key.empty()) p.add("player_key", InfoValue{key});
    members[team].emplace_back(std::move(p));
  }
  for (int team = 0; team < 2; team++) {
    teams[team].add("team", InfoValue{static_cast<int64_t>(team)});
    teams[team].add("won", InfoValue{team == winner});
    teams[team].add("players", InfoValue{std::move(members[team])});
  }
  InfoTable info;
  info.add("event", InfoValue{std::string("ep3_battle_ended")});
  info.add("lobby_id", InfoValue{std::string("77")});
  info.add("winner_team", InfoValue{static_cast<int64_t>(winner)});
  info.add("tournament", InfoValue{false});
  info.add("rounds", InfoValue{rounds});
  info.add("duration_s", InfoValue{static_cast<int64_t>(300)});
  info.add("teams", InfoValue{std::move(teams)});
  return info;
}

int main(int argc, char** argv) {
  try {
    // --- Calendar arithmetic ---
    {
      auto t = clock_time(0, 0);
      check(t.year == 1970 && t.month == 1 && t.day == 1 && t.weekday == 4 && t.iso_week == 1 && t.yday == 1, "epoch fields wrong");
      int64_t s = civil_to_seconds(2026, 11, 24, 12, 34, 56);
      t = clock_time(s, 0);
      check(t.year == 2026 && t.month == 11 && t.day == 24 && t.hour == 12 && t.minute == 34 && t.second == 56, "civil round trip wrong");
      check(t.weekday == 2 && t.iso_year == 2026 && t.iso_week == 48, "2026-11-24 must be Tuesday of ISO week 48");
      t = clock_time(civil_to_seconds(2021, 1, 3, 0, 0, 0), 0);
      check(t.weekday == 7 && t.iso_year == 2020 && t.iso_week == 53, "2021-01-03 must be Sunday of 2020-W53");
      t = clock_time(civil_to_seconds(2024, 12, 30, 0, 0, 0), 0);
      check(t.weekday == 1 && t.iso_year == 2025 && t.iso_week == 1, "2024-12-30 must be Monday of 2025-W01");
      t = clock_time(civil_to_seconds(2024, 2, 29, 23, 59, 59), 0);
      check(t.month == 2 && t.day == 29 && t.yday == 60, "leap day wrong");
      // A zone offset moves the calendar fields, not utc.
      t = clock_time(civil_to_seconds(2026, 11, 24, 23, 30, 0), 3600);
      check(t.day == 25 && t.hour == 0 && t.minute == 30 && t.utc == civil_to_seconds(2026, 11, 24, 23, 30, 0), "offset wrong");
      t = clock_time(civil_to_seconds(2026, 1, 1, 0, 30, 0), -3600);
      check(t.year == 2025 && t.month == 12 && t.day == 31 && t.hour == 23, "negative offset wrong");

      ScheduleSpec weekly;
      weekly.minutes = {0};
      weekly.hours = {20};
      weekly.weekdays = {6, 7};
      weekly.validate();
      check(weekly.matches(clock_time(civil_to_seconds(2026, 11, 28, 20, 0, 5), 0)), "Saturday 20:00 not matched");
      check(!weekly.matches(clock_time(civil_to_seconds(2026, 11, 27, 20, 0, 0), 0)), "Friday matched");
      check(!weekly.matches(clock_time(civil_to_seconds(2026, 11, 28, 20, 1, 0), 0)), "20:01 matched");
      ScheduleSpec every;
      every.every_minutes = 15;
      every.validate();
      check(every.matches(clock_time(civil_to_seconds(2026, 11, 28, 20, 45, 0), 0)) &&
              !every.matches(clock_time(civil_to_seconds(2026, 11, 28, 20, 46, 0), 0)), "every_minutes wrong");
      ScheduleSpec bad;
      rejects([&] { bad.validate(); }, "empty schedule accepted");
      bad.minutes = {60};
      rejects([&] { bad.validate(); }, "minute 60 accepted");
      bad = ScheduleSpec{};
      bad.every_minutes = 5;
      bad.hours = {1};
      rejects([&] { bad.validate(); }, "mixed every_minutes accepted");
    }

    // --- Player data ---
    {
      PlayerData data;
      auto a = key_for(1), b = key_for(2), c = key_for(3);
      check(valid_player_key(a) && !valid_player_key("p123") && !valid_player_key("x" + a.substr(1)) &&
              !valid_player_key("p00000000000000000000000G"), "player key validation wrong");
      check(!data.get(a, "x") && !data.dirty, "empty store returned a value");
      data.set(a, "x", std::string("1"));
      check(data.dirty && *data.get(a, "x") == "1", "player value lost");
      rejects([&] { data.set("bad", "x", std::string("1")); }, "invalid key accepted");
      rejects([&] { data.set(a, "Bad Name", std::string("1")); }, "invalid field name accepted");
      rejects([&] { data.set(a, "y", std::string(257, 'v')); }, "oversized value accepted");
      for (int z = 0; z < 15; z++) data.set(a, "k" + std::to_string(z), std::string("v"));
      rejects([&] { data.set(a, "one-too-many", std::string("v")); }, "per-player key quota not enforced");
      data.set(a, "x", std::string("2")); // Replacing needs no new slot.
      data.set(a, "x", std::nullopt);
      check(!data.get(a, "x"), "delete failed");

      check(!data.grant_id_used(b, "g1"), "unused grant ID reported used");
      data.remember_grant_id(b, "g1");
      check(data.grant_id_used(b, "g1") && !data.grant_id_used(c, "g1"), "grant ID memory wrong");
      for (int z = 0; z < 40; z++) data.remember_grant_id(b, "n" + std::to_string(z));
      check(!data.grant_id_used(b, "g1") && data.grant_id_used(b, "n39") && data.players.at(b).grant_ids.size() == 32,
          "grant ID window not bounded");

      check(data.board_add("wins", a, 2, "Alice") == 2, "board add wrong");
      data.board_add("wins", b, 3, "Bob");
      data.board_add("wins", c, 1, "Cid");
      data.board_add("wins", c, 2, ""); // Cid reaches 3 after Bob: Bob stays ahead; empty name keeps the old one.
      auto top = data.board_top("wins", 10);
      check(top.size() == 3 && top[0].name == "Bob" && top[1].name == "Cid" && top[2].name == "Alice" && top[1].score == 3,
          "board order or tie rule wrong");
      check(data.board_top("wins", 1).size() == 1 && *data.board_get("wins", a) == 2 && !data.board_get("none", a), "board read wrong");
      rejects([&] { data.board_top("wins", 51); }, "board_top count limit not enforced");
      check(!data.board_reset("none") && data.board_reset("wins"), "board reset result wrong");
      check(data.board_top("wins", 10).empty(), "reset board not empty");
      auto previous = data.board_top("wins.previous", 2);
      check(previous.size() == 2 && previous[0].name == "Bob", "previous period not kept");
      for (int z = 0; z < 7; z++) data.board_add("b" + std::to_string(z), a, 1, "");
      rejects([&] { data.board_add("ninth", a, 1, ""); }, "board quota not enforced");
      data.validate();
    }

    Context alice{42, 1, "Alice", "everyone", "GC_EP3", false, true, key_for(1)};

    // --- Runtime API scope and grants ---
    {
      TestHost host;
      host.utc = civil_to_seconds(2026, 11, 24, 10, 0, 0);
      rejects([&] { Runtime r(host, {"t", "1", "extensions.schedule('a', {minute = 0}, function() end)", {"events"}, {}}); },
          "schedule without its grant accepted");
      rejects([&] { Runtime r(host, {"t", "1", "extensions.schedule('a', {minute = 0, bogus = 1}, function() end)", {"schedule"}, {}}); },
          "unknown schedule field accepted");
      rejects([&] { Runtime r(host, {"t", "1", "extensions.schedule('a', {hour = 3}, function() end)", {"schedule"}, {}}); },
          "schedule without minute accepted");
      rejects([&] { Runtime r(host, {"t", "1", "extensions.player_set('" + key_for(1) + "', 'a', 'b')", {"players.store"}, {}}); },
          "player store usable during initialization");
      rejects([&] { Runtime r(host, {"t", "1", "extensions.on('ep3_battle_ended', function() end)", {}, {}}); },
          "battle event registered without events grant");

      Runtime runtime(host, {"t", "1", R"lua(
        local fired = 0
        extensions.schedule("tick", {minute = {0, 30}, hour = 10}, function(info)
          fired = fired + 1
          extensions.log("schedule " .. info.name .. " " .. info.event .. " " .. info.utc .. " n=" .. fired)
          local id = extensions.service_after(50, function(ctx)
            extensions.log("service timer can_act=" .. tostring(ctx.can_act))
            local ok, status = extensions.meseta_grant("self", 5, "x", "no player here")
            extensions.log("self grant without player: " .. tostring(ok) .. " " .. status)
          end)
          extensions.log("timer id type " .. math.type(id))
        end)
        extensions.command("clock", {cooldown_ms = 0}, function(ctx)
          local t = extensions.now()
          extensions.reply(t.year .. "-" .. t.month .. "-" .. t.day .. " w" .. t.weekday .. " W" .. t.iso_week .. " d" .. t.day_number)
          extensions.reply("key " .. ctx.player_key)
        end)
        extensions.command("late", {cooldown_ms = 0}, function(ctx)
          extensions.schedule("late", {minute = 1}, function() end)
        end)
        extensions.command("badkey", {cooldown_ms = 0}, function(ctx)
          extensions.player_get("not-a-key", "a")
        end)
        extensions.command("nogrant", {cooldown_ms = 0}, function(ctx)
          extensions.meseta_grant("self", 1, "id", "r")
        end)
      )lua", {"commands", "schedule", "chat.reply", "players.store", "economy.meseta"}, {}});
      check(runtime.schedule_count() == 1, "schedule not registered");
      runtime.dispatch("clock", "", alice);
      check(host.replies.size() == 2 && host.replies[0] == "2026-11-24 w2 W48 d" + std::to_string(host.utc / 86400) &&
              host.replies[1] == "key " + key_for(1), "now() or ctx.player_key wrong");
      runtime.run_schedules(clock_time(civil_to_seconds(2026, 11, 24, 10, 15, 0), 0));
      check(host.logs.empty(), "schedule fired at a non-matching minute");
      runtime.run_schedules(clock_time(civil_to_seconds(2026, 11, 24, 10, 30, 0), 0));
      check(host.logs.size() == 2 && host.logs[0].find("schedule tick schedule") == 0 && host.logs[1] == "timer id type integer",
          "schedule did not fire with metadata");
      check(runtime.timer_count() == 1, "service timer not pending");
      // A session leaving must not cancel a service timer (it has no player).
      runtime.cancel_client(0);
      host.live = false; // No player authorization exists for service timers.
      std::this_thread::sleep_for(std::chrono::milliseconds(70));
      runtime.tick();
      check(runtime.timer_count() == 0 && host.logs.size() == 4 && host.logs[2] == "service timer can_act=false",
          "service timer did not run under extension authority");
      check(host.logs[3] == "self grant without player: false unavailable",
          "self grant outside a player callback was not refused");
      host.live = true;
      size_t errors = runtime.diagnostics().callback_errors;
      runtime.dispatch("late", "", alice);
      check(runtime.diagnostics().callback_errors == errors + 1 && runtime.schedule_count() == 1, "late schedule registration accepted");
      runtime.dispatch("badkey", "", alice);
      check(runtime.diagnostics().callback_errors == errors + 2, "invalid player key accepted");
    }
    {
      TestHost host;
      Runtime runtime(host, {"t", "1", R"lua(
        extensions.command("nogrant", {cooldown_ms = 0}, function(ctx)
          extensions.meseta_grant("self", 1, "id", "r")
        end)
        extensions.command("boards", {cooldown_ms = 0}, function(ctx)
          extensions.board_add("b", ctx.player_key, 1, "x")
        end)
      )lua", {"commands"}, {}});
      runtime.dispatch("nogrant", "", alice);
      runtime.dispatch("boards", "", alice);
      check(runtime.diagnostics().callback_errors == 2 && host.grants.empty() && host.data.boards.empty(),
          "economy or boards usable without grants");
    }

    // --- Shipped daily-reward package ---
    if (argc > 1) {
      TestHost host;
      host.known = {key_for(1), key_for(2)};
      host.online = host.known;
      host.utc = civil_to_seconds(2026, 11, 24, 9, 0, 0);
      Settings settings{{"meseta_per_day", setting(1000)}, {"streak_bonus", setting(100)}, {"streak_max_days", setting(3)},
          {"message", setting("Daily login reward:")}};
      Runtime runtime(host, {"daily-reward", "0.1.0", read_file(argv[1]),
          {"events", "players.store", "economy.meseta", "chat.reply"}, {}, 0, 0, settings});
      runtime.emit("lobby_join", alice);
      check(host.grants.size() == 1 && host.grants[0].amount == 1000 && host.grants[0].key == key_for(1) &&
              host.replies.size() == 1 && host.replies[0] == "Daily login reward: 1000 Meseta", "first daily reward wrong");
      runtime.emit("lobby_join", alice);
      host.utc += 3600 * 10;
      runtime.emit("lobby_join", alice);
      check(host.grants.size() == 1 && host.replies.size() == 1, "reward repeated on the same day");
      Context game = alice;
      game.is_game = true;
      Context bb = alice;
      bb.client_version = "BB_V4";
      bb.player_key = key_for(2);
      Context nokey = alice;
      nokey.player_key = "";
      host.utc = civil_to_seconds(2026, 11, 25, 0, 0, 1);
      runtime.emit("lobby_join", game);
      runtime.emit("lobby_join", bb);
      runtime.emit("lobby_join", nokey);
      check(host.grants.size() == 1, "reward given in a game, to a non-Episode 3 client, or without a key");
      runtime.emit("lobby_join", alice);
      check(host.grants.size() == 2 && host.grants[1].amount == 1100 && host.replies.back() == "Daily login reward: 1100 Meseta (day 2)",
          "second-day streak wrong");
      host.utc += 86400;
      runtime.emit("lobby_join", alice);
      host.utc += 86400;
      runtime.emit("lobby_join", alice);
      check(host.grants.size() == 4 && host.grants[2].amount == 1200 && host.grants[3].amount == 1200, "streak cap wrong");
      host.utc += 86400 * 3;
      runtime.emit("lobby_join", alice);
      check(host.grants.size() == 5 && host.grants[4].amount == 1000, "streak not reset after a missed day");
      // Lost player record: the grant ID still prevents a second payout that day.
      host.data.set(key_for(1), "last_day", std::nullopt);
      runtime.emit("lobby_join", alice);
      check(host.grants.size() == 5, "grant ID did not prevent a duplicate payout");
      // The leave event and read-only events never pay.
      runtime.emit("client_login", alice);
      check(host.grants.size() == 5 && runtime.diagnostics().callback_errors == 0, "daily-reward faulted or paid at login");
    }

    // --- Shipped player-of-the-period package ---
    if (argc > 2) {
      TestHost host;
      auto a = key_for(1), b = key_for(2), c = key_for(3);
      host.known = {a, b, c};
      host.online = {a};
      host.utc = civil_to_seconds(2026, 11, 24, 9, 0, 0); // Tuesday, 2026-W48
      Settings settings{{"period", setting("week")}, {"prize", setting(50000)}, {"count_com_wins", setting_bool(false)},
          {"min_rounds", setting(3)}, {"max_wins_per_day", setting(2)}, {"title", setting("Player of the Week")}};
      std::set<std::string> grants{"commands", "events", "schedule", "storage", "players.store", "boards", "economy.meseta",
          "chat.announce", "chat.reply"};
      Runtime runtime(host, {"player-of-the-period", "0.1.0", read_file(argv[2]), grants, {}, 0, 0, settings});
      runtime.activate("startup");
      check(runtime.storage().at("period") == "2026-W48" && host.grants.empty(), "first load must only record the period");

      runtime.emit_info("ep3_battle_ended", battle(0, {{0, a}, {1, b}}, 10)); // a beats b
      runtime.emit_info("ep3_battle_ended", battle(1, {{0, a}, {1, b}}, 10)); // b beats a
      runtime.emit_info("ep3_battle_ended", battle(1, {{0, a}, {1, b}}, 10)); // b beats a
      runtime.emit_info("ep3_battle_ended", battle(1, {{0, a}, {1, b}}, 10)); // over b's daily limit of 2
      runtime.emit_info("ep3_battle_ended", battle(0, {{0, c}, {1, ""}}, 10)); // COM opponent: not counted
      runtime.emit_info("ep3_battle_ended", battle(0, {{0, c}, {1, b}}, 2)); // too short
      runtime.emit_info("ep3_battle_ended", battle(0, {{0, a}, {0, c}, {1, b}, {1, ""}}, 8)); // 2v2: both winners count
      check(*host.data.board_get("wins", a) == 2 && *host.data.board_get("wins", b) == 2 && *host.data.board_get("wins", c) == 1,
          "win counting rules wrong");
      runtime.dispatch("ranking", "", alice);
      check(!host.replies.empty() && host.replies[0].find("1. Name002 - 2") != std::string::npos &&
              host.replies[0].find("2. Name001 - 2") != std::string::npos, "ranking output or tie order wrong");

      // Same week: the hourly check changes nothing.
      host.utc = civil_to_seconds(2026, 11, 29, 23, 0, 0);
      runtime.run_schedules(clock_time(host.utc, 0));
      check(host.grants.empty() && host.announcements.empty(), "settled before the period ended");
      // Monday 00:00: b reached 2 wins first, is offline, and gets a queued prize.
      host.utc = civil_to_seconds(2026, 11, 30, 0, 0, 0);
      runtime.run_schedules(clock_time(host.utc, 0));
      check(host.grants.size() == 1 && host.grants[0].key == b && host.grants[0].amount == 50000 &&
              host.grants[0].id == "prize-2026-W48", "winner prize wrong");
      check(host.announcements.size() == 1 && host.announcements[0] == "Player of the Week: Name002 (2 wins) wins 50000 Meseta!",
          "winner announcement wrong");
      check(host.data.board_top("wins", 5).empty() && host.data.board_top("wins.previous", 5).size() == 3 &&
              runtime.storage().at("period") == "2026-W49", "period not rolled over");
      runtime.run_schedules(clock_time(host.utc + 3600, 0));
      check(host.grants.size() == 1 && host.announcements.size() == 1, "period settled twice");

      // Restart during a later period with a stale stored period: package_load catches up.
      runtime.emit_info("ep3_battle_ended", battle(0, {{0, a}, {1, b}}, 5));
      host.utc = civil_to_seconds(2026, 12, 20, 12, 0, 0);
      Package again{"player-of-the-period", "0.1.0", read_file(argv[2]), grants, runtime.storage(), 0, 0, settings};
      Runtime restarted(host, std::move(again));
      restarted.activate("startup");
      check(host.grants.size() == 2 && host.grants[1].key == a && host.grants[1].id == "prize-2026-W49" &&
              restarted.storage().at("period") == "2026-W51", "restart catch-up wrong");
      // An empty period announces nothing.
      host.utc = civil_to_seconds(2026, 12, 28, 0, 0, 0);
      restarted.run_schedules(clock_time(host.utc, 0));
      check(host.grants.size() == 2 && host.announcements.size() == 2, "empty period paid or announced");
      check(runtime.diagnostics().callback_errors == 0 && restarted.diagnostics().callback_errors == 0, "package faulted");
    }

    std::cout << "extension player-data tests passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "extension player-data tests failed: " << e.what() << "\n";
    return 1;
  }
}
