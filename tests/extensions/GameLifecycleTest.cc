#include "Runtime.hh"

#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

using namespace teaserv::extensions;

static void check(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
template <typename Fn>
static void rejects(Fn fn) {
  bool rejected = false;
  try {
    fn();
  } catch (const std::exception&) {
    rejected = true;
  }
  check(rejected, "invalid game event was accepted");
}
struct TestHost : Host {
  size_t actions = 0;
  mutable size_t queries = 0;
  std::vector<std::string> logs;
  bool authorized(const Context&) const override { return true; }
  void reply(const Context&, const std::string&) override { ++actions; }
  void broadcast(const Context&, const std::string&) override { ++actions; }
  bool whisper(const Context&, uint64_t, const std::string&) override {
    ++actions;
    return true;
  }
  void log(const std::string&, const std::string& text) override { logs.emplace_back(text); }
  bool command_reserved(const std::string&) const override { return false; }
  std::optional<LobbyView> lobby_view(const Context&) const override {
    ++queries;
    return {};
  }
  SceneCapabilities scene_capabilities(const Context&) const override {
    ++queries;
    return {true, true, true, true};
  }
  SceneAssetSupport scene_asset_support(const Context&, const std::string&, const std::string&) const override {
    ++queries;
    return {true, nullptr, "primary"};
  }
  SceneResult scene_spawn(const Context&, const std::string&, const std::string&, const std::string&, const std::string&, uint32_t) override {
    ++actions;
    return {true};
  }
  SceneResult scene_remove(const Context&, const std::string&, const std::string&) override {
    ++actions;
    return {true};
  }
  SceneResult scene_event(const Context&, const std::string&, int) override {
    ++actions;
    return {true};
  }
};
static Package package(const std::string& source) {
  Package result{"games", "1", source,
      {"events", "commands", "storage", "timers", "chat.reply", "chat.broadcast", "chat.whisper", "lobby.read", "scene.media", "scene.event"}, {}};
  result.settings = resolve_settings({{"tag", {"string", false, std::string("games")}}}, {});
  return result;
}

int main(int argc, char** argv) {
  try {
    GameLifecycle games;
    GameAdmission valid{true, false, false, true, true, false, true, true};
    check(!games.begin(0, valid) && !games.begin(-1, valid), "invalid game ID admitted");
    check(!games.end(7), "unknown game produced a close");
    for (auto field : {&GameAdmission::is_game, &GameAdmission::spectator, &GameAdmission::playback,
             &GameAdmission::authenticated, &GameAdmission::connected, &GameAdmission::proxy,
             &GameAdmission::local_session, &GameAdmission::current_member}) {
      auto bad = valid;
      bad.*field = !(bad.*field);
      check(!GameLifecycle::eligible(bad) && !games.begin(7, bad) && !games.end(7), "ineligible or stale game member was admitted");
    }
    check(games.begin(7, valid) && !games.begin(7, valid), "multiple joins repeated game_opened");
    check(games.begin(8, valid), "independent primary game was lost");
    // No occupancy-based reset: a persistent game can empty and refill.
    check(!games.begin(7, valid), "persistent game refill repeated admission");
    check(games.end(7) && !games.end(7) && games.end(8), "close was duplicated or consumed another game");
    check(games.begin(7, valid) && games.end(7), "new registry lifetime could not use a retired ID");

    {
      GameLifecycle ready;
      check(!ready.member_joined(7, 42) && !ready.join_ready(7, 42, valid, true, true), "unknown game acquired a ready member");
      ready.begin(7, valid);
      ready.begin(8, valid);
      check(!ready.member_joined(7, 0), "zero client identity acquired readiness tracking");
      check(ready.member_joined(7, 42) && !ready.member_joined(7, 42), "duplicate join rearmed readiness");
      check(!ready.join_ready(8, 42, valid, true, true) && !ready.join_ready(7, 43, valid, true, true),
          "readiness crossed game or client identities");
      for (auto field : {&GameAdmission::is_game, &GameAdmission::spectator, &GameAdmission::playback,
               &GameAdmission::authenticated, &GameAdmission::connected, &GameAdmission::proxy,
               &GameAdmission::local_session, &GameAdmission::current_member}) {
        auto bad = valid;
        bad.*field = !(bad.*field);
        check(!ready.join_ready(7, 42, bad, true, true), "ineligible readiness was admitted");
      }
      check(!ready.join_ready(7, 42, valid, false, true) && !ready.join_ready(7, 42, valid, true, false),
          "repeated acknowledgement or quest-ready alias became initial join readiness");
      check(ready.join_ready(7, 42, valid, true, true) && !ready.join_ready(7, 42, valid, true, true),
          "readiness was missing or duplicated after rejected observations");
      check(!ready.member_joined(7, 42) && !ready.join_ready(7, 42, valid, true, true), "duplicate membership rearmed a ready client");
      ready.member_joined(7, 43);
      ready.member_left(7, 42);
      check(!ready.join_ready(7, 42, valid, true, true) && ready.join_ready(7, 43, valid, true, true), "leave damaged another member");
      check(ready.member_joined(7, 42) && ready.join_ready(7, 42, valid, true, true), "leave/rejoin did not start fresh readiness");
      ready.member_joined(8, 42);
      ready.forget_client(42); // Disconnect or proxy handoff, even with stale multiple-game tracking.
      check(!ready.join_ready(7, 42, valid, true, true) && !ready.join_ready(8, 42, valid, true, true), "disconnect retained readiness");
      ready.member_joined(7, 44);
      ready.end(7);
      check(!ready.join_ready(7, 44, valid, true, true), "closed game emitted readiness");
      ready.begin(7, valid);
      check(!ready.join_ready(7, 44, valid, true, true), "new registry lifetime inherited readiness");
      check(ready.member_joined(7, 44) && ready.join_ready(7, 44, valid, true, true), "fresh game lifetime could not observe readiness");
    }

    GameSnapshot snapshot{INT64_MAX, "Primary game", 4, 2, 3};
    validate_game_snapshot(snapshot);
    for (int z = 0; z < 7; z++) {
      auto bad = snapshot;
      switch (z) {
        case 0:
          bad.lobby_id = 0;
          break;
        case 1:
          bad.name.assign(257, 'x');
          break;
        case 2:
          bad.name = std::string("a\0b", 3);
          break;
        case 3:
          bad.episode = 0;
          break;
        case 4:
          bad.episode = 5;
          break;
        case 5:
          bad.mode = 4;
          break;
        case 6:
          bad.difficulty = 255;
          break;
      }
      rejects([&] { validate_game_snapshot(bad); });
    }
    TestHost host;
    auto definition = package(R"(
      local retained
      extensions.on('game_opened',function(info, second)
        assert(second == nil and info.event == 'game_opened' and info.reason == 'first_join')
        assert(info.lobby_id == '9223372036854775807' and info.name == 'Primary game')
        assert(info.episode == 4 and info.mode == 2 and info.difficulty == 3 and info.quest_number == nil and info.quest_joinable == nil)
        assert(info.client_id == nil and info.account_id == nil and info.password == nil and info.permission == nil)
        assert(extensions.setting('tag') == 'games' and extensions.lobby() == nil)
        local caps = extensions.scene_capabilities()
        assert(not extensions.scene_asset_support('asset').supported)
        assert(not caps.native_ep3_media and not caps.lobby_events and caps.max_media_compressed_bytes == 0)
        extensions.storage_set('opened','1')
        extensions.log('opened')
        info.lobby_id, info.name, info.can_act = '1', 'forged', true
        retained = info
      end)
      extensions.on('game_closed',function(info)
        assert(info.event == 'game_closed' and info.reason == 'removed')
        assert(info.lobby_id == '9223372036854775807' and info.name == 'Renamed game')
        assert(info.quest_number == 4294967295 and info.quest_joinable == false)
        assert(retained.name == 'forged' and extensions.storage_get('opened') == '1')
        assert(extensions.lobby() == nil)
        extensions.storage_set('closed','1')
        extensions.log('closed')
      end)
      extensions.command('normal',{cooldown_ms=0},function(ctx, args)
        assert(ctx.client_id == '42' and ctx.lobby_id == '7' and args == 'normal' and ctx.can_act)
        extensions.reply('normal')
      end)
    )");
    Runtime runtime(host, definition);
    runtime.emit("game_opened", {42, 7, "Alice"});
    runtime.emit("game_closed", {42, 7, "Alice"});
    check(runtime.diagnostics().callbacks == 0, "generic player events forged a game snapshot");
    rejects([&] { runtime.emit_game("game_ready", snapshot); });
    runtime.emit_game("game_opened", snapshot);
    check(runtime.diagnostics().callback_errors == 0 && runtime.storage().at("opened") == "1" && snapshot.name == "Primary game", "open metadata/storage isolation failed");
    snapshot.name = "Renamed game";
    snapshot.quest_number = UINT32_MAX;
    runtime.emit_game("game_closed", snapshot);
    check(runtime.diagnostics().callback_errors == 0 && runtime.storage().at("closed") == "1" && host.queries == 0 && host.actions == 0,
        "close snapshot used stale data or gained live authority");
    runtime.dispatch("normal", "normal", {42, 7, "Alice"});
    check(host.actions == 1 && runtime.diagnostics().callback_errors == 0, "game scope leaked into player callbacks");
    auto no_events = definition;
    no_events.grants.erase("events");
    rejects([&] { Runtime denied(host, no_events); });
    auto no_storage = package("extensions.on('game_closed',function() extensions.storage_set('key','x') end)");
    no_storage.grants.erase("storage");
    Runtime denied_storage(host, no_storage);
    denied_storage.emit_game("game_closed", snapshot);
    check(denied_storage.storage().empty() && denied_storage.diagnostics().callback_errors == 1, "game observation bypassed storage grant");

    for (const auto& event : {"game_opened", "game_closed", "quest_assigned", "quest_map_loaded", "quest_load_acknowledged", "quest_join_acknowledged"}) {
      for (const auto& action : {"extensions.reply('x')", "extensions.broadcast('x')", "extensions.whisper('1','x')",
               "extensions.after(50,function() end)", "extensions.cancel(1)", "extensions.scene_spawn('x','y','z')",
               "extensions.every(50,function() end)",
               "extensions.scene_spawn('x','y','z',50)",
               "extensions.scene_remove('x')", "extensions.scene_event(1)",
               "extensions.command('late',{},function() end)", "extensions.on('lobby_join',function() end)", "extensions.migrate(function() end)"}) {
        auto source = std::string("extensions.on('") + event + "',function(info) info.can_act=true; " + action + " end)";
        Runtime bad(host, package(source));
        auto actions = host.actions;
        for (size_t z = 0; z < 3; z++) {
          if (std::string(event) == "quest_load_acknowledged")
            bad.emit_quest_acknowledgement({snapshot, 42, "GC_EP3"});
          else if (std::string(event) == "quest_join_acknowledged") {
            auto joinable = snapshot;
            joinable.quest_joinable = true;
            bad.emit_quest_acknowledgement({joinable, 42, "GC_EP3", QuestAcknowledgementKind::JOINABLE_PLAYER_VISIBLE});
          } else
            bad.emit_game(event, snapshot);
        }
        check(!bad.enabled() && bad.diagnostics().callback_errors == 3 && host.actions == actions, "game callback escaped read-only scope or fault containment");
        check(runtime.enabled(), "faulted observer damaged another package");
      }
    }
    Runtime runaway(host, package("extensions.on('game_closed',function() while true do end end)"));
    runaway.emit_game("game_closed", snapshot);
    check(runaway.diagnostics().callback_errors == 1, "runaway game event escaped instruction budget");
    Runtime budget(host, package("extensions.on('game_opened',function() for i=1,33 do extensions.setting('tag') end end)"));
    budget.emit_game("game_opened", snapshot);
    check(budget.diagnostics().callback_errors == 1, "game event bypassed API budget");
    Runtime zero_quest(host, package("extensions.on('game_closed',function(info) assert(info.quest_number == 0) end)"));
    snapshot.quest_number = 0;
    zero_quest.emit_game("game_closed", snapshot);
    check(zero_quest.diagnostics().callback_errors == 0, "quest zero was confused with absent quest");
    Runtime allocation_error(host, package(R"(
      extensions.on('game_closed',function()
        local text = 'x'
        while true do text = text .. text end
      end)
      extensions.command('recover',{cooldown_ms=0},function(ctx)
        assert(ctx.client_id == '42' and ctx.can_act and ctx.event == nil)
        extensions.storage_set('recovered','yes')
      end)
    )"));
    allocation_error.emit_game("game_closed", snapshot);
    allocation_error.dispatch("recover", "", {42, 7, "Alice"});
    check(allocation_error.diagnostics().callback_errors == 1 && allocation_error.memory_bytes() <= Runtime::MAX_MEMORY_BYTES &&
            allocation_error.storage().at("recovered") == "yes",
        "game allocation error escaped its budget or retained a stale snapshot frame");

    if (argc > 1) {
      std::ifstream file(argv[1]);
      check(file.good(), "cannot read game-audit example");
      auto example = package(std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>()));
      example.grants = {"events", "storage"};
      GameLifecycle tracking;
      Runtime first(host, example);
      check(tracking.begin(snapshot.lobby_id, valid), "audit game not admitted");
      first.emit_game("game_opened", snapshot);
      check(first.storage().at("opened") == "1", "game-audit did not count opening");
      check(tracking.member_joined(snapshot.lobby_id, 42) && tracking.join_ready(snapshot.lobby_id, 42, valid, true, true),
          "first audit membership could not become ready");
      first.emit("game_join_ready", {42, snapshot.lobby_id, "Alice", "everyone", "GC_EP3", true});
      check(first.storage().at("joins-ready") == "1", "game-audit did not count join acknowledgement");
      check(tracking.member_joined(snapshot.lobby_id, 43), "pending audit membership was not retained");
      example.storage = first.storage();
      Runtime replacement(host, example);
      check(!tracking.begin(snapshot.lobby_id, valid), "package replacement reset game admission");
      check(!tracking.join_ready(snapshot.lobby_id, 42, valid, true, true) &&
              tracking.join_ready(snapshot.lobby_id, 43, valid, true, true),
          "package replacement replayed past readiness or lost a pending member");
      replacement.emit("game_join_ready", {43, snapshot.lobby_id, "Bob", "everyone", "GC_EP3", true});
      check(replacement.storage().at("joins-ready") == "2", "game-audit replacement lost join counts");
      check(tracking.end(snapshot.lobby_id), "replacement observer lost game closure");
      replacement.emit_game("game_closed", snapshot);
      check(replacement.storage().at("opened") == "1" && replacement.storage().at("closed") == "1" && replacement.enabled(), "game-audit replacement lost counts");
      Runtime late_observer(host, package("extensions.on('game_closed',function() extensions.storage_set('closed','1') end)"));
      late_observer.emit_game("game_closed", snapshot);
      check(late_observer.storage().at("closed") == "1", "late observer required historical open replay");
    }
    std::cout << "Primary game eligibility, tracking, snapshots, scopes, faults, budgets and audit example passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
