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
  check(rejected, "invalid quest observation was accepted");
}
struct TestHost : Host {
  mutable size_t queries = 0;
  size_t replies = 0;
  std::vector<std::string> logs;
  bool authorized(const Context&) const override { return true; }
  void reply(const Context&, const std::string&) override { ++replies; }
  void broadcast(const Context&, const std::string&) override {}
  bool whisper(const Context&, uint64_t, const std::string&) override { return false; }
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
};
static Package package(const std::string& source) {
  Package result{"quests", "1", source, {"events", "commands", "storage", "chat.reply", "lobby.read", "scene.media"}, {}};
  result.settings = resolve_settings({{"tag", {"string", false, std::string("quests")}}}, {});
  return result;
}

int main(int argc, char** argv) {
  try {
    GameLifecycle games;
    GameAdmission admission{true, false, false, true, true, false, true, true};
    check(!games.assign_quest(7, 0) && !games.load_quest_map(7, 0), "unobserved game gained quest state");
    check(!games.begin(7, {}) && !games.assign_quest(7, 0), "ineligible game gained quest state");
    check(games.begin(7, admission) && !games.load_quest_map(7, 0), "map load preceded assignment");
    check(games.assign_quest(7, 0), "quest zero could not be assigned");
    check(!games.assign_quest(7, 0) && !games.assign_quest(7, 1), "duplicate or replacement assignment admitted");
    check(!games.load_quest_map(7, 1) && games.load_quest_map(7, 0) && !games.load_quest_map(7, 0), "mismatched or duplicate map load admitted");
    check(!games.begin(7, admission) && !games.assign_quest(7, 0), "additional join reset quest state");
    check(games.begin(8, admission) && games.assign_quest(8, UINT32_MAX) && games.load_quest_map(8, UINT32_MAX), "independent max-number quest was lost");
    check(games.end(7) && !games.assign_quest(7, 0) && !games.load_quest_map(7, 0), "removed game retained quest state");
    check(games.begin(7, admission) && games.assign_quest(7, 0) && games.load_quest_map(7, 0), "new game lifetime retained old deduplication");

    {
      GameLifecycle acknowledgements;
      check(!acknowledgements.acknowledge_quest(7, 42, 0, admission), "unknown game acquired an acknowledgement");
      acknowledgements.begin(7, admission);
      acknowledgements.member_joined(7, 42);
      check(!acknowledgements.acknowledge_quest(7, 42, 0, admission), "acknowledgement preceded assignment");
      acknowledgements.assign_quest(7, 0);
      check(!acknowledgements.acknowledge_quest(7, 43, 0, admission) && !acknowledgements.acknowledge_quest(7, 42, 1, admission),
          "unknown member or mismatched quest acquired acknowledgement");
      for (auto field : {&GameAdmission::is_game, &GameAdmission::spectator, &GameAdmission::playback,
               &GameAdmission::authenticated, &GameAdmission::connected, &GameAdmission::proxy,
               &GameAdmission::local_session, &GameAdmission::current_member}) {
        auto bad = admission;
        bad.*field = !(bad.*field);
        check(!acknowledgements.acknowledge_quest(7, 42, 0, bad), "ineligible quest acknowledgement was admitted");
      }
      check(acknowledgements.acknowledge_quest(7, 42, 0, admission) &&
              !acknowledgements.acknowledge_quest(7, 42, 0, admission),
          "quest zero was missing or acknowledged twice");
      check(acknowledgements.join_ready(7, 42, admission, true, true), "quest acknowledgement consumed initial join readiness");
      check(!acknowledgements.member_joined(7, 42) && !acknowledgements.acknowledge_quest(7, 42, 0, admission),
          "duplicate membership rearmed acknowledgement");
      check(acknowledgements.load_quest_map(7, 0), "client acknowledgement consumed map preparation");
      acknowledgements.member_joined(7, 43);
      check(acknowledgements.acknowledge_quest(7, 43, 0, admission), "map preparation prevented a later acknowledgement");
      acknowledgements.member_left(7, 42);
      check(!acknowledgements.acknowledge_quest(7, 42, 0, admission), "departed member acknowledged a quest");
      acknowledgements.member_joined(7, 42);
      check(acknowledgements.acknowledge_quest(7, 42, 0, admission), "new membership inherited old acknowledgement");
      acknowledgements.forget_client(42);
      check(!acknowledgements.acknowledge_quest(7, 42, 0, admission), "disconnected member retained acknowledgement state");
      acknowledgements.end(7);
      check(!acknowledgements.acknowledge_quest(7, 43, 0, admission), "removed game acknowledged a quest");
      acknowledgements.begin(7, admission);
      acknowledgements.member_joined(7, 43);
      acknowledgements.assign_quest(7, UINT32_MAX);
      check(acknowledgements.acknowledge_quest(7, 43, UINT32_MAX, admission), "new lifetime or max quest number lost acknowledgement");
    }

    {
      GameLifecycle joins;
      joins.begin(7, admission);
      joins.member_joined(7, 42);
      joins.assign_quest(7, 0); // Non-joinable metadata must never qualify.
      check(!joins.acknowledge_joinable_quest(7, 42, 0, admission), "non-joinable quest acquired a late-join acknowledgement");
      joins.begin(8, admission);
      joins.member_joined(8, 42);
      check(!joins.acknowledge_joinable_quest(8, 42, 0, admission), "late acknowledgement preceded assignment");
      joins.assign_quest(8, 0, true);
      check(!joins.acknowledge_joinable_quest(8, 0, 0, admission) && !joins.acknowledge_joinable_quest(8, 43, 0, admission) &&
              !joins.acknowledge_joinable_quest(8, 42, 1, admission),
          "unknown member or mismatched quest acquired late acknowledgement");
      for (auto field : {&GameAdmission::is_game, &GameAdmission::spectator, &GameAdmission::playback,
               &GameAdmission::authenticated, &GameAdmission::connected, &GameAdmission::proxy,
               &GameAdmission::local_session, &GameAdmission::current_member}) {
        auto bad = admission;
        bad.*field = !(bad.*field);
        check(!joins.acknowledge_joinable_quest(8, 42, 0, bad), "ineligible late acknowledgement was admitted");
      }
      check(joins.acknowledge_joinable_quest(8, 42, 0, admission) && !joins.acknowledge_joinable_quest(8, 42, 0, admission),
          "late acknowledgement was missing or duplicated");
      check(joins.join_ready(8, 42, admission, true, true) && joins.acknowledge_quest(8, 42, 0, admission),
          "late acknowledgement consumed initial join/load tracking");
      check(!joins.member_joined(8, 42) && !joins.acknowledge_joinable_quest(8, 42, 0, admission), "duplicate join rearmed late acknowledgement");
      joins.member_joined(8, 43);
      joins.load_quest_map(8, 0);
      check(joins.acknowledge_joinable_quest(8, 43, 0, admission), "map preparation lost an independent late join");
      joins.member_left(8, 42);
      check(!joins.acknowledge_joinable_quest(8, 42, 0, admission), "departed member retained late acknowledgement");
      joins.member_joined(8, 42);
      check(joins.acknowledge_joinable_quest(8, 42, 0, admission), "genuine rejoin inherited completed late acknowledgement");
      joins.forget_client(42);
      check(!joins.acknowledge_joinable_quest(8, 42, 0, admission), "disconnect/proxy handoff retained late acknowledgement");
      joins.end(8);
      check(!joins.acknowledge_joinable_quest(8, 43, 0, admission), "closed game retained late acknowledgement");
      joins.begin(8, admission);
      joins.assign_quest(8, UINT32_MAX, true);
      check(!joins.acknowledge_joinable_quest(8, 43, UINT32_MAX, admission), "new lifetime inherited member state");
      joins.member_joined(8, 43);
      check(joins.acknowledge_joinable_quest(8, 43, UINT32_MAX, admission), "fresh lifetime/max quest did not acknowledge a late join");
    }
    {
      TestHost host;
      GameSnapshot game{INT64_MAX, "Joinable game", 1, 0, 2, 0, true};
      QuestLoadAcknowledgement visible{game, UINT64_MAX, "GC_V3", QuestAcknowledgementKind::JOINABLE_PLAYER_VISIBLE};
      auto bb = visible;
      bb.client_version = "BB_V4";
      bb.kind = QuestAcknowledgementKind::JOINABLE_BB_READY;
      auto definition = package(R"(
        local previous
        extensions.on('quest_join_acknowledged',function(info, extra)
          assert(extra == nil and info.event == 'quest_join_acknowledged')
          assert(info.lobby_id == '9223372036854775807' and info.client_id == '18446744073709551615')
          assert(info.name == 'Joinable game' and info.quest_number == 0 and info.quest_joinable)
          assert(info.account_id == nil and info.player_name == nil and info.permission == nil and info.password == nil)
          if info.client_version == 'BB_V4' then assert(info.reason == 'bb_ready')
          else assert(info.client_version == 'GC_V3' and info.reason == 'player_visible') end
          assert(previous ~= info and extensions.setting('tag') == 'quests' and extensions.lobby() == nil)
          local caps = extensions.scene_capabilities()
          assert(not extensions.scene_asset_support('asset').supported)
          assert(not caps.native_ep3_media and not caps.lobby_events and caps.max_media_compressed_bytes == 0)
          previous = info
          info.can_act, info.client_id = true, 'forged'
          extensions.storage_set('joins', tostring(tonumber(extensions.storage_get('joins') or '0') + 1))
          extensions.log('join-observed')
        end)
        extensions.on('quest_load_acknowledged',function() extensions.storage_set('initial','yes') end)
        extensions.on('game_closed',function(info) assert(info.client_id == nil and info.client_version == nil) end)
        extensions.command('recover',{cooldown_ms=0},function(ctx) assert(ctx.can_act); extensions.reply('normal') end)
      )");
      Runtime observer(host, definition);
      observer.emit("quest_join_acknowledged", {42, 7, "forged"});
      rejects([&] { observer.emit_game("quest_join_acknowledged", game); });
      check(observer.diagnostics().callbacks == 0, "generic dispatch forged late acknowledgement");
      for (size_t z = 0; z < 4; ++z) {
        auto bad = visible;
        if (z == 0)
          bad.kind = static_cast<QuestAcknowledgementKind>(99);
        if (z == 1)
          bad.game.quest_joinable = false;
        if (z == 2)
          bad.kind = QuestAcknowledgementKind::JOINABLE_BB_READY;
        if (z == 3)
          bad.client_version = "BB_V4";
        rejects([&] { observer.emit_quest_acknowledgement(bad); });
      }
      observer.emit_quest_acknowledgement(visible);
      observer.emit_quest_acknowledgement(bb);
      check(observer.enabled() && !observer.diagnostics().callback_errors && observer.storage().at("joins") == "2" &&
              !observer.storage().count("initial") && visible.client_id == UINT64_MAX && host.queries == 0,
          "late acknowledgement reasons, metadata, or restricted scope failed");
      observer.emit_game("game_closed", game);
      observer.dispatch("recover", "", {42, 7, "Alice"});
      check(observer.diagnostics().callback_errors == 0 && host.replies == 1, "late acknowledgement scope leaked into later callbacks");
      definition.grants.erase("events");
      rejects([&] { Runtime denied(host, definition); });
      definition = package("extensions.on('quest_join_acknowledged',function() extensions.storage_set('key','x') end)");
      definition.grants.erase("storage");
      Runtime denied(host, definition);
      denied.emit_quest_acknowledgement(bb);
      check(denied.storage().empty() && denied.diagnostics().callback_errors == 1, "late acknowledgement bypassed storage grant");
      Runtime runaway(host, package("extensions.on('quest_join_acknowledged',function() while true do end end)"));
      runaway.emit_quest_acknowledgement(bb);
      check(runaway.diagnostics().callback_errors == 1, "late acknowledgement bypassed instruction budget");
      Runtime api_budget(host, package("extensions.on('quest_join_acknowledged',function() for i=1,33 do extensions.setting('tag') end end)"));
      api_budget.emit_quest_acknowledgement(visible);
      check(api_budget.diagnostics().callback_errors == 1, "late acknowledgement bypassed API budget");
      Runtime allocation(host, package(R"(
        extensions.on('quest_join_acknowledged',function() local s='x'; while true do s=s..s end end)
        extensions.on('game_closed',function(info) assert(info.client_id == nil and info.client_version == nil); extensions.storage_set('recovered','yes') end)
      )"));
      allocation.emit_quest_acknowledgement(bb);
      allocation.emit_game("game_closed", game);
      check(allocation.diagnostics().callback_errors == 1 && allocation.storage().at("recovered") == "yes" &&
              allocation.memory_bytes() <= Runtime::MAX_MEMORY_BYTES,
          "late acknowledgement allocation failure retained metadata");
      GameLifecycle tracking;
      tracking.begin(game.lobby_id, admission);
      tracking.member_joined(game.lobby_id, visible.client_id);
      tracking.assign_quest(game.lobby_id, 0, true);
      check(tracking.acknowledge_joinable_quest(game.lobby_id, visible.client_id, 0, admission), "late observation not admitted");
      Runtime faulty(host, package("extensions.on('quest_join_acknowledged',function() error('observer failed') end)"));
      faulty.emit_quest_acknowledgement(visible);
      observer.emit_quest_acknowledgement(visible);
      check(faulty.diagnostics().callback_errors == 1 && observer.enabled() &&
              !tracking.acknowledge_joinable_quest(game.lobby_id, visible.client_id, 0, admission),
          "observer failure rearmed late acknowledgement or damaged another observer");
    }
    {
      TestHost host;
      GameSnapshot game{INT64_MAX, "Acknowledged game", 3, 1, 2, 0, true};
      QuestLoadAcknowledgement acknowledgement{game, UINT64_MAX, "GC_EP3"};
      validate_quest_acknowledgement(acknowledgement);
      for (size_t z = 0; z < 7; ++z) {
        auto bad = acknowledgement;
        if (z == 0)
          bad.client_id = 0;
        if (z == 1)
          bad.game.quest_number.reset();
        if (z == 2)
          bad.client_version.clear();
        if (z == 3)
          bad.client_version.assign(33, 'A');
        if (z == 4)
          bad.client_version = "GC EP3";
        if (z == 5)
          bad.client_version = std::string("GC\0EP3", 6);
        if (z == 6)
          bad.game.lobby_id = 0;
        rejects([&] { validate_quest_acknowledgement(bad); });
      }
      auto source = R"(
        local previous
        extensions.on('quest_load_acknowledged',function(info, extra)
          assert(extra == nil and info.event == 'quest_load_acknowledged' and info.reason == 'load_acknowledged')
          assert(info.client_id == '18446744073709551615' and info.client_version == 'GC_EP3')
          assert(info.lobby_id == '9223372036854775807' and info.name == 'Acknowledged game')
          assert(info.quest_number == 0 and info.quest_joinable and info.episode == 3 and info.mode == 1 and info.difficulty == 2)
          assert(info.account_id == nil and info.password == nil and info.player_name == nil and info.permission == nil)
          assert(extensions.setting('tag') == 'quests' and extensions.lobby() == nil)
          local caps = extensions.scene_capabilities()
          assert(not caps.native_ep3_media and not caps.lobby_events and caps.max_media_compressed_bytes == 0)
          assert(previous ~= info)
          previous = info
          info.client_id, info.can_act, info.quest_number = 'forged', true, 99
          extensions.storage_set('acknowledged', 'yes')
          extensions.log('acknowledged')
        end)
        extensions.on('game_closed',function(info) assert(info.client_id == nil and info.client_version == nil) end)
        extensions.command('recover',{cooldown_ms=0},function(ctx) assert(ctx.can_act); extensions.reply('normal') end)
      )";
      auto definition = package(source);
      Runtime observed(host, definition);
      observed.emit("quest_load_acknowledged", {42, 7, "forged"});
      rejects([&] { observed.emit_game("quest_load_acknowledged", game); });
      check(observed.diagnostics().callbacks == 0, "generic dispatch forged a quest acknowledgement");
      observed.emit_quest_acknowledgement(acknowledgement);
      observed.emit_quest_acknowledgement(acknowledgement);
      check(observed.enabled() && observed.storage().at("acknowledged") == "yes" && !observed.diagnostics().callback_errors &&
              acknowledgement.client_id == UINT64_MAX && host.queries == 0,
          "acknowledgement metadata or restricted scope failed");
      observed.emit_game("game_closed", game);
      GameLifecycle tracking;
      tracking.begin(game.lobby_id, admission);
      tracking.member_joined(game.lobby_id, acknowledgement.client_id);
      tracking.assign_quest(game.lobby_id, 0);
      Runtime failed(host, package("extensions.on('quest_load_acknowledged',function() error('failed observer') end)"));
      check(tracking.acknowledge_quest(game.lobby_id, acknowledgement.client_id, 0, admission), "valid acknowledgement not admitted");
      failed.emit_quest_acknowledgement(acknowledgement);
      observed.emit_quest_acknowledgement(acknowledgement);
      check(failed.diagnostics().callback_errors == 1 && observed.enabled() &&
              !tracking.acknowledge_quest(game.lobby_id, acknowledgement.client_id, 0, admission),
          "observer failure rearmed acknowledgement or damaged another observer");
      auto replies = host.replies;
      observed.dispatch("recover", "", {42, 7, "Alice"});
      check(observed.diagnostics().callback_errors == 0 && host.replies == replies + 1, "acknowledgement scope leaked into later callbacks");
      definition.grants.erase("events");
      rejects([&] { Runtime denied(host, definition); });
      definition = package("extensions.on('quest_load_acknowledged',function() extensions.storage_set('ack','yes') end)");
      definition.grants.erase("storage");
      Runtime no_storage(host, definition);
      no_storage.emit_quest_acknowledgement(acknowledgement);
      check(no_storage.storage().empty() && no_storage.diagnostics().callback_errors == 1, "acknowledgement bypassed storage grant");
      Runtime runaway(host, package("extensions.on('quest_load_acknowledged',function() while true do end end)"));
      runaway.emit_quest_acknowledgement(acknowledgement);
      check(runaway.diagnostics().callback_errors == 1, "acknowledgement bypassed instruction budget");
      Runtime api_budget(host, package("extensions.on('quest_load_acknowledged',function() for i=1,33 do extensions.setting('tag') end end)"));
      api_budget.emit_quest_acknowledgement(acknowledgement);
      check(api_budget.diagnostics().callback_errors == 1, "acknowledgement bypassed API budget");
      Runtime allocation(host, package(R"(
        extensions.on('quest_load_acknowledged',function() local s='x'; while true do s=s..s end end)
        extensions.on('game_closed',function(info) assert(info.client_id == nil and info.client_version == nil); extensions.storage_set('recovered','yes') end)
      )"));
      allocation.emit_quest_acknowledgement(acknowledgement);
      allocation.emit_game("game_closed", game);
      check(allocation.diagnostics().callback_errors == 1 && allocation.storage().at("recovered") == "yes" &&
              allocation.memory_bytes() <= Runtime::MAX_MEMORY_BYTES,
          "allocation failure leaked acknowledgement metadata/scope");
    }
    TestHost host;
    auto definition = package(R"(
      local retained
      extensions.on('quest_assigned',function(info, extra)
        assert(extra == nil and info.event == 'quest_assigned' and info.reason == 'assigned')
        assert(info.lobby_id == '9223372036854775807' and info.name == 'Quest game')
        assert(info.episode == 3 and info.mode == 1 and info.difficulty == 2)
        assert(info.quest_number == 0 and info.quest_joinable == true)
        assert(info.client_id == nil and info.account_id == nil and info.password == nil and info.permission == nil)
        assert(extensions.setting('tag') == 'quests' and extensions.lobby() == nil)
        assert(not extensions.scene_capabilities().native_ep3_media)
        assert(not extensions.scene_asset_support('asset').supported)
        extensions.storage_set('assigned','1')
        extensions.log('assigned')
        info.quest_number, info.quest_joinable, info.lobby_id, info.can_act = 99, false, '1', true
        retained = info
      end)
      extensions.on('quest_map_loaded',function(info)
        assert(info.event == 'quest_map_loaded' and info.reason == 'map_loaded')
        assert(info.quest_number == 0 and info.quest_joinable == true and info.name == 'Renamed quest game')
        assert(info.lobby_id == '9223372036854775807' and retained.quest_number == 99)
        assert(extensions.storage_get('assigned') == '1' and extensions.lobby() == nil)
        extensions.storage_set('loaded','1')
      end)
      extensions.command('normal',{cooldown_ms=0},function(ctx)
        assert(ctx.client_id == '42' and ctx.can_act and ctx.event == nil)
        extensions.reply('normal')
      end)
    )");
    Runtime runtime(host, definition);
    GameSnapshot snapshot{INT64_MAX, "Quest game", 3, 1, 2};
    for (const auto& event : {"quest_assigned", "quest_map_loaded"}) {
      rejects([&] { runtime.emit_game(event, snapshot); });
      runtime.emit(event, {42, 7, "Alice"});
    }
    check(runtime.diagnostics().callbacks == 0, "missing quest data or generic dispatch entered Lua");
    snapshot.quest_number = 0;
    snapshot.quest_joinable = true;
    runtime.emit_game("quest_assigned", snapshot);
    check(runtime.diagnostics().callback_errors == 0 && runtime.storage().at("assigned") == "1" &&
            *snapshot.quest_number == 0 && snapshot.quest_joinable,
        "assignment snapshot/scopes failed");
    snapshot.name = "Renamed quest game";
    runtime.emit_game("quest_map_loaded", snapshot);
    check(runtime.storage().at("loaded") == "1" && runtime.diagnostics().callback_errors == 0 && host.queries == 0, "map observation did not use a fresh restricted snapshot");
    runtime.dispatch("normal", "", {42, 7, "Alice"});
    check(host.replies == 1 && runtime.diagnostics().callback_errors == 0, "quest scope leaked into normal callback");
    auto no_events = definition;
    no_events.grants.erase("events");
    rejects([&] { Runtime denied(host, no_events); });
    auto no_storage = package("extensions.on('quest_assigned',function() extensions.storage_set('key','x') end)");
    no_storage.grants.erase("storage");
    Runtime denied_storage(host, no_storage);
    denied_storage.emit_game("quest_assigned", snapshot);
    check(denied_storage.storage().empty() && denied_storage.diagnostics().callback_errors == 1, "quest handler bypassed storage grant");
    snapshot.quest_number = UINT32_MAX;
    snapshot.quest_joinable = false;
    Runtime max_number(host, package("extensions.on('quest_assigned',function(info) assert(info.quest_number == 4294967295 and info.quest_joinable == false) end)"));
    max_number.emit_game("quest_assigned", snapshot);
    check(max_number.diagnostics().callback_errors == 0, "max quest number or false joinability lost precision/type");
    Runtime failed_observer(host, package(R"(
      extensions.on('quest_assigned',function() error('injected observer failure') end)
      extensions.on('quest_map_loaded',function() extensions.storage_set('map','observed') end)
    )"));
    failed_observer.emit_game("quest_assigned", snapshot);
    failed_observer.emit_game("quest_map_loaded", snapshot);
    check(failed_observer.diagnostics().callback_errors == 1 && failed_observer.storage().at("map") == "observed", "assignment failure damaged later map callback");

    if (argc > 1) {
      std::ifstream file(argv[1]);
      check(file.good(), "cannot read quest-audit example");
      auto example = package(std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>()));
      example.grants = {"events", "storage"};
      GameLifecycle tracking;
      Runtime first(host, example);
      snapshot.quest_number = 0;
      check(tracking.begin(snapshot.lobby_id, admission) && tracking.assign_quest(snapshot.lobby_id, 0), "audit assignment not admitted");
      first.emit_game("quest_assigned", snapshot);
      check(first.storage().at("assigned") == "1" && first.storage().at("last-assigned-quest") == "0", "quest-audit lost assignment zero");
      tracking.member_joined(snapshot.lobby_id, 42);
      check(tracking.acknowledge_quest(snapshot.lobby_id, 42, 0, admission), "audit acknowledgement not admitted");
      first.emit_quest_acknowledgement({snapshot, 42, "GC_EP3"});
      check(first.storage().at("loads-acknowledged") == "1" && first.storage().at("last-acknowledged-quest") == "0",
          "quest-audit lost initial acknowledgement");
      tracking.member_joined(snapshot.lobby_id, 43);
      example.storage = first.storage();
      Runtime replacement(host, example);
      check(!tracking.begin(snapshot.lobby_id, admission) && !tracking.assign_quest(snapshot.lobby_id, 0), "package reload reset assignment tracking");
      check(!tracking.acknowledge_quest(snapshot.lobby_id, 42, 0, admission) && tracking.acknowledge_quest(snapshot.lobby_id, 43, 0, admission),
          "package replacement replayed or lost client acknowledgements");
      replacement.emit_quest_acknowledgement({snapshot, 43, "BB_V4"});
      check(replacement.storage().at("loads-acknowledged") == "2", "quest-audit replacement lost acknowledgement counts");
      check(tracking.load_quest_map(snapshot.lobby_id, 0), "reload lost pending map observation");
      replacement.emit_game("quest_map_loaded", snapshot);
      check(!tracking.load_quest_map(snapshot.lobby_id, 0) && replacement.storage().at("maps-loaded") == "1" && replacement.storage().at("last-map-quest") == "0", "quest-audit lost or duplicated map observation");
      GameLifecycle late_tracking;
      check(late_tracking.begin(snapshot.lobby_id, admission) && late_tracking.assign_quest(snapshot.lobby_id, 0),
          "assignment without an enabled observer was not tracked");
      example.storage.clear();
      Runtime late(host, example);
      late_tracking.member_joined(snapshot.lobby_id, 44);
      check(late_tracking.acknowledge_quest(snapshot.lobby_id, 44, 0, admission), "late observer lost pending acknowledgement");
      late.emit_quest_acknowledgement({snapshot, 44, "GC_EP3"});
      check(late_tracking.load_quest_map(snapshot.lobby_id, 0), "late observer lost pending map notification");
      late.emit_game("quest_map_loaded", snapshot);
      check(late.storage().at("maps-loaded") == "1" && !late.storage().count("assigned"),
          "late quest observer required or replayed an assignment callback");
      auto joinable_snapshot = snapshot;
      joinable_snapshot.quest_joinable = true;
      GameLifecycle joining;
      joining.begin(snapshot.lobby_id, admission);
      joining.assign_quest(snapshot.lobby_id, 0, true);
      joining.member_joined(snapshot.lobby_id, 42);
      joining.member_joined(snapshot.lobby_id, 43);
      example.storage.clear();
      Runtime joined(host, example);
      check(joining.acknowledge_joinable_quest(snapshot.lobby_id, 42, 0, admission), "audit late join not admitted");
      joined.emit_quest_acknowledgement({joinable_snapshot, 42, "GC_V3", QuestAcknowledgementKind::JOINABLE_PLAYER_VISIBLE});
      check(joined.storage().at("joins-acknowledged") == "1" && joined.storage().at("last-join-quest") == "0" &&
              !joined.storage().count("loads-acknowledged"),
          "audit late join mixed initial and joinable counters");
      example.storage = joined.storage();
      Runtime join_replacement(host, example);
      check(!joining.acknowledge_joinable_quest(snapshot.lobby_id, 42, 0, admission) &&
              joining.acknowledge_joinable_quest(snapshot.lobby_id, 43, 0, admission),
          "package reload lost or repeated late acknowledgement");
      join_replacement.emit_quest_acknowledgement({joinable_snapshot, 43, "BB_V4", QuestAcknowledgementKind::JOINABLE_BB_READY});
      check(join_replacement.enabled() && join_replacement.storage().at("joins-acknowledged") == "2",
          "audit reload lost late acknowledgement counts");
    }
    std::cout << "Quest ordering, deduplication, snapshots, grants, observer faults and audit example passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
