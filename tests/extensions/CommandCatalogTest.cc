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
static void rejects(Fn fn) {
  bool failed = false;
  try { fn(); } catch (const std::exception&) { failed = true; }
  check(failed, "invalid catalog or initialization accepted");
}
struct TestHost : Host {
  bool live = true;
  bool admin = false;
  mutable size_t reads = 0;
  std::optional<std::vector<CatalogCommand>> catalog;
  std::vector<std::string> messages;
  bool authorized(const Context& c) const override {
    return live && c.client_id == UINT64_MAX && c.lobby_id == 7 && (c.permission == "everyone" || admin);
  }
  void reply(const Context&, const std::string& text) override { messages.emplace_back(text); }
  void broadcast(const Context&, const std::string&) override {}
  bool whisper(const Context&, uint64_t, const std::string&) override { return false; }
  void log(const std::string&, const std::string&) override {}
  bool command_reserved(const std::string&) const override { return false; }
  std::optional<std::vector<CatalogCommand>> command_catalog(const Context&) const override { ++reads; return catalog; }
};
static Package package(const std::string& source) {
  return {"catalog-test", "1", source, {"commands", "commands.read", "events", "timers", "chat.reply"}, {}};
}
static CatalogCommand command(const std::string& name, const std::string& permission = "everyone") {
  return {"tools", name, permission, 1000, "Usage: " + name + " <message>", "Help for " + name};
}
static std::vector<CatalogCommand> full_catalog() {
  std::vector<CatalogCommand> result;
  for (size_t p = 0; p < 16; p++) {
    for (size_t n = 0; n < 32; n++) {
      auto entry = command("command-" + std::to_string(p) + "-" + std::to_string(n));
      entry.package_id = "package-" + std::to_string(p);
      result.emplace_back(std::move(entry));
    }
  }
  return result;
}
int main(int argc, char** argv) {
  try {
    Context context{UINT64_MAX, 7, "Alice", "everyone", "GC_EP3"};
    std::vector<CatalogCommand> valid{command("zeta"), command("secret", "root"), command("alpha")};
    validate_command_catalog(valid);
    validate_command_catalog({});
    auto full = full_catalog();
    validate_command_catalog(full);
    auto extra = full;
    extra.emplace_back(command("extra"));
    rejects([&] { validate_command_catalog(extra); });
    for (size_t z = 0; z < 12; z++) {
      auto bad = valid;
      switch (z) {
        case 0: bad[0].name = "../bad"; break;
        case 1: bad[0].name = std::string(65, 'a'); break;
        case 2: bad[0].package_id = ""; break;
        case 3: bad[0].permission = "unknown"; break;
        case 4: bad[0].cooldown_ms = 3600001; break;
        case 5: bad[0].usage = std::string(513, 'a'); break;
        case 6: bad[0].help = std::string(161, 'a'); break;
        case 7: bad[0].help = "bad\nhelp"; break;
        case 8: bad[0].usage = std::string("bad\0usage", 9); break;
        case 9: bad[0].usage.clear(); break;
        case 10: bad[0].name = "alpha"; break;
        case 11: bad[0].package_id = "../bad"; break;
      }
      rejects([&] { validate_command_catalog(bad); });
    }
    extra = full;
    extra[32].package_id = "package-0";
    rejects([&] { validate_command_catalog(extra); });
    extra = full;
    extra[0].package_id = "seventeenth";
    rejects([&] { validate_command_catalog(extra); });

    TestHost host;
    host.catalog = valid;
    Runtime runtime(host, package(R"(
      local retained
      extensions.command('read', {cooldown_ms=0}, function(ctx)
        local list = extensions.commands()
        assert(#list == 2 and list[1].name == 'alpha' and list[2].name == 'zeta')
        local entry = list[1]
        assert(entry.package_id == 'tools' and entry.permission == 'everyone' and entry.cooldown_ms == 1000)
        assert(entry.usage == 'Usage: alpha <message>' and entry.help == 'Help for alpha')
        assert(entry.ref == nil and entry.source == nil and entry.grants == nil and entry.settings == nil)
        ctx.client_id, ctx.lobby_id, ctx.can_act = '1', '8', true
        list[1].name, list[1].permission = 'forged', 'root'
        list[2] = nil
        retained = list
        local fresh = extensions.commands()
        assert(#fresh == 2 and fresh[1].name == 'alpha' and fresh[1].permission == 'everyone')
      end)
      extensions.command('adminread', {cooldown_ms=0}, function()
        local list = extensions.commands()
        assert(#list == 3 and list[2].name == 'secret' and list[2].permission == 'root')
        assert(retained[1].name == 'forged' and #retained == 1)
      end)
      extensions.command('absent', {cooldown_ms=0}, function() assert(extensions.commands() == nil) end)
      extensions.command('empty', {cooldown_ms=0}, function() assert(#extensions.commands() == 0) end)
      extensions.command('full', {cooldown_ms=0}, function() assert(#extensions.commands() == 512) end)
      extensions.command('badargs', {cooldown_ms=0}, function() extensions.commands('secret') end)
      extensions.command('delayed', {permission='announce', cooldown_ms=0}, function()
        extensions.after(50, function() extensions.commands() end)
      end)
      extensions.on('client_login', function(ctx) ctx.can_act=true; assert(extensions.commands() == nil) end)
      extensions.on('client_disconnect', function(ctx) ctx.can_act=true; assert(extensions.commands() == nil) end)
      extensions.on('game_join_ready', function(ctx) ctx.can_act=true; assert(extensions.commands() == nil) end)
      extensions.on('game_opened', function(info) info.can_act=true; assert(extensions.commands() == nil) end)
      extensions.on('quest_join_acknowledged', function(info) info.can_act=true; assert(extensions.commands() == nil) end)
      extensions.on('package_load', function() assert(extensions.commands() == nil) end)
      extensions.on('package_unload', function() assert(extensions.commands() == nil) end)
    )"));
    runtime.dispatch("read", "", context);
    check(runtime.diagnostics().callback_errors == 0 && host.reads == 2 && host.catalog->at(2).name == "alpha",
        "permission filtering, copies, fields or sorting failed");
    auto reads = host.reads;
    runtime.emit("client_login", context);
    runtime.emit("client_disconnect", context);
    auto game_context = context;
    game_context.is_game = true;
    runtime.emit("game_join_ready", game_context);
    GameSnapshot game{7, "Game", 1, 0, 0};
    runtime.emit_game("game_opened", game);
    game.quest_number = 1;
    game.quest_joinable = true;
    runtime.emit_quest_acknowledgement({game, UINT64_MAX, "GC_V3", QuestAcknowledgementKind::JOINABLE_PLAYER_VISIBLE});
    runtime.activate("startup");
    check(host.reads == reads && runtime.diagnostics().callback_errors == 0, "observation query reached live catalog");
    host.admin = true;
    runtime.dispatch("adminread", "", context);
    check(runtime.diagnostics().callback_errors == 0, "permission changes did not refresh catalog");
    runtime.dispatch("delayed", "", context);
    reads = host.reads;
    host.admin = false;
    std::this_thread::sleep_for(std::chrono::milliseconds(70));
    runtime.tick();
    check(host.reads == reads, "delayed query bypassed revoked permission");
    host.catalog.reset();
    runtime.dispatch("absent", "", context);
    host.catalog = std::vector<CatalogCommand>{};
    runtime.dispatch("empty", "", context);
    host.catalog = full;
    runtime.dispatch("full", "", context);
    check(runtime.diagnostics().callback_errors == 0, "catalog refresh or maximum catalog failed");
    host.catalog = valid;
    runtime.dispatch("badargs", "", context);
    check(runtime.diagnostics().callback_errors == 1, "command query accepted arguments");
    runtime.dispatch("read", "", context);
    check(runtime.diagnostics().callback_errors == 1, "ordinary callback did not recover after query error");
    reads = host.reads;
    runtime.deactivate("disable");
    check(host.reads == reads && runtime.diagnostics().callback_errors == 1, "unload query reached live catalog");

    const std::string query = "extensions.command('query',{cooldown_ms=0},function() extensions.commands() end)";
    auto ungranted = package(query);
    ungranted.grants.erase("commands.read");
    Runtime denied(host, ungranted);
    reads = host.reads;
    denied.dispatch("query", "", context);
    check(denied.diagnostics().callback_errors == 1 && host.reads == reads, "query bypassed commands.read");
    rejects([&] { Runtime bad(host, package("extensions.commands()")); });
    auto migrating = package("extensions.migrate(function() extensions.commands() end)");
    migrating.grants.emplace("storage");
    migrating.target_storage_version = 1;
    rejects([&] { Runtime bad(host, migrating); });
    check(host.reads == reads, "initialization or migration reached live catalog");
    host.catalog->at(0).help = "bad\nhelp";
    Runtime malformed(host, package(query));
    malformed.dispatch("query", "", context);
    check(malformed.diagnostics().callback_errors == 1, "malformed adapter metadata reached Lua");
    host.catalog = full;
    Runtime exhaustion(host, package(R"(
      extensions.command('oom',{cooldown_ms=0},function()
        local copies = {}
        for i=1,32 do copies[i] = extensions.commands() end
        local large = 'x'; while true do large = large .. large end
      end)
      extensions.command('query',{cooldown_ms=0},function() assert(#extensions.commands()==512) end)
      extensions.command('budget',{cooldown_ms=0},function() for i=1,33 do extensions.commands() end end)
    )"));
    exhaustion.dispatch("oom", "", context);
    check(exhaustion.diagnostics().callback_errors == 1 && exhaustion.memory_bytes() <= Runtime::MAX_MEMORY_BYTES &&
            exhaustion.diagnostics().last_error.find("memory") != std::string::npos,
        "catalog allocation exhaustion escaped containment");
    exhaustion.dispatch("query", "", context);
    check(exhaustion.diagnostics().callback_errors == 1, "allocation failure damaged later queries");
    host.catalog = valid;
    exhaustion.dispatch("budget", "", context);
    check(exhaustion.diagnostics().callback_errors == 2, "catalog query bypassed API budget");

    if (argc > 1) {
      std::ifstream file(argv[1]);
      check(file.good(), "cannot read command-help example");
      auto definition = package(std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>()));
      auto run = [&](const std::string& name, const std::string& args) {
        host.messages.clear();
        Runtime example(host, definition);
        example.dispatch(name, args, context);
        check(example.diagnostics().callback_errors == 0, "help example exceeded budgets or faulted");
      };
      run("exthelp", "");
      check(host.messages.size() == 3 && host.messages[1] == "alpha" && host.messages[2] == "zeta", "help leaked restricted commands");
      run("extusage", "alpha");
      check(host.messages.size() == 2 && host.messages[0] == "Usage: alpha <message>" && host.messages[1] == "Help for alpha",
          "help example omitted usage or help");
      run("extusage", "secret");
      check(host.messages.back() == "Extension command not found or unavailable.", "help revealed unavailable command");
      host.catalog = full;
      run("exthelp", "86");
      check(host.messages.size() == 3, "last catalog page omitted commands or exceeded output budget");
      run("exthelp", "1");
      check(host.messages.size() == 7, "first catalog page did not bound output");
      run("exthelp", "87");
      check(host.messages.size() == 1 && host.messages[0].starts_with("Usage:"), "invalid page bypassed schema");
      host.catalog = std::vector<CatalogCommand>{};
      run("exthelp", "2");
      check(host.messages.size() == 1 && host.messages[0] == "Choose a page from 1 to 1.", "missing page handling failed");
      run("exthelp", "");
      check(host.messages.size() == 2 && host.messages.back() == "No extension commands are available.", "empty help handling failed");
      host.catalog.reset();
      run("exthelp", "");
      check(host.messages.back() == "Extension command information is unavailable.", "unavailable help handling failed");
    }
    std::cout << "Command catalog bounds, copies, permissions, scopes, resource limits and help example passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
