#include "Runtime.hh"
#include "MessageText.hh"

#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <thread>

using namespace teaserv::extensions;

struct TestHost : Host {
  bool live = true;
  bool admin = false;
  bool target_present = true;
  uint64_t last_reply_client_id = 0;
  bool slow_reply = false;
  bool media_available = false;
  bool extended_media = false;
  std::string scene_owner;
  size_t scene_calls = 0;
  mutable size_t asset_queries = 0;
  mutable std::string queried_owner;
  mutable std::string queried_asset;
  SceneAssetSupport asset_support{true, nullptr, "fallback"};
  uint32_t scene_lifetime_ms = 0;
  std::vector<std::string> messages;
  std::vector<std::string> logs;
  bool authorized(const Context& c) const override { return live && (c.permission == "everyone" || admin); }
  void reply(const Context& context, const std::string& text) override {
    if (slow_reply) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    last_reply_client_id = context.client_id;
    messages.emplace_back(text);
  }
  void broadcast(const Context&, const std::string& text) override { messages.emplace_back(text); }
  bool whisper(const Context&, uint64_t target, const std::string& text) override {
    if (!target_present) {
      return false;
    }
    messages.emplace_back(std::to_string(target) + ":" + text);
    return true;
  }
  void log(const std::string&, const std::string& text) override { logs.emplace_back(text); }
  bool command_reserved(const std::string& name) const override { return name == "debug"; }
  SceneCapabilities scene_capabilities(const Context&) const override { return {media_available, false, admin, extended_media}; }
  SceneAssetSupport scene_asset_support(const Context&, const std::string& owner, const std::string& asset) const override {
    ++asset_queries;
    queried_owner = owner;
    queried_asset = asset;
    return asset_support;
  }
  SceneResult scene_spawn(const Context&, const std::string& owner, const std::string&, const std::string&, const std::string&, uint32_t lifetime_ms) override {
    if (!media_available) {
      return {false, "unsupported recipient"};
    }
    scene_owner = owner;
    scene_lifetime_ms = lifetime_ms;
    ++scene_calls;
    return {true};
  }
  SceneResult scene_remove(const Context&, const std::string& owner, const std::string&) override {
    scene_owner = owner;
    ++scene_calls;
    return {true};
  }
  SceneResult scene_event(const Context&, const std::string& owner, int) override {
    scene_owner = owner;
    return admin ? SceneResult{true} : SceneResult{false, "permission denied"};
  }
};

static void check(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

static Package package(const std::string& source) {
  return {"test", "1.0", source, {"commands", "events", "timers", "chat.reply", "chat.broadcast", "chat.whisper", "storage"}, {}};
}

static void rejects(const std::string& source) {
  TestHost host;
  bool rejected = false;
  try {
    check(plain_message_text("a\tb\rc\x1b\nd") == "abc\nd", "message control filtering failed");
    check(plain_message_text("$C4#%") == "$C4#%", "plain-text punctuation changed");
    check(plain_message_text(std::string(300, 'a')).size() == 200, "legacy text delivery limit exceeded");
    check(plain_message_text(std::string(199, 'a') + "\xE2\x82\xAC").size() == 199, "UTF-8 character split at delivery boundary");
    Runtime runtime(host, package(source));
  } catch (const std::exception&) {
    rejected = true;
  }
  check(rejected, "bad package was accepted");
}

int main(int argc, char** argv) {
  try {
    Context context{42, 1, "Alice", "everyone"};
    TestHost host;
    Runtime runtime(host, package(R"(
      assert(io == nil and os == nil and package == nil and debug == nil)
      assert(load == nil and dofile == nil and pcall == nil and coroutine == nil)
      assert(setmetatable == nil and string.dump == nil and string.match == nil)
      assert(string.rep == nil and table.move == nil and table.sort == nil)
      extensions.command('hello', {cooldown_ms=0}, function(ctx, args)
        extensions.reply(ctx.player_name .. ':' .. args)
      end)
      extensions.command('private', {cooldown_ms=0}, function(ctx, args)
        extensions.whisper('123', args)
      end)
      extensions.command('admin', {permission='announce', cooldown_ms=0}, function()
        extensions.broadcast('admin')
      end)
      extensions.command('store', {cooldown_ms=0}, function()
        extensions.storage_set('key', 'value')
        extensions.reply(extensions.storage_get('key'))
      end)
      extensions.command('timer', {cooldown_ms=0}, function()
        extensions.after(50, function() extensions.reply('later') end)
      end)
      extensions.on('lobby_join', function(ctx, event) extensions.reply(event) end)
    )"));
    check(!runtime.dispatch("missing", "", context), "unknown command was consumed");
    check(runtime.dispatch("hello", "world", context), "command not consumed");
    check(host.messages.back() == "Alice:world", "context or args mismatch");
    runtime.dispatch("private", "secret", context);
    check(host.messages.back() == "123:secret", "whisper target mismatch");
    runtime.dispatch("admin", "", context);
    check(host.messages.back() != "admin", "permission bypass");
    host.admin = true;
    runtime.dispatch("admin", "", context);
    check(host.messages.back() == "admin", "authorized command failed");
    runtime.dispatch("store", "", context);
    check(runtime.storage().at("key") == "value" && runtime.storage_dirty(), "storage did not persist in host state");
    runtime.emit("lobby_join", context);
    check(host.messages.back() == "lobby_join", "event not delivered");
    runtime.dispatch("timer", "", context);
    check(runtime.timer_count() == 1, "timer not scheduled");
    std::this_thread::sleep_for(std::chrono::milliseconds(70));
    runtime.tick();
    check(host.messages.back() == "later" && runtime.timer_count() == 0, "timer failed");
    runtime.dispatch("timer", "", context);
    runtime.cancel_client(context.client_id);
    check(runtime.timer_count() == 0, "disconnect did not cancel timers");
    runtime.dispatch("timer", "", context);
    host.live = false;
    size_t before = host.messages.size();
    std::this_thread::sleep_for(std::chrono::milliseconds(70));
    runtime.tick();
    check(host.messages.size() == before, "stale timer acted on departed client");
    host.live = true;

    Runtime typed(host, package(R"(
      extensions.command('typed', {cooldown_ms=0, arguments={
        {name='count', type='integer', min=-5, max=5},
        {name='target', type='client_id'},
        {name='message', type='text', max_bytes=20, optional=true}
      }}, function(ctx, raw, args)
        assert(type(args.count) == 'number' and type(args.target) == 'string')
        extensions.reply(tostring(args.count) .. ':' .. args.target .. ':' .. (args.message or 'absent'))
      end)
      extensions.command('none', {cooldown_ms=10000, arguments={}}, function(ctx, raw, args)
        assert(args ~= nil and next(args) == nil)
        extensions.reply('no-args')
      end)
      extensions.command('raw', {cooldown_ms=0}, function(ctx, raw, args)
        assert(args == nil)
        extensions.reply(raw)
      end)
      extensions.command('wide', {cooldown_ms=0, arguments={
        {name='value', type='integer', min=math.mininteger, max=math.maxinteger}
      }}, function(ctx, raw, args) extensions.reply(tostring(args.value)) end)
      extensions.command('word', {cooldown_ms=0, arguments={
        {name='name', type='word', max_bytes=8}
      }}, function(ctx, raw, args)
        assert(raw == '  hello\t' and args.name == 'hello')
        extensions.reply('word-ok')
      end)
      extensions.command('protected-args', {permission='root', arguments={
        {name='number', type='integer'}
      }}, function() error('should not run') end)
    )"));
    for (const auto& input : {"", "6 1", "1.0 1", "1e0 1", "+1 1", "1 0", "1 -1", "1 +1",
             "1 18446744073709551616", "1 000000000000000000001", "1 x", "-6 1", "1 1 too-many-message-bytes-here"}) {
      typed.dispatch("typed", input, context);
      check(host.messages.back() == "Usage: typed <count> <target> [message]", "invalid input reached Lua or had no usage reply");
    }
    typed.dispatch("typed", std::string("1 1 a\0b", 7), context);
    check(host.messages.back().starts_with("Usage:"), "NUL argument accepted");
    typed.dispatch("typed", "1 1 line\nbreak", context);
    check(host.messages.back().starts_with("Usage:"), "control argument accepted");
    typed.dispatch("typed", " -5\t18446744073709551615 hello  world ", context);
    check(typed.enabled() && host.messages.back() == "-5:18446744073709551615:hello  world ", "typed parsing lost ID precision or remainder spacing");
    typed.dispatch("typed", "5 00123", context);
    check(host.messages.back() == "5:00123:absent", "optional argument or ID spelling changed");
    typed.dispatch("wide", "-9223372036854775808", context);
    check(host.messages.back() == "-9223372036854775808", "minimum signed integer changed");
    typed.dispatch("wide", "9223372036854775807", context);
    check(host.messages.back() == "9223372036854775807", "maximum signed integer changed");
    typed.dispatch("wide", "9223372036854775808", context);
    check(host.messages.back().starts_with("Usage:"), "signed integer overflow accepted");
    typed.dispatch("word", "two words", context);
    check(host.messages.back().starts_with("Usage:"), "extra tokens accepted");
    typed.dispatch("word", "  hello\t", context);
    check(host.messages.back() == "word-ok", "raw arguments changed for schema handler");
    typed.dispatch("none", "unexpected", context);
    typed.dispatch("none", " \t", context);
    check(host.messages.back() == "no-args", "invalid arguments consumed cooldown or empty schema failed");
    typed.dispatch("raw", "legacy raw words", context);
    check(host.messages.back() == "legacy raw words", "legacy argument handler changed");
    host.admin = false;
    typed.dispatch("protected-args", "invalid", context);
    check(!host.messages.back().starts_with("Usage:"), "schema bypassed command authorization");
    host.admin = true;
    check(typed.enabled(), "bad user input faulted a package");
    {
      auto definition = package(R"(
        extensions.migrate(function(old, target)
          assert(old == 0 and target == 1)
          assert(extensions.storage_get('legacy') == '41')
          extensions.storage_set('count', '42')
          extensions.storage_set('legacy', nil)
        end)
        extensions.command('read', {cooldown_ms=0}, function()
          extensions.reply(extensions.storage_get('count'))
        end)
      )");
      definition.storage = {{"legacy", "41"}};
      definition.target_storage_version = 1;
      Runtime migrated(host, definition);
      check(migrated.storage_version() == 1 && migrated.storage_dirty() && migrated.storage().at("count") == "42" &&
          !migrated.storage().count("legacy"), "storage migration did not commit to staged runtime");
      check(definition.storage.at("legacy") == "41", "migration changed its source storage snapshot");
      migrated.dispatch("read", "", context);
      check(host.messages.back() == "42", "normal storage access failed after migration");
      migrated.storage_saved();
      check(!migrated.storage_dirty(), "saved migration remained dirty");
      auto current = definition;
      current.storage = migrated.storage();
      current.storage_version = 1;
      current.source = "extensions.migrate(function() error('must not run twice') end)";
      Runtime unchanged(host, current);
      check(unchanged.storage_version() == 1 && !unchanged.storage_dirty(), "current-version migration ran again");
      auto migration_rejected = [&](Package candidate) {
        bool failed = false;
        auto messages = host.messages.size();
        try { Runtime invalid(host, std::move(candidate)); }
        catch (const std::exception&) { failed = true; }
        check(failed && host.messages.size() == messages && migrated.storage().at("count") == "42",
            "failed migration caused side effects or changed the active runtime");
      };
      for (const auto& body : {"error('no migration handler')",
               "extensions.migrate(function() extensions.storage_set('count','broken'); error('rollback') end)",
               "extensions.migrate(function() while true do end end)",
               "extensions.migrate(function() for i=1,33 do extensions.storage_get('x') end end)",
               "extensions.migrate(function() local s='x'; for i=1,13 do s=s..s end; extensions.storage_set('large',s) end)",
               "extensions.migrate(function() extensions.reply('not allowed') end)",
               "extensions.migrate(function() extensions.broadcast('not allowed') end)",
               "extensions.migrate(function() extensions.after(50,function() end) end)",
               "extensions.migrate(function() extensions.log('not allowed') end)",
               "extensions.migrate(function() extensions.command('new',{},function() end) end)",
               "extensions.migrate(function() extensions.on('lobby_join',function() end) end)",
               "extensions.migrate(function() end); extensions.migrate(function() end)"}) {
        auto invalid = definition;
        invalid.source = body;
        migration_rejected(std::move(invalid));
      }
      auto missing = definition;
      missing.source = "extensions.command('no-upgrade', {}, function() end)";
      migration_rejected(missing);
      missing.source = definition.source;
      missing.grants.erase("storage");
      migration_rejected(missing);
      auto downgrade = current;
      downgrade.target_storage_version = 0;
      migration_rejected(downgrade);
      auto oversized_version = definition;
      oversized_version.target_storage_version = 0x80000000;
      migration_rejected(oversized_version);
      auto unversioned = package("extensions.command('v0', {}, function() end)");
      unversioned.storage = {{"legacy", "untouched"}};
      Runtime legacy(host, unversioned);
      check(legacy.storage_version() == 0 && !legacy.storage_dirty() && legacy.storage() == unversioned.storage,
          "unversioned storage compatibility changed");
    }
    for (const auto& schema : {"true", "{bad=true}", "{[2]={name='x',type='word'}}",
             "{{name='x',type='float'}}", "{{name='x',type='word',typo=1}}",
             "{{name='x',type='word',optional=1}}", "{{name='x',type='word',max_bytes=0}}",
             "{{name='x',type='integer',min=3,max=2}}", "{{name='x',type='integer',min=1.5}}",
             "{{name='x',type='word',min=1}}", "{{name='x',type='word'},{name='x',type='word'}}",
             "{{name='x',type='client_id',max_bytes=21}}",
             "{{name='x',type='text'},{name='y',type='word'}}",
             "{{name='x',type='word',optional=true},{name='y',type='word'}}"}) {
      rejects(std::string("extensions.command('bad', {arguments=") + schema + "}, function() end)");
    }

    rejects("extensions.command('debug', {}, function() end)");
    rejects("extensions.command('twice', {}, function() end); extensions.command('twice', {}, function() end)");
    rejects("extensions.reply('initialization side effect')");
    rejects("while true do end");
    rejects("local s = 'x'; for i=1,24 do s = s .. s end");
    rejects("extensions.on('scene_ready', function() end)");
    rejects("extensions.command('bad', {permission='made-up'}, function() end)");
    rejects(std::string("\x1bLua", 4));

    Runtime runaway(host, package("extensions.command('loop', {cooldown_ms=0}, function() while true do end end)"));
    for (int z = 0; z < 3; z++) {
      runaway.dispatch("loop", "", context);
    }
    check(!runaway.enabled(), "runaway package not disabled");
    check(runaway.diagnostics().callbacks == 3 && runaway.diagnostics().callback_errors == 3 &&
        runaway.diagnostics().last_error.find("budget exceeded") != std::string::npos,
        "fault diagnostics did not retain callback failures");
    {
      Runtime measured(host, package(R"(
        extensions.command('measured', {cooldown_ms=0, help='Store a short value', arguments={}}, function()
          extensions.storage_set('key', 'value')
        end)
        extensions.command('denied', {permission='root'}, function() end)
        extensions.command('broken', {cooldown_ms=0}, function() error('diagnostic failure') end)
        extensions.on('lobby_join', function() extensions.log('observed') end)
      )"));
      auto stats = measured.diagnostics();
      check(stats.callbacks == 0 && stats.callback_errors == 0 && stats.last_error.empty() && stats.storage_bytes == 0,
          "new runtime diagnostics were not clean");
      check(stats.last_callback_us == 0 && stats.peak_callback_us == 0 && stats.total_callback_us == 0 && stats.over_budget_callbacks == 0,
          "initialization was counted as timed callback work");
      check(stats.peak_memory_bytes >= measured.memory_bytes() && stats.peak_memory_bytes <= Runtime::MAX_MEMORY_BYTES,
          "Lua allocation peak was not bounded");
      auto details = measured.command_details();
      check(details.size() == 3 && details[2].name == "measured" && details[2].permission == "everyone" &&
          details[2].cooldown_ms == 0 && details[2].usage == "Usage: measured" && details[2].help == "Store a short value",
          "command diagnostic metadata was lost");
      check(details[0].usage == "Usage: broken [raw arguments]", "legacy command metadata was changed");
      host.admin = false;
      measured.dispatch("denied", "", context);
      measured.dispatch("measured", "invalid extra token", context);
      check(measured.diagnostics().callbacks == 0 && measured.diagnostics().total_callback_us == 0,
          "denied/invalid input counted as timed Lua callback attempts");
      host.admin = true;
      measured.dispatch("measured", "", context);
      measured.emit("lobby_join", context);
      measured.dispatch("broken", "", context);
      stats = measured.diagnostics();
      check(stats.callbacks == 3 && stats.callback_errors == 1 && stats.storage_bytes == 8 &&
          stats.last_error.find("diagnostic failure") != std::string::npos && measured.enabled(), "runtime health counters were wrong");
      check(stats.peak_memory_bytes >= measured.memory_bytes() && measured.grants().count("storage"), "runtime resource/grant details failed");
      check(stats.total_callback_us >= stats.peak_callback_us && stats.peak_callback_us >= stats.last_callback_us,
          "callback timing aggregates were inconsistent");
      auto earlier = stats;
      measured.dispatch("measured", "", context);
      check(earlier.callbacks == 3 && measured.diagnostics().callbacks == 4 && measured.diagnostics().callback_errors == 1,
          "diagnostics were not snapshots or successful calls erased failures");
      Runtime replacement(host, package("extensions.command('fresh', {}, function() end)"));
      check(replacement.diagnostics().callbacks == 0 && replacement.diagnostics().last_error.empty() &&
              replacement.diagnostics().total_callback_us == 0 && replacement.diagnostics().over_budget_callbacks == 0,
          "replacement inherited old health/timing counters");
    }
    {
      TestHost timing_host;
      Runtime timing(timing_host, package(R"(
        extensions.command('slow',{cooldown_ms=0},function() extensions.reply('slow native work') end)
        extensions.command('failure',{cooldown_ms=0},function() error('timed failure') end)
        extensions.command('oom',{cooldown_ms=0},function()
          local large='x'; while true do large=large..large end
        end)
        extensions.command('schedule',{cooldown_ms=0},function()
          extensions.after(50,function() extensions.reply('due timer') end)
        end)
        extensions.on('lobby_join',function() extensions.log('event') end)
        extensions.on('package_load',function() extensions.log('load') end)
        extensions.on('package_unload',function() extensions.log('unload') end)
      )"));
      timing_host.slow_reply = true;
      timing.dispatch("slow", "", context);
      timing_host.slow_reply = false;
      const auto slow = timing.diagnostics();
      check(slow.callbacks == 1 && slow.callback_errors == 0 && slow.over_budget_callbacks == 1 &&
              slow.last_callback_us >= Runtime::CALLBACK_TIME_BUDGET_MS * 1000 &&
              slow.total_callback_us == slow.last_callback_us && slow.peak_callback_us == slow.last_callback_us && timing.enabled(),
          "native work was not timed or measurement changed fault semantics");
      timing.dispatch("failure", "", context);
      auto failed = timing.diagnostics();
      check(failed.callbacks == 2 && failed.callback_errors == 1 && failed.total_callback_us >= slow.total_callback_us &&
              failed.peak_callback_us >= slow.peak_callback_us && failed.total_callback_us >= failed.peak_callback_us,
          "failed callback was absent from timing aggregates");
      timing.dispatch("oom", "", context);
      auto exhausted = timing.diagnostics();
      check(exhausted.callbacks == 3 && exhausted.callback_errors == 2 && exhausted.total_callback_us >= failed.total_callback_us &&
              exhausted.peak_callback_us >= exhausted.last_callback_us && timing.memory_bytes() <= Runtime::MAX_MEMORY_BYTES &&
              exhausted.last_error.find("memory") != std::string::npos,
          "memory exhaustion lost callback timing or escaped containment");
      timing.emit("lobby_join", context);
      timing.activate("startup");
      check(timing.diagnostics().callbacks == 5 && timing.diagnostics().callback_errors == 2,
          "normal event/load callbacks were not measured after failures");
      timing.dispatch("schedule", "", context);
      const auto queued = timing.diagnostics();
      check(timing.timer_count() == 1 && timing.diagnostics().total_callback_us == queued.total_callback_us,
          "inspection changed queued timing state");
      timing_host.live = false;
      std::this_thread::sleep_for(std::chrono::milliseconds(55));
      timing.tick();
      auto skipped = timing.diagnostics();
      check(skipped.callbacks == queued.callbacks && skipped.total_callback_us == queued.total_callback_us &&
              skipped.last_callback_us == queued.last_callback_us && skipped.over_budget_callbacks == queued.over_budget_callbacks,
          "unauthorized due timer was measured as a callback");
      timing_host.live = true;
      timing.dispatch("schedule", "", context);
      auto scheduled = timing.diagnostics();
      std::this_thread::sleep_for(std::chrono::milliseconds(55));
      timing.tick();
      check(timing.diagnostics().callbacks == scheduled.callbacks + 1 && timing_host.messages.back() == "due timer",
          "due timer was not measured on execution");
      timing.deactivate("disable");
      const auto retired = timing.diagnostics();
      check(retired.callbacks == scheduled.callbacks + 2 && retired.total_callback_us >= scheduled.total_callback_us &&
              retired.callback_errors == 2 && retired.over_budget_callbacks >= 1 &&
              slow.callbacks == 1 && slow.total_callback_us == slow.last_callback_us,
          "unload timing, independent snapshots or cumulative counters failed");
      auto migration = package("extensions.migrate(function() extensions.storage_set('migrated','yes') end)");
      migration.target_storage_version = 1;
      Runtime migrated(timing_host, migration);
      check(migrated.storage().at("migrated") == "yes" && migrated.diagnostics().callbacks == 0 &&
              migrated.diagnostics().total_callback_us == 0 && migrated.diagnostics().over_budget_callbacks == 0,
          "staged migration was counted as timed callback work");
    }
    rejects("extensions.command('help', {help=123}, function() end)");
    rejects("extensions.command('help', {help='line\\nbreak'}, function() end)");
    rejects("extensions.command('help', {help='escape\\27'}, function() end)");
    rejects("local s='x'; for i=1,8 do s=s..s end; extensions.command('help', {help=s}, function() end)");

    auto restricted = package("extensions.command('deny', {cooldown_ms=0}, function() extensions.broadcast('not allowed') end)");
    restricted.grants.erase("chat.broadcast");
    Runtime denied(host, restricted);
    before = host.messages.size();
    denied.dispatch("deny", "", context);
    check(host.messages.size() == before + 1 && host.messages.back() != "not allowed", "grant bypass");

    Runtime budget(host, package(R"(
      extensions.command('budget', {cooldown_ms=0}, function()
        for i=1,40 do extensions.reply('x') end
      end)
      extensions.command('quota', {cooldown_ms=0}, function()
        for i=1,32 do extensions.after(50, function() end) end
      end)
      extensions.command('overflow', {cooldown_ms=0}, function()
        local timer, message = extensions.after(50, function() end)
        assert(timer == nil and message == 'timer quota exceeded')
      end)
    )"));
    budget.dispatch("budget", "", context);
    check(!host.logs.empty(), "API budget failure not logged");
    budget.dispatch("quota", "", context);
    check(budget.timer_count() == Runtime::MAX_TIMERS, "timer quota setup failed");
    budget.dispatch("overflow", "", context);
    check(budget.timer_count() == Runtime::MAX_TIMERS, "timer quota bypass");
    check(budget.enabled(), "normal timer capacity exhaustion faulted the package");
    {
      TestHost repeating_host;
      Runtime repeating(repeating_host, package(R"(
        local recurring
        local count=0
        extensions.command('start',{cooldown_ms=0},function()
          recurring=extensions.every(50,function()
            count=count+1
            extensions.reply('repeat-'..count)
            if count==2 then assert(extensions.cancel(recurring)) end
          end)
          assert(recurring)
          extensions.after(50,function() extensions.reply('one-shot') end)
        end)
      )"));
      repeating.dispatch("start", "", context);
      check(repeating.timer_count() == 2, "repeating timer was not counted against shared quota");
      std::this_thread::sleep_for(std::chrono::milliseconds(140));
      repeating.tick();
      check(repeating_host.messages == std::vector<std::string>{"repeat-1"} && repeating.timer_count() == 2,
          "first repeating callback or retained timer slot failed");
      repeating.tick();
      check(repeating_host.messages == std::vector<std::string>{"repeat-1", "one-shot"} && repeating.timer_count() == 1,
          "recurring timer starved an older due one-shot");
      std::this_thread::sleep_for(std::chrono::milliseconds(70));
      repeating.tick();
      check(repeating_host.messages.back() == "repeat-2" && repeating.timer_count() == 0 && repeating.diagnostics().callback_errors == 0,
          "self-cancelled repeating timer was resurrected or faulted");

      auto before = repeating_host.messages.size();
      Runtime cancellation(repeating_host, package(R"(
        extensions.command('start',{cooldown_ms=0},function()
          local id
          id=extensions.every(50,function()
            assert(extensions.cancel(id))
            extensions.after(50,function() extensions.reply('replacement') end)
          end)
        end)
      )"));
      cancellation.dispatch("start", "", context);
      std::this_thread::sleep_for(std::chrono::milliseconds(70));
      cancellation.tick();
      check(cancellation.timer_count() == 1, "self-cancel lost a newly registered timer");
      std::this_thread::sleep_for(std::chrono::milliseconds(70));
      cancellation.tick();
      check(cancellation.timer_count() == 0 && repeating_host.messages.size() == before + 1 &&
              repeating_host.messages.back() == "replacement" && cancellation.diagnostics().callback_errors == 0,
          "registry reference reuse damaged replacement timer");

      Runtime failing(repeating_host, package("extensions.command('start',{cooldown_ms=0},function() extensions.every(50,function() error('recurring failure') end) end)"));
      failing.dispatch("start", "", context);
      std::this_thread::sleep_for(std::chrono::milliseconds(70));
      failing.tick();
      check(failing.timer_count() == 0 && failing.enabled() && failing.diagnostics().callback_errors == 1,
          "failed repeating timer remained armed or changed package fault threshold");
      for (size_t z = 0; z < 2; z++) {
        failing.dispatch("start", "", context);
        std::this_thread::sleep_for(std::chrono::milliseconds(70));
        failing.tick();
      }
      check(!failing.enabled() && failing.timer_count() == 0 && failing.diagnostics().callback_errors == 3,
          "fault cleanup damaged an executing recurring timer or retained its reference");
      Runtime revoke(repeating_host, package("extensions.command('start',{permission='announce',cooldown_ms=0},function() extensions.every(50,function() extensions.reply('forbidden') end) end)"));
      repeating_host.admin = true;
      revoke.dispatch("start", "", context);
      repeating_host.admin = false;
      before = repeating_host.messages.size();
      std::this_thread::sleep_for(std::chrono::milliseconds(70));
      revoke.tick();
      check(revoke.timer_count() == 0 && repeating_host.messages.size() == before && revoke.diagnostics().callbacks == 1,
          "repeating timer bypassed revoked command permission");
      Runtime leave(repeating_host, package("extensions.command('start',{cooldown_ms=0},function() extensions.every(50,function() extensions.reply('departed') end) end)"));
      leave.dispatch("start", "", context);
      leave.cancel_client(context.client_id);
      check(leave.timer_count() == 0, "membership cleanup left a repeating timer");
      Runtime quota(repeating_host, package(R"(
        extensions.command('full',{cooldown_ms=0},function()
          for i=1,16 do
            assert(extensions.every(50,function()
              local id,reason=extensions.after(50,function() end)
              assert(id==nil and reason=='timer quota exceeded')
            end))
          end
        end)
        extensions.command('checkfull',{cooldown_ms=0},function()
          local id,reason=extensions.every(50,function() end)
          assert(id==nil and reason=='timer quota exceeded')
          id,reason=extensions.after(50,function() end)
          assert(id==nil and reason=='timer quota exceeded')
        end)
      )"));
      quota.dispatch("full", "", context);
      quota.dispatch("full", "", context);
      quota.dispatch("checkfull", "", context);
      check(quota.timer_count() == Runtime::MAX_TIMERS && quota.enabled() && quota.diagnostics().callback_errors == 0,
          "repeating timer quota failure was not bounded and recoverable");
      std::this_thread::sleep_for(std::chrono::milliseconds(70));
      quota.tick();
      check(quota.timer_count() == Runtime::MAX_TIMERS && quota.diagnostics().callback_errors == 0,
          "executing repeating timer released its reserved quota slot");
      Runtime maximum(repeating_host, package("extensions.command('start',{cooldown_ms=0},function() assert(extensions.every(3600000,function() end)) end)"));
      maximum.dispatch("start", "", context);
      check(maximum.timer_count() == 1 && maximum.diagnostics().callback_errors == 0, "maximum repeating interval was rejected");
      auto no_timer_grant = package("extensions.command('start',{cooldown_ms=0},function() extensions.every(50,function() end) end)");
      no_timer_grant.grants.erase("timers");
      Runtime denied(repeating_host, no_timer_grant);
      denied.dispatch("start", "", context);
      check(denied.timer_count() == 0 && denied.diagnostics().callback_errors == 1, "repeating timer bypassed timers grant");
      rejects("extensions.every(50,function() end)");
      for (const std::string input : {"49", "3600001", "-1", "1.5", "true", "'50'"}) {
        Runtime invalid(repeating_host, package("extensions.command('bad',{cooldown_ms=0},function() extensions.every(" + input + ",function() end) end)"));
        invalid.dispatch("bad", "", context);
        check(invalid.timer_count() == 0 && invalid.diagnostics().callback_errors == 1, "invalid repeating interval was accepted");
      }
    }

    Runtime cooldown(host, package("extensions.command('slow', {cooldown_ms=10000}, function() extensions.reply('ran') end)"));
    cooldown.dispatch("slow", "", context);
    cooldown.dispatch("slow", "", context);
    check(host.messages.back() != "ran", "cooldown bypass");

    auto scene_package = package(R"(
      extensions.command('scene-test', {cooldown_ms=0}, function(ctx)
        local caps = extensions.scene_capabilities()
        assert(caps.arbitrary_transforms == false and caps.experimental_models == false)
        if not caps.native_ep3_media then
          assert(caps.max_media_compressed_bytes == 0 and caps.max_media_decompressed_bytes == 0)
        elseif caps.extended_ep3_media then
          assert(caps.max_media_compressed_bytes == 0x7BF0 and caps.max_media_decompressed_bytes == 16777216)
        else
          assert(caps.max_media_compressed_bytes == 0x3800 and caps.max_media_decompressed_bytes == 0x36FFF)
        end
        local ok, reason = extensions.scene_spawn('welcome', 'banner', 'teleporter-west')
        if caps.native_ep3_media then
          assert(ok and reason == nil)
          ctx.client_id = 'fake-owner'
          assert(extensions.scene_remove('welcome'))
        else
          assert(not ok and reason == 'unsupported recipient')
        end
      end)
      extensions.command('event-test', {cooldown_ms=0}, function()
        local ok, reason = extensions.scene_event(1)
        if not ok then assert(reason == 'permission denied') end
      end)
    )");
    scene_package.grants.insert("scene.media");
    scene_package.grants.insert("scene.event");
    Runtime scene_runtime(host, scene_package);
    host.media_available = false;
    scene_runtime.dispatch("scene-test", "", context);
    check(host.scene_calls == 0 && scene_runtime.enabled(), "unsupported scene target caused actions or faulted script");
    host.media_available = true;
    scene_runtime.dispatch("scene-test", "", context);
    check(host.scene_owner == "test" && host.scene_calls == 2, "scene ownership escaped the package ID");
    host.extended_media = true;
    scene_runtime.dispatch("scene-test", "", context);
    check(scene_runtime.enabled() && host.scene_calls == 4, "extended media capabilities faulted script");
    host.admin = false;
    scene_runtime.dispatch("event-test", "", context);
    check(scene_runtime.enabled(), "denied event permission faulted script");
    host.admin = true;
    scene_runtime.dispatch("event-test", "", context);
    auto denied_scene_package = package("extensions.command('denied-scene', {cooldown_ms=0}, function() extensions.scene_spawn('x','y','z') end)");
    Runtime denied_scene(host, denied_scene_package);
    size_t previous_scene_calls = host.scene_calls;
    denied_scene.dispatch("denied-scene", "", context);
    check(host.scene_calls == previous_scene_calls, "missing scene grant reached the host");
    rejects("extensions.scene_capabilities()");

    auto support_package = package(R"(
      extensions.command('support',{cooldown_ms=0},function(ctx)
        local first = extensions.scene_asset_support('banner')
        assert(first.supported and first.reason==nil and first.variant=='fallback')
        assert(first.path==nil and first.data==nil and first.client_id==nil)
        first.supported,first.variant=false,'forged'
        ctx.client_id,ctx.lobby_id='1','1'
        local fresh = extensions.scene_asset_support('banner')
        assert(fresh.supported and fresh.variant=='fallback')
      end)
      extensions.command('unsupported',{cooldown_ms=0},function()
        local info=extensions.scene_asset_support('banner')
        assert(not info.supported and info.reason=='scenes_disabled' and info.variant==nil)
      end)
      extensions.command('budget',{cooldown_ms=0},function()
        for i=1,33 do extensions.scene_asset_support('banner') end
      end)
      extensions.on('lobby_leave',function(ctx)
        ctx.can_act=true
        local info=extensions.scene_asset_support('banner')
        assert(not info.supported and info.reason=='context_unavailable' and info.variant==nil)
      end)
    )");
    support_package.grants.emplace("scene.media");
    Runtime support_runtime(host, support_package);
    auto query_start = host.asset_queries;
    auto action_start = host.scene_calls;
    support_runtime.dispatch("support", "", context);
    check(host.asset_queries == query_start + 2 && host.queried_owner == "test" && host.queried_asset == "banner" &&
            host.scene_calls == action_start && support_runtime.diagnostics().callback_errors == 0,
        "asset support query lost owner scoping, copies or read-only behavior");
    host.live = false;
    support_runtime.emit("lobby_leave", context);
    check(host.asset_queries == query_start + 2 && support_runtime.diagnostics().callback_errors == 0,
        "stale query reached adapter or trusted forged metadata");
    host.live = true;
    host.asset_support = {false, "scenes_disabled", nullptr};
    support_runtime.dispatch("unsupported", "", context);
    check(support_runtime.diagnostics().callback_errors == 0, "unavailable support query faulted script");
    host.asset_support = {true, nullptr, "fallback"};
    support_runtime.dispatch("budget", "", context);
    check(support_runtime.diagnostics().callback_errors == 1, "asset queries bypassed API budget");
    auto ungranted_support = package("extensions.command('read',{cooldown_ms=0},function() extensions.scene_asset_support('a') end)");
    Runtime denied_support(host, ungranted_support);
    query_start = host.asset_queries;
    denied_support.dispatch("read", "", context);
    check(host.asset_queries == query_start && denied_support.diagnostics().callback_errors == 1, "asset query bypassed scene.media grant");
    for (const std::string args : {"", "nil", "true", "1", "{}", "'a','b'"}) {
      auto bad = package("extensions.command('bad',{cooldown_ms=0},function() extensions.scene_asset_support(" + args + ") end)");
      bad.grants.emplace("scene.media");
      Runtime invalid(host, bad);
      invalid.dispatch("bad", "", context);
      check(host.asset_queries == query_start && invalid.diagnostics().callback_errors == 1, "malformed asset query reached adapter");
    }
    rejects("extensions.scene_asset_support('banner')");
    auto reject_support_initialization = [&](Package candidate) {
      bool failed = false;
      try { Runtime invalid(host, candidate); } catch (const std::exception&) { failed = true; }
      check(failed && host.asset_queries == query_start, "initialization/migration reached live asset query");
    };
    auto support_init = package("extensions.scene_asset_support('banner')");
    support_init.grants.emplace("scene.media");
    reject_support_initialization(support_init);
    support_init.source = "extensions.migrate(function() extensions.scene_asset_support('banner') end)";
    support_init.target_storage_version = 1;
    reject_support_initialization(support_init);

    host.media_available = true;
    auto temporary_package = package(R"(
      extensions.command('timed', {cooldown_ms=0}, function()
        assert(extensions.scene_spawn('key','asset','teleporter-west',50))
      end)
      extensions.command('maximum', {cooldown_ms=0}, function()
        assert(extensions.scene_spawn('key','asset','teleporter-west',3600000))
      end)
      extensions.command('persistent', {cooldown_ms=0}, function()
        assert(extensions.scene_spawn('key','asset','teleporter-west',nil))
        assert(extensions.scene_spawn('key','asset','teleporter-west',0))
      end)
    )");
    temporary_package.grants.emplace("scene.media");
    temporary_package.grants.erase("timers");
    Runtime temporary_runtime(host, temporary_package);
    temporary_runtime.dispatch("timed", "", context);
    check(host.scene_lifetime_ms == 50, "Lua lifetime was not forwarded exactly");
    temporary_runtime.dispatch("maximum", "", context);
    check(host.scene_lifetime_ms == 3600000, "maximum Lua lifetime failed");
    temporary_runtime.dispatch("persistent", "", context);
    check(host.scene_lifetime_ms == 0 && temporary_runtime.diagnostics().callback_errors == 0,
        "nil/zero persistent lifetime failed");
    check(temporary_runtime.timer_count() == 0, "scene lifetime consumed a Lua timer");
    for (const std::string value : {"-1", "1", "49", "3600001", "1.5", "true", "'50'", "{}"}) {
      auto bad = package("extensions.command('bad',{cooldown_ms=0},function() extensions.scene_spawn('k','a','s'," + value + ") end)");
      bad.grants.emplace("scene.media");
      Runtime invalid(host, bad);
      auto calls = host.scene_calls;
      invalid.dispatch("bad", "", context);
      check(invalid.diagnostics().callback_errors == 1 && host.scene_calls == calls, "invalid Lua lifetime reached host");
    }

    Runtime safety(host, package(R"(
      extensions.command('whisper-check', {cooldown_ms=0}, function()
        assert(extensions.whisper('not-an-id', 'x') == false)
        assert(extensions.whisper('123', 'x') == false)
      end)
      extensions.command('oom', {cooldown_ms=0}, function()
        local s='x'; for i=1,24 do s=s..s end
      end)
      extensions.command('big-store', {cooldown_ms=0}, function()
        local s='x'; for i=1,12 do s=s..s end
        for i=1,20 do extensions.storage_set(tostring(i), s) end
      end)
      extensions.command('forged', {cooldown_ms=0}, function(ctx)
        ctx.client_id = '999'; ctx.lobby_id = '999'
        extensions.reply('original-context')
      end)
    )"));
    host.target_present = false;
    for (int z = 0; z < 3; z++) {
      safety.dispatch("whisper-check", "", context);
    }
    check(safety.enabled(), "absent/invalid whisper target faulted the extension");
    safety.dispatch("oom", "", context);
    check(safety.memory_bytes() <= Runtime::MAX_MEMORY_BYTES && safety.enabled(), "allocation limit failed");
    safety.dispatch("big-store", "", context);
    size_t stored = 0;
    for (const auto& [key, value] : safety.storage()) {
      stored += key.size() + value.size();
    }
    check(stored <= Runtime::MAX_STORAGE_BYTES, "storage quota bypass");
    safety.dispatch("forged", "", context);
    check(host.messages.back() == "original-context", "out-of-memory error corrupted later callbacks");
    check(host.last_reply_client_id == context.client_id, "script forged the host context");

    Runtime faulted(host, package(R"(
      extensions.command('schedule', {cooldown_ms=0}, function()
        extensions.after(3600000, function() end)
      end)
      extensions.command('fail', {cooldown_ms=0}, function() error('failure') end)
    )"));
    faulted.dispatch("schedule", "", context);
    for (int z = 0; z < 3; z++) {
      faulted.dispatch("fail", "", context);
    }
    check(!faulted.enabled() && faulted.timer_count() == 0, "faulted package retained pending timers");

    Runtime privilege_timer(host, package(R"(
      extensions.command('privileged-timer', {permission='announce', cooldown_ms=0}, function()
        extensions.after(50, function() extensions.broadcast('privileged-later') end)
      end)
    )"));
    host.admin = true;
    privilege_timer.dispatch("privileged-timer", "", context);
    host.admin = false;
    before = host.messages.size();
    std::this_thread::sleep_for(std::chrono::milliseconds(70));
    privilege_timer.tick();
    check(host.messages.size() == before, "timer bypassed revoked permission");

    // Loading a bad replacement has no externally visible side effects and the
    // previously loaded instance remains callable.
    rejects("extensions.command('replacement', {}, function() end); error('bad replacement')");
    runtime.dispatch("hello", "still-live", context);
    check(host.messages.back() == "Alice:still-live", "failed replacement damaged an independent live state");

    if (argc > 1) {
      std::ifstream file(argv[1]);
      check(file.good(), "cannot read shipped example script");
      std::string source((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
      auto example_package = package(source);
      example_package.target_storage_version = 1;
      example_package.settings = resolve_settings({{"shout_cooldown_ms", {"integer", false, int64_t(10000)}}}, {});
      Runtime example(host, example_package);
      check(example.storage_version() == 1 && example.storage().at("total-visits") == "0", "shipped storage migration failed");
      check(example.command_names().size() == 6, "example command registration mismatch");
      example.dispatch("shout", "example", context);
      check(host.messages.back() == "Alice: example", "shipped shout example failed");
      example.dispatch("whisper", "999 absent", context);
      check(host.messages.back() != "Whisper sent.", "example incorrectly confirmed an absent recipient");
      check(example.enabled(), "example failed on normal bad user input");
      example.dispatch("repeatremind", "1 1", context);
      check(example.timer_count() == 1, "shipped repeating reminder failed to schedule");
      std::this_thread::sleep_for(std::chrono::milliseconds(1050));
      example.tick();
      check(example.timer_count() == 0 && host.messages.back() == "Repeating reminder 1/1." &&
              example.diagnostics().callback_errors == 0, "shipped reminder did not stop after its requested count");
    }
    if (argc > 2) {
      std::ifstream file(argv[2]);
      check(file.good(), "cannot read shipped lobby decoration script");
      std::string source((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
      TestHost scene_host;
      scene_host.admin = true;
      auto definition = package(source);
      definition.grants.insert("scene.media");
      definition.grants.insert("scene.event");
      Runtime example(scene_host, definition);
      example.emit("lobby_join", context);
      check(scene_host.scene_calls == 0, "example ignored unsupported-client capabilities");
      scene_host.media_available = true;
      example.emit("lobby_join", context);
      check(scene_host.scene_calls == 1, "shipped decoration join handler failed");
      example.dispatch("decor", "remove", context);
      check(scene_host.scene_calls == 2, "shipped decoration removal failed");
      example.dispatch("decortimed", "10", context);
      check(scene_host.scene_calls == 3 && scene_host.scene_lifetime_ms == 10000,
          "shipped temporary decoration example failed");
      auto asset_reads = scene_host.asset_queries;
      example.dispatch("decorcheck", "checker", context);
      check(scene_host.asset_queries == asset_reads + 1 && scene_host.queried_asset == "checker" &&
              scene_host.scene_calls == 3 && scene_host.messages.back().find("fallback") != std::string::npos,
          "shipped asset support example mutated scenes or omitted selected variant");
      example.dispatch("season", "1", context);
      example.dispatch("season", "release", context);
      check(example.enabled(), "shipped scene example faulted");
      auto calls_before = scene_host.scene_calls;
      scene_host.live = false;
      for (int z = 0; z < 3; z++) {
        example.emit("lobby_join", context);
      }
      check(example.enabled() && scene_host.scene_calls == calls_before,
          "unavailable event context faulted the capability query or changed its scene");
      scene_host.live = true;
      example.emit("lobby_join", context);
      check(scene_host.scene_calls == calls_before + 1, "scene example did not recover after unavailable context");
    }
    std::cout << "Extension runtime tests passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
