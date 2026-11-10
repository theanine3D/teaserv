#include "Runtime.hh"

#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <thread>

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
  check(rejected, "invalid lobby view was accepted");
}
struct TestHost : Host {
  bool live = true;
  bool admin = true;
  mutable size_t reads = 0;
  std::optional<LobbyView> view;
  std::vector<std::string> messages;
  bool authorized(const Context& c) const override { return live && (c.permission == "everyone" || admin); }
  void reply(const Context&, const std::string& text) override { messages.emplace_back(text); }
  void broadcast(const Context&, const std::string&) override {}
  bool whisper(const Context&, uint64_t, const std::string&) override { return false; }
  void log(const std::string&, const std::string&) override {}
  bool command_reserved(const std::string&) const override { return false; }
  std::optional<LobbyView> lobby_view(const Context&) const override {
    ++reads;
    return view;
  }
};
static Package package(const std::string& source) {
  return {"lobby-test", "1", source, {"commands", "events", "timers", "chat.reply", "lobby.read"}, {}};
}

int main(int argc, char** argv) {
  try {
    Context context{UINT64_MAX, 7, "Alice", "everyone", "GC_EP3"};
    LobbyView valid{7, "CARD lobby", false, 3, 12,
        {{UINT64_MAX, 2, "Alice", "GC_EP3", true}, {9007199254740993ULL, 5, "Bob", "BB_V4", false}}};
    validate_lobby_view(valid, context.client_id, 7);
    rejects([&] { validate_lobby_view(valid, context.client_id, 8); });
    rejects([&] { validate_lobby_view(valid, 1, 7); });
    rejects([&] { validate_lobby_view(valid, 0, 7); });
    for (int z = 0; z < 10; z++) {
      auto bad = valid;
      switch (z) {
        case 0:
          bad.capacity = 13;
          break;
        case 1:
          bad.capacity = 0;
          break;
        case 2:
          bad.name.assign(257, 'x');
          break;
        case 3:
          bad.players[1].slot = 12;
          break;
        case 4:
          bad.players[1].slot = 2;
          break;
        case 5:
          bad.players[1].client_id = UINT64_MAX;
          break;
        case 6:
          bad.players[1].client_id = 0;
          break;
        case 7:
          bad.players[1].player_name.assign(257, 'x');
          break;
        case 8:
          bad.players[1].client_version.assign(33, 'x');
          break;
        case 9:
          bad.players[1].player_name = std::string("a\0b", 3);
          break;
      }
      rejects([&] { validate_lobby_view(bad, context.client_id, 7); });
    }
    auto full = valid;
    full.players.clear();
    for (size_t slot = 0; slot < 12; slot++) {
      full.players.emplace_back(LobbyPlayerView{slot + 1, slot, std::string(256, 'x'), "GC_EP3", slot == 0});
    }
    full.name.assign(256, 'x');
    validate_lobby_view(full, 1, 7);
    full.players.emplace_back(LobbyPlayerView{13, 0, "extra", "GC_EP3", false});
    rejects([&] { validate_lobby_view(full, 1, 7); });

    TestHost host;
    host.view = valid;
    Runtime runtime(host, package(R"(
      local retained
      extensions.command('read', {cooldown_ms=0}, function(ctx)
        local lobby = extensions.lobby()
        assert(lobby.lobby_id == '7' and lobby.name == 'CARD lobby' and not lobby.is_game)
        assert(lobby.event == 3 and lobby.capacity == 12 and #lobby.players == 2)
        assert(lobby.password == nil and lobby.account_id == nil)
        local me, other = lobby.players[1], lobby.players[2]
        assert(me.client_id == '18446744073709551615' and me.slot == 2 and me.player_name == 'Alice')
        assert(me.client_version == 'GC_EP3' and me.is_self and me.is_leader)
        assert(other.client_id == '9007199254740993' and other.slot == 5 and not other.is_self and not other.is_leader)
        assert(other.account_id == nil and other.ip_address == nil and other.permission == nil)
        lobby.name, me.client_id = 'forged', '1'
        lobby.players[2] = nil
        retained = lobby
        ctx.lobby_id, ctx.client_id = '999', '1'
        local fresh = extensions.lobby()
        assert(fresh.name == 'CARD lobby' and fresh.lobby_id == '7' and fresh.players[1].client_id == '18446744073709551615' and #fresh.players == 2)
      end)
      extensions.command('changed', {cooldown_ms=0}, function()
        local fresh = extensions.lobby()
        assert(fresh.name == 'Game' and fresh.is_game and #fresh.players == 1 and fresh.capacity == 4)
        assert(retained.name == 'forged' and #retained.players == 1)
      end)
      extensions.command('absent', {cooldown_ms=0}, function() assert(extensions.lobby() == nil) end)
      extensions.on('lobby_leave', function() assert(extensions.lobby() == nil) end)
      extensions.on('client_login', function(ctx) ctx.can_act=true; assert(extensions.lobby() == nil) end)
      extensions.on('client_disconnect', function(ctx) ctx.can_act=true; assert(extensions.lobby() == nil) end)
      extensions.command('budget', {cooldown_ms=0}, function() for i=1,33 do extensions.lobby() end end)
      extensions.command('oom', {cooldown_ms=0}, function()
        local copies = {}
        for i=1,32 do copies[i] = extensions.lobby() end
        local large = 'x'
        while true do large = large .. large end
      end)
      extensions.command('delayed', {permission='announce', cooldown_ms=0}, function()
        extensions.after(50, function() extensions.lobby() end)
      end)
    )"));
    runtime.dispatch("read", "", context);
    check(runtime.diagnostics().callback_errors == 0 && host.reads == 2, "snapshot fields or scalar copies failed");
    check(host.view->name == valid.name && host.view->players.size() == 2, "script changed host data");
    runtime.emit("client_login", context);
    runtime.emit("client_disconnect", context);
    check(runtime.diagnostics().callback_errors == 0 && host.reads == 2, "read-only session event queried live lobby");
    host.live = false;
    runtime.emit("lobby_leave", context);
    check(runtime.diagnostics().callback_errors == 0 && host.reads == 2, "stale lobby event queried host or faulted");
    host.live = true;
    host.view->name = "Game";
    host.view->is_game = true;
    host.view->capacity = 4;
    host.view->players.pop_back();
    runtime.dispatch("changed", "", context);
    check(runtime.diagnostics().callback_errors == 0, "snapshot did not refresh after host change");
    host.view.reset();
    runtime.dispatch("absent", "", context);
    check(runtime.diagnostics().callback_errors == 0, "unavailable query was a script failure");
    host.view = valid;
    runtime.dispatch("delayed", "", context);
    auto reads = host.reads;
    host.admin = false;
    std::this_thread::sleep_for(std::chrono::milliseconds(70));
    runtime.tick();
    check(host.reads == reads, "delayed query bypassed revoked permission");
    runtime.dispatch("oom", "", context);
    check(runtime.memory_bytes() <= Runtime::MAX_MEMORY_BYTES && runtime.diagnostics().callback_errors == 1, "OOM escaped containment");
    runtime.dispatch("read", "", context);
    check(runtime.diagnostics().callback_errors == 1, "OOM damaged subsequent lobby queries");
    runtime.dispatch("budget", "", context);
    check(runtime.diagnostics().callback_errors == 2, "lobby reads bypassed API budget");

    auto no_grant = package("extensions.command('read', {cooldown_ms=0}, function() extensions.lobby() end)");
    no_grant.grants.erase("lobby.read");
    Runtime denied(host, no_grant);
    reads = host.reads;
    denied.dispatch("read", "", context);
    check(denied.diagnostics().callback_errors == 1 && host.reads == reads, "query bypassed lobby.read grant");
    rejects([&] { Runtime bad(host, package("extensions.lobby()")); });
    auto migration = package("extensions.migrate(function() extensions.lobby() end)");
    migration.grants.emplace("storage");
    migration.target_storage_version = 1;
    reads = host.reads;
    rejects([&] { Runtime bad(host, migration); });
    check(host.reads == reads, "migration reached live lobby adapter");
    host.view->lobby_id = 8;
    Runtime wrong_lobby(host, package("extensions.command('read', {cooldown_ms=0}, function() extensions.lobby() end)"));
    wrong_lobby.dispatch("read", "", context);
    check(wrong_lobby.diagnostics().callback_errors == 1, "out-of-scope adapter result was returned to Lua");
    host.view = valid;

    if (argc > 1) {
      host.messages.clear();
      std::ifstream file(argv[1]);
      check(file.good(), "cannot read lobby-tools example");
      auto definition = package(std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>()));
      definition.grants = {"commands", "chat.reply", "lobby.read"};
      Runtime example(host, definition);
      example.dispatch("who", "", context);
      check(host.messages.size() == 3 && host.messages[0] == "CARD lobby (2/12)" &&
              host.messages[1] == "18446744073709551615: Alice [GC_EP3] (you)" &&
              host.messages[2] == "9007199254740993: Bob [BB_V4]",
          "who example omitted IDs or players");
      auto before = host.messages.size();
      example.dispatch("who", "", context);
      check(host.messages.size() == before + 1 && host.messages.back().find("wait") != std::string::npos, "who cooldown failed");
      host.view->name.assign(256, 'x');
      host.view->players.clear();
      for (size_t slot = 0; slot < 12; slot++) {
        host.view->players.emplace_back(LobbyPlayerView{UINT64_MAX - slot, slot, std::string(256, 'x'), std::string(32, 'v'), slot == 0});
      }
      Runtime full_example(host, definition);
      before = host.messages.size();
      full_example.dispatch("who", "", context);
      check(full_example.diagnostics().callback_errors == 0 && host.messages.size() == before + 13,
          "full lobby who exceeded API/output budgets");
      host.view.reset();
      Runtime unavailable(host, definition);
      unavailable.dispatch("who", "", context);
      check(host.messages.back() == "Lobby information is unavailable." && unavailable.enabled(), "who unavailable handling failed");
    }
    std::cout << "Lobby view bounds, permissions, copies, scopes, budgets, OOM and who example passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
