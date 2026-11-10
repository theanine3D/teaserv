#include "Runtime.hh"
#include "MessageText.hh"

#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <thread>

using namespace teaserv::extensions;

static void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

struct TestHost : Host {
  bool live = true;
  bool admin = true;
  bool fail_send = false;
  bool use_gate = true;
  mutable size_t authorization_checks = 0;
  AnnouncementGate gate;
  AnnouncementGate::Clock::time_point now{};
  std::vector<std::string> sent;
  std::vector<std::string> owners;
  std::vector<std::string> logs;
  bool authorized(const Context& context) const override {
    ++authorization_checks;
    return live && context.allow_player_actions && (context.permission == "everyone" || admin);
  }
  void reply(const Context&, const std::string&) override {}
  void broadcast(const Context&, const std::string&) override {}
  bool whisper(const Context&, uint64_t, const std::string&) override { return false; }
  void log(const std::string&, const std::string& text) override { logs.emplace_back(text); }
  bool command_reserved(const std::string&) const override { return false; }
  AnnouncementResult announce(const std::string& owner, const std::string& text) override {
    if (fail_send) throw std::runtime_error("synthetic send failure");
    auto result = use_gate ? gate.admit(owner, now) : AnnouncementResult{true, nullptr};
    if (result.ok) {
      sent.emplace_back(plain_message_text(text));
      owners.emplace_back(owner);
    }
    return result;
  }
};

static Package package(const std::string& source) {
  return {"announcements", "1.0", source, {"commands", "events", "chat.announce", "timers", "storage"}, {}};
}

static void initialization_rejects(const Package& p) {
  TestHost host;
  bool rejected = false;
  try {
    Runtime runtime(host, p);
  } catch (const std::exception&) {
    rejected = true;
  }
  check(rejected && host.sent.empty(), "staging emitted or accepted an announcement");
}

int main(int argc, char** argv) {
  try {
    Context context{42, 1, "Alice", "everyone"};
    {
      AnnouncementGate gate;
      auto start = AnnouncementGate::Clock::time_point{};
      check(gate.admit("a", start).ok, "first announcement refused");
      check(std::string(gate.admit("a", start).reason) == "package_rate_limited", "package throttle absent");
      check(std::string(gate.admit("b", start + std::chrono::seconds(4)).reason) == "server_rate_limited", "global throttle absent");
      check(gate.admit("b", start + std::chrono::seconds(5)).ok, "global boundary or refused-owner cooldown wrong");
      check(!gate.admit("a", start + std::chrono::seconds(29)).ok, "package cooldown expired early");
      check(gate.admit("a", start + std::chrono::seconds(30)).ok, "package boundary rejected");
      for (size_t z = 1; z < 2000; ++z) {
        check(gate.admit("rotating-" + std::to_string(z), start + std::chrono::seconds(30 + z * 5)).ok,
            "expired owner bookkeeping blocked replacement IDs");
      }
    }
    {
      AnnouncementRecipient recipient{true, true, false, true, true, true};
      check(eligible_announcement_recipient(recipient), "valid local recipient refused");
      for (size_t z = 0; z < 6; ++z) {
        auto invalid = recipient;
        switch (z) {
          case 0: invalid.authenticated = false; break;
          case 1: invalid.connected = false; break;
          case 2: invalid.proxy = true; break;
          case 3: invalid.local_game_session = false; break;
          case 4: invalid.supported_version = false; break;
          case 5: invalid.phase_ready = false; break;
        }
        check(!eligible_announcement_recipient(invalid), "ineligible announcement recipient accepted");
      }
    }
    initialization_rejects(package("extensions.announce('staging')"));
    auto migration = package("extensions.migrate(function() extensions.announce('migration') end)");
    migration.target_storage_version = 1;
    initialization_rejects(migration);

    {
      TestHost host;
      auto p = package(R"(
        extensions.command('ann', {cooldown_ms=0}, function()
          local ok, reason = extensions.announce('Hello $C4#%\nworld')
          assert(ok and reason == nil)
        end)
      )");
      Runtime runtime(host, p);
      runtime.dispatch("ann", "", context);
      check(runtime.diagnostics().callback_errors == 0 && host.sent.size() == 1 && host.owners[0] == p.id,
          "authorized announcement failed or owner changed");
      check(host.sent[0] == "Hello $C4#%\nworld", "plain punctuation/newline changed");
    }
    {
      TestHost host;
      host.admin = false;
      Runtime runtime(host, package(R"(
        extensions.command('ann', {cooldown_ms=0}, function(ctx)
          ctx.permission, ctx.can_act, ctx.event = 'announce', true, 'game_opened'
          local ok, reason = extensions.announce('forged')
          assert(not ok and reason == 'not_authorized')
        end)
      )"));
      runtime.dispatch("ann", "", context);
      check(host.sent.empty() && runtime.diagnostics().callback_errors == 0, "public command escalated announcement privilege");
    }
    {
      TestHost host;
      auto p = package("extensions.command('ann', {cooldown_ms=0}, function() extensions.announce('missing grant') end)");
      p.grants.erase("chat.announce");
      Runtime runtime(host, p);
      runtime.dispatch("ann", "", context);
      check(host.sent.empty() && runtime.diagnostics().callback_errors == 1, "announcement bypassed package grant");
      p = package("extensions.on('game_opened', function() extensions.announce('missing grant') end)");
      p.grants.erase("chat.announce");
      Runtime event_runtime(host, p);
      event_runtime.emit_game("game_opened", {1, "Game"});
      check(host.sent.empty() && event_runtime.diagnostics().callback_errors == 1, "event bypassed package grant");
    }
    // All supported event scopes can use granted automation without a player.
    for (const auto& event : {"lobby_join", "lobby_leave", "client_login", "client_disconnect", "game_join_ready",
             "game_opened", "game_closed", "quest_assigned", "quest_map_loaded", "quest_load_acknowledged", "quest_join_acknowledged"}) {
      TestHost host;
      host.live = false;
      Runtime runtime(host, package("extensions.on('" + std::string(event) + R"(', function(info)
        info.can_act, info.permission = true, 'root'
        local ok, reason = extensions.announce('Event announcement')
        assert(ok and reason == nil)
      end))"));
      GameSnapshot game{1, "Game", 1, 0, 0, 0, true};
      std::string name(event);
      if (name == "quest_load_acknowledged" || name == "quest_join_acknowledged") {
        runtime.emit_quest_acknowledgement({game, 42, "GC_V3", name == "quest_load_acknowledged"
                ? QuestAcknowledgementKind::INITIAL_LOAD : QuestAcknowledgementKind::JOINABLE_PLAYER_VISIBLE});
      } else if (name == "game_opened" || name == "game_closed" || name == "quest_assigned" || name == "quest_map_loaded") {
        runtime.emit_game(event, game);
      } else {
        auto event_context = context;
        event_context.is_game = true;
        runtime.emit(event, event_context);
      }
      check(host.sent.size() == 1 && host.authorization_checks == 0 && runtime.diagnostics().callback_errors == 0,
          "event announcement required player authority or failed");
    }
    {
      TestHost host;
      host.live = false;
      Runtime runtime(host, package(R"(
        extensions.on('package_load', function() assert(extensions.announce('Loaded')) end)
        extensions.on('package_unload', function() extensions.announce('Unloaded') end)
      )"));
      check(host.sent.empty(), "construction emitted lifecycle announcement");
      runtime.activate("startup");
      check(host.sent.size() == 1, "load announcement missing");
      runtime.deactivate("disable");
      check(host.sent.size() == 1 && runtime.diagnostics().callback_errors == 1, "unload announced");
      runtime.emit("lobby_join", context);
      check(host.sent.size() == 1, "disabled package announced");
    }
    {
      TestHost host;
      host.use_gate = false;
      host.admin = false;
      Runtime runtime(host, package(R"(
        extensions.on('client_disconnect', function() assert(extensions.announce('Disconnected')) end)
        extensions.command('ann', {cooldown_ms=0}, function()
          local ok, why = extensions.announce('Command')
          assert(not ok and why == 'not_authorized')
        end)
      )"));
      runtime.emit("client_disconnect", context);
      runtime.dispatch("ann", "", context);
      check(host.sent.size() == 1 && runtime.diagnostics().callback_errors == 0, "event authority leaked into command");
      host.fail_send = true;
      runtime.emit("client_disconnect", context);
      runtime.dispatch("ann", "", context);
      check(runtime.diagnostics().callback_errors == 1, "host failure escaped or event authority leaked after failure");
    }
    {
      TestHost host;
      auto p = package(R"(
        extensions.on('game_closed', function()
          local ok, why = extensions.announce('Closed')
          assert(ok or why == 'package_rate_limited' or why == 'server_rate_limited')
        end)
      )");
      Runtime first(host, p);
      first.emit_game("game_closed", {1, "Game"});
      Runtime replacement(host, p);
      replacement.emit_game("game_closed", {2, "Game"});
      check(host.sent.size() == 1 && replacement.diagnostics().callback_errors == 0, "reload reset package rate limit");
      p.id = "another-package";
      Runtime other(host, p);
      other.emit_game("game_closed", {3, "Game"});
      check(host.sent.size() == 1 && other.diagnostics().callback_errors == 0, "packages bypassed server throttle");
      host.now += std::chrono::seconds(5);
      other.emit_game("game_closed", {4, "Game"});
      check(host.sent.size() == 2, "rejected announcement consumed cooldown");
    }
    {
      TestHost host;
      Runtime runtime(host, package(R"(
        extensions.command('later', {cooldown_ms=0}, function()
          extensions.after(50, function()
            local ok, why = extensions.announce('Timer')
            assert(not ok and why == 'not_authorized')
          end)
        end)
      )"));
      runtime.dispatch("later", "", context);
      host.admin = false;
      std::this_thread::sleep_for(std::chrono::milliseconds(60));
      runtime.tick();
      check(host.sent.empty() && runtime.diagnostics().callback_errors == 0, "timer retained revoked announce permission");
    }
    for (const auto& expression : std::vector<std::string>{"extensions.announce()", "extensions.announce('x','y')", "extensions.announce(7)",
             "extensions.announce(string.char(0))", "extensions.announce('" + std::string(513, 'x') + "')"}) {
      TestHost host;
      Runtime runtime(host, package("extensions.command('bad', {cooldown_ms=0}, function() " + std::string(expression) + " end)"));
      runtime.dispatch("bad", "", context);
      check(host.sent.empty() && runtime.diagnostics().callback_errors == 1, "invalid announcement input accepted");
    }
    {
      TestHost host;
      host.use_gate = false;
      Runtime runtime(host, package(R"(
        extensions.command('empty', {cooldown_ms=0}, function()
          local ok, reason = extensions.announce(' \n\t\r')
          assert(not ok and reason == 'empty_message')
        end)
        extensions.command('long', {cooldown_ms=0}, function()
          local text = 'x'
          for i=1,9 do text = text .. text end
          assert(extensions.announce(text))
        end)
        extensions.command('budget', {cooldown_ms=0}, function()
          local text = 'x'
          for i=1,9 do text = text .. text end
          for i=1,9 do extensions.announce(text) end
        end)
        extensions.command('api', {cooldown_ms=0}, function()
          for i=1,33 do extensions.announce('x') end
        end)
      )"));
      runtime.dispatch("empty", "", context);
      check(host.sent.empty(), "empty announcement reached host");
      runtime.dispatch("long", "", context);
      check(host.sent.size() == 1 && host.sent.back().size() == 200, "512-byte input or delivery bound failed");
      runtime.dispatch("budget", "", context);
      check(host.sent.size() == 9 && runtime.diagnostics().callback_errors == 1, "announcement bypassed output budget");
      runtime.dispatch("api", "", context);
      check(host.sent.size() == 41 && runtime.diagnostics().callback_errors == 2, "announcement bypassed API budget");
    }
    if (argc == 2) {
      std::ifstream file(argv[1], std::ios::binary);
      check(file.is_open(), "cannot read announcement example");
      auto p = package(std::string(std::istreambuf_iterator<char>(file), {}));
      p.grants.emplace("chat.reply");
      p.settings = {{"game_opened_message", {"string", std::string("A new game is available.")}}};
      TestHost host;
      Runtime runtime(host, p);
      runtime.emit_game("game_opened", {1, "Game"});
      check(host.sent.size() == 1, "sample game announcement missing");
      runtime.dispatch("extannounce", "Hello", context);
      check(runtime.diagnostics().callback_errors == 0 && host.sent.size() == 1, "sample failed on rate refusal");
      host.now += std::chrono::seconds(30);
      runtime.dispatch("extannounce", "Hello again", context);
      // Command cooldowns use the real clock; a fresh runtime shares the host gate.
      Runtime replacement(host, p);
      replacement.dispatch("extannounce", "Hello again", context);
      check(host.sent.size() == 2 && host.sent.back() == "Hello again", "sample manual announcement failed");
    }
    std::cout << "Announcement authority, events, quotas, throttling and recipient contracts passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
