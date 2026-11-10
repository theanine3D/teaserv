#include "SessionLifecycle.hh"

#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

using namespace teaserv::extensions;

static void check(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

struct TestHost : Host {
  bool live = true;
  size_t actions = 0;
  mutable size_t queries = 0;
  std::vector<std::string> logs;
  bool authorized(const Context&) const override { return live; }
  void reply(const Context&, const std::string&) override { ++actions; }
  void broadcast(const Context&, const std::string&) override { ++actions; }
  bool whisper(const Context&, uint64_t, const std::string&) override {
    ++actions;
    return true;
  }
  void log(const std::string&, const std::string& text) override { logs.emplace_back(text); }
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
  bool command_reserved(const std::string&) const override { return false; }
};

static Package package(const std::string& source) {
  return {"sessions", "1", source, {"commands", "events", "storage", "timers", "chat.reply", "chat.broadcast", "chat.whisper", "scene.media", "scene.event", "lobby.read"}, {}};
}

int main(int argc, char** argv) {
  try {
    TestHost host;
    Context login{42, 0, "Alice", "everyone", "GC_EP3", false, true};
    SessionLifecycle sessions;
    check(!sessions.begin(Context{}), "zero session identity admitted");
    check(sessions.begin(login) && !sessions.begin(login), "duplicate local login was admitted");
    Runtime observer(host, package(R"(
      extensions.on('client_login', function(ctx, event, args)
        assert(event == 'client_login' and args == nil and not ctx.can_act)
        assert(ctx.client_id == '42' and ctx.lobby_id == '0' and ctx.client_version == 'GC_EP3' and not ctx.is_game)
        local caps = extensions.scene_capabilities()
        assert(not extensions.scene_asset_support('asset').supported)
        assert(not caps.native_ep3_media and not caps.experimental_models and not caps.lobby_events)
        assert(caps.max_media_compressed_bytes == 0 and caps.max_media_decompressed_bytes == 0)
        extensions.storage_set('logins', tostring(tonumber(extensions.storage_get('logins') or '0') + 1))
        extensions.log('login-observed')
      end)
      extensions.on('client_disconnect', function(ctx, event)
        assert(event == 'client_disconnect' and not ctx.can_act and ctx.is_game)
        assert(ctx.client_id == '42' and ctx.lobby_id == '99' and ctx.player_name == 'Updated Alice')
        extensions.storage_set('disconnects', '1')
        extensions.log('disconnect-observed')
      end)
      extensions.on('game_join_ready', function(ctx, event, args)
        assert(event == 'game_join_ready' and args == nil and not ctx.can_act and ctx.is_game)
        assert(ctx.client_id == '42' and ctx.lobby_id == '99' and ctx.player_name == 'Updated Alice')
        assert(ctx.client_version == 'GC_EP3' and ctx.account_id == nil and ctx.quest_number == nil)
        assert(extensions.lobby() == nil)
        local caps = extensions.scene_capabilities()
        assert(not caps.native_ep3_media and not caps.experimental_models and not caps.lobby_events)
        extensions.storage_set('join-ready', '1')
        extensions.log('join-ready-observed')
        ctx.client_id, ctx.player_name, ctx.can_act = '1', 'forged', true
      end)
      extensions.command('game', {cooldown_ms=0}, function(ctx)
        assert(ctx.can_act and ctx.is_game and ctx.client_version == 'GC_EP3')
        extensions.reply('normal-player-action')
      end)
      extensions.on('lobby_join', function(ctx)
        assert(ctx.can_act and ctx.is_game and ctx.lobby_id == '99')
        extensions.storage_set('game-joins', '1')
      end)
    )"));
    observer.emit("client_login", login); // Runtime must restrict even a permissive adapter/host.
    check(observer.enabled() && observer.storage().at("logins") == "1" && host.actions == 0, "login observation failed");
    auto game = login;
    game.lobby_id = 99;
    game.player_name = "Updated Alice";
    game.is_game = true;
    for (size_t z = 0; z < 3; ++z) {
      auto invalid = game;
      if (z == 0)
        invalid.client_id = 0;
      if (z == 1)
        invalid.lobby_id = 0;
      if (z == 2)
        invalid.is_game = false;
      observer.emit("game_join_ready", invalid);
    }
    check(!observer.storage().count("join-ready"), "invalid readiness context reached Lua");
    observer.emit("game_join_ready", game);
    check(observer.enabled() && observer.storage().at("join-ready") == "1" && host.queries == 0 && host.actions == 0 &&
            game.player_name == "Updated Alice",
        "readiness observer lost metadata, scope, or copied context");
    sessions.observe(game);
    auto ended = sessions.end(42);
    check(ended && !ended->allow_player_actions && ended->lobby_id == 99 && ended->is_game, "disconnect snapshot lost last membership");
    check(!sessions.end(42), "duplicate disconnect was admitted");
    sessions.observe(game);
    check(!sessions.end(42), "membership observation invented a login");
    host.live = false; // Storage and logging must still work after departure.
    observer.emit("client_disconnect", *ended);
    check(observer.enabled() && observer.storage().at("disconnects") == "1" && host.actions == 0, "disconnected observation failed");
    host.live = true;
    observer.emit("lobby_join", game);
    check(observer.storage().at("game-joins") == "1", "membership callback lost game metadata");
    observer.dispatch("game", "", game);
    check(observer.enabled() && host.actions == 1, "session callback restriction leaked into normal commands");
    auto reconnect = login;
    reconnect.client_id = 43;
    check(sessions.begin(reconnect), "new connection could not log in");
    sessions.forget(43); // Successful proxy handoff suppresses later local cleanup.
    check(!sessions.end(43), "proxy cleanup produced a local disconnect");
    check(sessions.begin(reconnect) && !sessions.begin(reconnect), "return from proxy did not start one local session");
    check(sessions.end(43).has_value() && !sessions.end(43), "returned local session did not close exactly once");

    for (const auto& action : {"extensions.reply('x')", "extensions.broadcast('x')", "extensions.whisper('1','x')",
             "extensions.after(50,function() end)", "extensions.cancel(1)", "extensions.scene_spawn('x','y','z')", "extensions.scene_remove('x')",
             "extensions.every(50,function() end)",
             "extensions.scene_spawn('x','y','z',50)",
             "extensions.scene_event(1)", "extensions.on('game_join_ready',function() end)",
             "extensions.command('late',{},function() end)"}) {
      for (const auto& event : {"client_login", "client_disconnect", "game_join_ready"}) {
        auto source = std::string("extensions.on('") + event + "', function(ctx) ctx.can_act=true; " + action + " end)";
        Runtime bad(host, package(source));
        auto count = host.actions;
        for (size_t z = 0; z < 3; ++z)
          bad.emit(event, game);
        check(!bad.enabled() && bad.timer_count() == 0 && host.actions == count,
            "read-only event allowed an action, forged scope, or escaped fault containment");
        check(observer.enabled(), "another package was damaged by a faulty session handler");
      }
    }
    Runtime timer(host, package(R"(
      extensions.command('timer', {cooldown_ms=0}, function() extensions.after(1000,function() end) end)
      extensions.on('client_disconnect', function() extensions.storage_set('ended','yes') end)
    )"));
    timer.dispatch("timer", "", game);
    check(timer.timer_count() == 1, "timer setup failed");
    timer.cancel_client(game.client_id);
    timer.emit("client_disconnect", game);
    check(timer.timer_count() == 0 && timer.storage().at("ended") == "yes", "disconnect left timers or lost storage observation");
    {
      auto restricted = package("extensions.on('client_login', function() end)");
      restricted.grants.erase("events");
      bool failed = false;
      try {
        Runtime denied(host, restricted);
      } catch (const std::exception&) {
        failed = true;
      }
      check(failed, "session event registration bypassed the events grant");
    }
    {
      auto restricted = package("extensions.on('game_join_ready', function() end)");
      restricted.grants.erase("events");
      bool failed = false;
      try {
        Runtime denied(host, restricted);
      } catch (const std::exception&) {
        failed = true;
      }
      check(failed, "readiness registration bypassed the events grant");
      Runtime runaway(host, package("extensions.on('game_join_ready',function() while true do end end)"));
      for (size_t z = 0; z < 3; ++z)
        runaway.emit("game_join_ready", game);
      check(!runaway.enabled() && runaway.diagnostics().callback_errors == 3, "runaway readiness escaped callback limits");
      GameLifecycle tracking;
      GameAdmission admission{true, false, false, true, true, false, true, true};
      tracking.begin(game.lobby_id, admission);
      tracking.member_joined(game.lobby_id, game.client_id);
      Runtime faulty(host, package("extensions.on('game_join_ready',function() error('observer failed') end)"));
      check(tracking.join_ready(game.lobby_id, game.client_id, admission, true, true), "ready callback not admitted");
      faulty.emit("game_join_ready", game);
      observer.emit("game_join_ready", game);
      check(faulty.diagnostics().callback_errors == 1 && observer.enabled() &&
              !tracking.join_ready(game.lobby_id, game.client_id, admission, true, true),
          "observer failure damaged another observer or rearmed readiness");
      Runtime metadata(host, package(R"(
        local previous
        extensions.on('game_join_ready',function(ctx,event,args)
          assert(ctx.client_id == '18446744073709551615' and ctx.lobby_id == '9223372036854775807')
          assert(event == 'game_join_ready' and args == nil and ctx.is_game and not ctx.can_act)
          assert(previous ~= ctx)
          previous = ctx
          ctx.client_id, ctx.can_act = 'forged', true
          extensions.storage_set('copied','yes')
        end)
      )"));
      Context large_ids{UINT64_MAX, INT64_MAX, "Alice", "everyone", "GC_EP3", true};
      metadata.emit("game_join_ready", large_ids);
      metadata.emit("game_join_ready", large_ids);
      check(metadata.enabled() && metadata.storage().at("copied") == "yes", "readiness lost exact IDs or fresh metadata");
    }
    if (argc > 1) {
      std::ifstream file(argv[1]);
      check(file.good(), "cannot read session-audit example");
      auto definition = package(std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>()));
      definition.grants = {"events", "storage"};
      definition.target_storage_version = 1;
      Runtime audit(host, definition);
      audit.emit("client_login", login);
      host.live = false;
      audit.emit("client_disconnect", *ended);
      check(audit.enabled() && audit.storage().at("logins") == "1" && audit.storage().at("disconnects") == "1",
          "shipped session-audit script failed");
    }
    std::cout << "Session lifecycle tracking, read-only callbacks, metadata, isolation and example tests passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
