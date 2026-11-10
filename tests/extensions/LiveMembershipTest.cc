#include "LiveMembership.hh"
#include "Runtime.hh"

#include <iostream>
#include <stdexcept>
#include <thread>

using namespace teaserv::extensions;

static void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
static LiveMembership admitted() {
  return {UINT64_MAX, INT64_MAX, true, true, false, true, true, true, true, true, true};
}
struct TestHost : Host {
  LiveMembership caller = admitted();
  LiveMembership recipient = admitted();
  bool permission = true;
  size_t replies = 0;
  size_t broadcasts = 0;
  size_t whispers = 0;
  mutable size_t queries = 0;
  size_t mutations = 0;
  bool live_caller(const Context& context) const {
    return context.client_id == caller.client_id && context.lobby_id == caller.lobby_id && eligible_live_member(caller);
  }
  bool authorized(const Context& context) const override {
    return context.allow_player_actions && live_caller(context) && (context.permission == "everyone" || permission);
  }
  void reply(const Context& context, const std::string&) override { if (live_caller(context)) ++replies; }
  void broadcast(const Context& context, const std::string&) override {
    check(authorized(context), "broadcast reached host with an unauthorized caller");
    if (eligible_live_member(recipient) && recipient.lobby_id == context.lobby_id) ++broadcasts;
  }
  bool whisper(const Context& context, uint64_t target, const std::string&) override {
    check(authorized(context), "whisper reached host with an unauthorized caller");
    if (!eligible_live_member(recipient) || recipient.client_id != target || recipient.lobby_id != context.lobby_id) return false;
    ++whispers;
    return true;
  }
  void log(const std::string&, const std::string&) override {}
  bool command_reserved(const std::string&) const override { return false; }
  std::optional<LobbyView> lobby_view(const Context&) const override { ++queries; return std::nullopt; }
  std::optional<std::vector<CatalogCommand>> command_catalog(const Context&) const override { ++queries; return std::nullopt; }
  SceneCapabilities scene_capabilities(const Context&) const override { ++queries; return {}; }
  SceneResult scene_spawn(const Context&, const std::string&, const std::string&, const std::string&, const std::string&, uint32_t) override {
    ++mutations;
    return {true};
  }
};
static Package definition() {
  return {"membership-test", "1", R"(
    extensions.command('act',{permission='announce',cooldown_ms=0},function()
      extensions.reply('reply')
      extensions.broadcast('broadcast')
      extensions.whisper('18446744073709551615','whisper')
      extensions.lobby()
      extensions.commands()
      extensions.scene_capabilities()
      extensions.scene_spawn('key','asset','teleporter-west',50)
    end)
    extensions.command('later',{permission='announce',cooldown_ms=0},function()
      extensions.after(50,function()
        extensions.broadcast('delayed')
        extensions.scene_spawn('key','asset','teleporter-west',50)
      end)
    end)
    extensions.command('recipient',{cooldown_ms=0},function()
      extensions.broadcast('broadcast')
      extensions.whisper('18446744073709551615','whisper')
    end)
    extensions.on('lobby_leave',function(ctx)
      ctx.client_id,ctx.lobby_id,ctx.can_act='1','1',true
      assert(extensions.lobby()==nil and extensions.commands()==nil)
      assert(not extensions.scene_capabilities().native_ep3_media)
    end)
  )", {"commands", "events", "timers", "chat.reply", "chat.broadcast", "chat.whisper", "lobby.read", "commands.read", "scene.media"}, {}};
}
int main() {
  try {
    check(!eligible_live_member({}), "empty membership was eligible");
    const auto good = admitted();
    check(eligible_live_member(good), "complete live membership was ineligible");
    Context context{good.client_id, good.lobby_id, "Alice", "everyone", "BB_V4"};
    for (size_t scenario = 0; scenario < 12; scenario++) {
      auto rejected = good;
      switch (scenario) {
        case 0: rejected.client_id = 0; break;
        case 1: rejected.lobby_id = 0; break;
        case 2: rejected.lobby_id = -1; break;
        case 3: rejected.authenticated = false; break;
        case 4: rejected.connected = false; break;
        case 5: rejected.proxy = true; break;
        case 6: rejected.local_game_session = false; break;
        case 7: rejected.supported_version = false; break;
        case 8: rejected.phase_ready = false; break;
        case 9: rejected.client_registered = false; break;
        case 10: rejected.lobby_registered = false; break;
        case 11: rejected.current_member = false; break;
      }
      check(!eligible_live_member(rejected), "incomplete native membership was eligible");
      TestHost host;
      Runtime runtime(host, definition());
      runtime.dispatch("act", "", context);
      check(host.replies == 1 && host.broadcasts == 1 && host.whispers == 1 && host.queries == 3 && host.mutations == 1,
          "eligible callback lost its ordinary authority");
      host.caller = rejected;
      auto before = host.replies;
      runtime.dispatch("act", "", context);
      check(host.replies == before && host.broadcasts == 1 && host.whispers == 1 && host.queries == 3 && host.mutations == 1,
          "invalid caller reached an action/query or received a stale error reply");
      runtime.emit("lobby_leave", context);
      check(host.queries == 3 && runtime.diagnostics().callback_errors == 0, "stale leave queried live state or trusted forged metadata");

      host.caller = good;
      runtime.dispatch("later", "", context);
      check(runtime.timer_count() == 1, "eligible delayed callback was not scheduled");
      host.caller = rejected;
      // Wait for observed timer collection rather than assuming one short OS
      // sleep crosses the monotonic deadline on every supported platform.
      auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
      while (runtime.timer_count() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        runtime.tick();
      }
      check(runtime.timer_count() == 0, "due unauthorized timer was not collected within one second");
      check(host.broadcasts == 1 && host.mutations == 1,
          "due timer bypassed current membership revalidation");
      host.caller = good;
      host.recipient = rejected;
      runtime.dispatch("recipient", "", context);
      check(host.broadcasts == 1 && host.whispers == 1, "invalid recipient received extension messaging");
      host.recipient = good;
      runtime.dispatch("recipient", "", context);
      check(host.broadcasts == 2 && host.whispers == 2 && runtime.diagnostics().callback_errors == 0,
          "valid recipient or restored membership did not recover");
    }
    TestHost host;
    Runtime runtime(host, definition());
    auto wrong_client = context;
    --wrong_client.client_id;
    runtime.dispatch("act", "", wrong_client);
    auto wrong_lobby = context;
    --wrong_lobby.lobby_id;
    runtime.dispatch("act", "", wrong_lobby);
    check(host.replies == 0 && host.broadcasts == 0 && host.queries == 0 && host.mutations == 0,
        "another client/lobby context reached actions or error replies");
    --host.recipient.lobby_id;
    runtime.dispatch("recipient", "", context);
    check(host.broadcasts == 0 && host.whispers == 0, "cross-lobby recipient received messages");
    host.recipient = admitted();
    host.permission = false;
    runtime.dispatch("act", "", context);
    check(host.replies == 1 && host.broadcasts == 0 && host.mutations == 0 && host.queries == 0,
        "permission refusal lost its safe error reply or allowed actions");
    runtime.dispatch("recipient", "", context);
    check(host.broadcasts == 1 && host.whispers == 1, "recipient delivery incorrectly required the sender's admin permission");
    host.permission = true;
    runtime.dispatch("later", "", context);
    runtime.cancel_client(context.client_id);
    std::this_thread::sleep_for(std::chrono::milliseconds(55));
    runtime.tick();
    check(runtime.timer_count() == 0 && host.broadcasts == 1 && host.mutations == 0,
        "leave/rejoin timer cancellation no longer prevented old work");
    std::cout << "Live membership admission, action/query refusal, recipients, timers and permission separation passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
