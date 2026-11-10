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
  check(rejected, "invalid lifecycle action was accepted");
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
  Package result{"lifecycle", "1.2.3", source,
      {"events", "commands", "storage", "timers", "chat.reply", "chat.broadcast", "chat.whisper", "lobby.read", "scene.media", "scene.event"}, {}};
  result.settings = resolve_settings({{"tag", {"string", false, std::string("audit")}}}, {});
  return result;
}

int main(int argc, char** argv) {
  try {
    TestHost host;
    auto definition = package(R"(
      extensions.on('package_load', function(info, second)
        assert(info.id == 'lifecycle' and info.version == '1.2.3' and info.event == 'package_load' and second == nil)
        assert(info.reason == 'startup' or info.reason == 'reload')
        assert(info.client_id == nil and info.lobby_id == nil and info.permission == nil)
        assert(extensions.setting('tag') == 'audit' and extensions.lobby() == nil)
        assert(not extensions.scene_capabilities().native_ep3_media)
        assert(not extensions.scene_asset_support('asset').supported)
        extensions.storage_set('loads', tostring(tonumber(extensions.storage_get('loads') or '0') + 1))
        extensions.log('load:' .. info.reason)
        info.id, info.reason, info.can_act = 'forged', 'forged', true
      end)
      extensions.on('package_unload', function(info)
        assert(info.id == 'lifecycle' and info.event == 'package_unload' and info.reason == 'disable')
        assert(extensions.lobby() == nil and not extensions.scene_capabilities().native_ep3_media)
        assert(not extensions.scene_asset_support('asset').supported)
        assert(extensions.setting('tag') == 'audit')
        assert(extensions.storage_get('loads') == '1')
        extensions.log('unload:' .. info.reason)
      end)
      extensions.command('timer', {cooldown_ms=0}, function(ctx)
        assert(ctx.client_id == '42' and ctx.player_name == 'Alice' and ctx.can_act)
        extensions.after(3600000, function() end)
      end)
    )");
    {
      Runtime candidate(host, definition);
      check(host.logs.empty() && candidate.diagnostics().callbacks == 0 && candidate.storage().empty(), "candidate construction emitted lifecycle events");
      candidate.deactivate("shutdown");
      candidate.emit("package_load", {42, 1, "Alice"});
      candidate.emit("package_unload", {42, 1, "Alice"});
      check(host.logs.empty(), "ordinary event dispatch or staged destruction emitted lifecycle events");
    }
    check(host.logs.empty(), "unactivated candidate destruction ran unload");
    Runtime runtime(host, definition);
    rejects([&] { runtime.activate("invalid"); });
    runtime.activate("startup");
    runtime.activate("reload");
    check(host.logs == std::vector<std::string>{"load:startup"} && runtime.storage().at("loads") == "1" && runtime.storage_dirty(), "load duplicated or failed to initialize storage");
    check(host.actions == 0 && host.queries == 0 && runtime.diagnostics().callback_errors == 0, "load escaped its scope");
    runtime.dispatch("timer", "", {42, 1, "Alice"});
    check(runtime.timer_count() == 1, "lifecycle scope leaked into a normal player callback");
    runtime.deactivate("disable");
    runtime.deactivate("shutdown");
    runtime.activate("reload");
    check(host.logs.size() == 2 && host.logs.back() == "unload:disable" && !runtime.enabled() && runtime.timer_count() == 0 && host.queries == 0,
        "unload duplicated, reactivated a retired instance, or left timers");
    check(runtime.diagnostics().callbacks == 3, "lifecycle callback attempts were not counted");

    for (const auto& event : {"package_load", "package_unload"}) {
      for (const auto& action : {"extensions.reply('x')", "extensions.broadcast('x')", "extensions.whisper('1','x')",
               "extensions.after(50,function() end)", "extensions.cancel(1)", "extensions.scene_spawn('x','y','z')",
               "extensions.every(50,function() end)",
               "extensions.scene_spawn('x','y','z',50)",
               "extensions.scene_remove('x')", "extensions.scene_event(1)",
               "extensions.command('late',{},function() end)", "extensions.on('lobby_join',function() end)",
               "extensions.migrate(function() end)"}) {
        auto source = std::string("extensions.on('") + event + "',function(info) info.can_act=true; " + action + " end)";
        Runtime bad(host, package(source));
        auto actions = host.actions;
        bad.activate("reload");
        bad.deactivate("shutdown");
        check(bad.diagnostics().callback_errors == 1 && host.actions == actions, "package callback bypassed a player or registration boundary");
      }
    }
    Runtime read_only(host, package(R"(
      extensions.on('package_load',function() extensions.storage_set('key','original') end)
      extensions.on('package_unload',function() extensions.storage_set('key','replacement') end)
    )"));
    read_only.activate("reload");
    read_only.storage_saved();
    read_only.deactivate("reload");
    check(read_only.storage().at("key") == "original" && !read_only.storage_dirty() && read_only.diagnostics().callback_errors == 1,
        "unload mutated replacement storage or prevented retirement");
    auto no_storage = package("extensions.on('package_load',function() extensions.storage_set('key','x') end)");
    no_storage.grants.erase("storage");
    Runtime denied_storage(host, no_storage);
    denied_storage.activate("reload");
    check(denied_storage.storage().empty() && denied_storage.diagnostics().callback_errors == 1, "load bypassed storage grant");
    auto no_events = definition;
    no_events.grants.erase("events");
    rejects([&] { Runtime denied(host, no_events); });
    Runtime runaway(host, package(R"(
      extensions.on('package_load',function() while true do end end)
      extensions.command('ok',{cooldown_ms=0},function() extensions.storage_set('ok','yes') end)
    )"));
    runaway.activate("reload");
    runaway.dispatch("ok", "", {42, 1, "Alice"});
    check(runaway.diagnostics().callback_errors == 1 && runaway.storage().at("ok") == "yes", "runaway load damaged normal callbacks");
    Runtime faulted(host, package(R"(
      for i=1,3 do extensions.on('package_load',function() error('load fault') end) end
      extensions.on('package_unload',function() extensions.log('must-not-run') end)
    )"));
    faulted.activate("reload");
    auto logs = host.logs.size();
    faulted.deactivate("shutdown");
    check(!faulted.enabled() && faulted.diagnostics().callback_errors == 3 && host.logs.size() == logs, "faulted package ran more lifecycle handlers");
    Runtime budget(host, package("extensions.on('package_load',function() for i=1,33 do extensions.setting('tag') end end)"));
    budget.activate("reload");
    check(budget.diagnostics().callback_errors == 1, "load bypassed API budget");
    Runtime unload_error(host, package("extensions.on('package_unload',function() while true do end end)"));
    unload_error.activate("reload");
    unload_error.deactivate("shutdown");
    check(!unload_error.enabled() && unload_error.diagnostics().callback_errors == 1, "runaway unload prevented retirement");

    if (argc > 1) {
      std::ifstream file(argv[1]);
      check(file.good(), "cannot read package-audit example");
      auto example = package(std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>()));
      example.grants = {"events", "storage"};
      Runtime audit(host, example);
      audit.activate("startup");
      check(audit.storage().at("activations") == "1", "package-audit did not count activation");
      audit.deactivate("reload");
      example.storage = audit.storage();
      Runtime replacement(host, example);
      replacement.activate("reload");
      check(replacement.storage().at("activations") == "2", "package-audit did not retain counts");
      replacement.deactivate("shutdown");
      check(replacement.diagnostics().callback_errors == 0, "package-audit failed in restricted scopes");
    }
    std::cout << "Package lifecycle scopes, idempotency, storage, faults, budgets, retirement and example passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
