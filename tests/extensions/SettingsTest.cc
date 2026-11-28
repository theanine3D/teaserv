#include "Runtime.hh"

#include <iostream>
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
  check(rejected, "invalid settings were accepted");
}
struct TestHost : Host {
  bool authorized(const Context&) const override { return true; }
  void reply(const Context&, const std::string&) override {}
  void broadcast(const Context&, const std::string&) override {}
  bool whisper(const Context&, uint64_t, const std::string&) override { return false; }
  void log(const std::string&, const std::string&) override {}
  bool command_reserved(const std::string&) const override { return false; }
};

int main() {
  try {
    SettingsSchema schema{
        {"enabled", {"boolean", false, true}},
        {"count", {"integer", true, {}, -10, 10}},
        {"label", {"string", false, std::string("default"), INT64_MIN, INT64_MAX, 8}},
        {"optional", {"string"}}};
    auto settings = resolve_settings(schema, {{"count", int64_t(0)}, {"enabled", false}, {"label", std::string("")}});
    check(!std::get<bool>(*settings.at("enabled").value), "false override lost");
    check(std::get<int64_t>(*settings.at("count").value) == 0, "zero override lost");
    check(std::get<std::string>(*settings.at("label").value).empty(), "empty override lost");
    check(!settings.at("optional").value, "unset setting acquired a value");
    auto defaults = resolve_settings(schema, {{"count", int64_t(-10)}});
    check(std::get<bool>(*defaults.at("enabled").value) &&
            std::get<std::string>(*defaults.at("label").value) == "default",
        "defaults lost");
    resolve_settings(schema, {{"count", int64_t(10)}});
    rejects([&] { resolve_settings(schema, {}); });
    rejects([&] { resolve_settings(schema, {{"count", int64_t(11)}}); });
    rejects([&] { resolve_settings(schema, {{"count", int64_t(-11)}}); });
    rejects([&] { resolve_settings(schema, {{"count", true}}); });
    rejects([&] { resolve_settings(schema, {{"count", std::string("0")}}); });
    rejects([&] { resolve_settings(schema, {{"count", int64_t(0)}, {"unknown", false}}); });
    rejects([&] { resolve_settings(schema, {{"count", int64_t(0)}, {"label", std::string(9, 'x')}}); });
    rejects([&] { resolve_settings(schema, {{"count", int64_t(0)}, {"label", std::string("a\0b", 3)}}); });
    auto invalid = schema;
    invalid.at("count").default_value = int64_t(20);
    rejects([&] { resolve_settings(invalid, {{"count", int64_t(0)}}); });
    invalid = schema;
    invalid.at("count").min = 11;
    rejects([&] { resolve_settings(invalid, {}); });
    rejects([] { resolve_settings({{"Bad Name", {"boolean"}}}, {}); });
    rejects([] { resolve_settings({}, {{std::string(65, 'x'), true}}); });
    rejects([] { resolve_settings({{"x", {"float"}}}, {}); });
    rejects([] { resolve_settings({{"x", {"string", false, {}, INT64_MIN, INT64_MAX, 0}}}, {}); });
    SettingsSchema large;
    for (size_t z = 0; z < 64; z++)
      large.emplace("s" + std::to_string(z), SettingDefinition{"string"});
    resolve_settings(large, {});
    large.emplace("too-many", SettingDefinition{"string"});
    rejects([&] { resolve_settings(large, {}); });
    large.erase("too-many");
    for (auto& [name, definition] : large) {
      (void)name;
      definition.default_value = std::string(1024, 'x');
    }
    rejects([&] { resolve_settings(large, {}); });
    SettingsSchema boundary;
    for (char ch = 'a'; ch <= 'p'; ch++) {
      boundary.emplace(std::string(1, ch), SettingDefinition{"string", false, std::string(1023, 'x')});
    }
    resolve_settings(boundary, {}); // Exactly 16 KiB including names.
    boundary.at("a").default_value = std::string(1024, 'x');
    rejects([&] { resolve_settings(boundary, {}); });
    auto extremes = resolve_settings({{"min", {"integer", false, INT64_MIN}}, {"max", {"integer", false, INT64_MAX}}}, {});

    // Choices (Teaserv Admin's Extensions page shows them as a list): defaults and overrides must be one of them
    SettingsSchema chosen{
        {"period", {"string", false, std::string("week"), INT64_MIN, INT64_MAX, 5,
                       {std::string("week"), std::string("month")}}},
        {"weekday", {"integer", false, int64_t(6), 0, 7, 1024, {int64_t(0), int64_t(1), int64_t(6), int64_t(7)}}}};
    auto picked = resolve_settings(chosen, {{"period", std::string("month")}});
    check(std::get<std::string>(*picked.at("period").value) == "month", "choice override lost");
    check(std::get<int64_t>(*picked.at("weekday").value) == 6, "choice default lost");
    rejects([&] { resolve_settings(chosen, {{"period", std::string("day")}}); });
    rejects([&] { resolve_settings(chosen, {{"weekday", int64_t(3)}}); }); // in bounds, not a choice
    auto bad_choices = chosen;
    bad_choices.at("period").default_value = std::string("year");
    rejects([&] { resolve_settings(bad_choices, {{"period", std::string("week")}}); }); // default must be a choice
    bad_choices = chosen;
    bad_choices.at("weekday").choices.emplace_back(int64_t(8)); // out of bounds
    rejects([&] { resolve_settings(bad_choices, {}); });
    bad_choices = chosen;
    bad_choices.at("weekday").choices.emplace_back(int64_t(1)); // duplicate
    rejects([&] { resolve_settings(bad_choices, {}); });
    bad_choices = chosen;
    bad_choices.at("period").choices.emplace_back(int64_t(1)); // wrong type
    rejects([&] { resolve_settings(bad_choices, {}); });
    rejects([] { resolve_settings({{"flag", {"boolean", false, true, INT64_MIN, INT64_MAX, 1024, {true, false}}}}, {}); });
    SettingsSchema many{{"n", {"integer"}}};
    for (int64_t z = 0; z < 65; z++) many.at("n").choices.emplace_back(z);
    rejects([&] { resolve_settings(many, {}); });

    TestHost host;
    Package package{"settings-test", "1", R"(
      assert(extensions.setting('enabled') == false)
      assert(extensions.setting('count') == 0)
      assert(extensions.setting('label') == '')
      assert(extensions.setting('optional') == nil)
      local captured = extensions.setting('count')
      extensions.command('test', {cooldown_ms=0}, function()
        assert(extensions.setting('count') == captured)
        local copy = extensions.setting('label') .. 'changed'
        assert(copy == 'changed' and extensions.setting('label') == '')
      end)
    )",
        {"commands"}, {}};
    package.settings = settings;
    Runtime runtime(host, package);
    runtime.dispatch("test", "", {1, 1, "Alice"});
    check(runtime.diagnostics().callback_errors == 0, "settings changed during callback");
    auto other = package;
    other.settings.at("count").value = int64_t(5);
    other.source = "assert(extensions.setting('count') == 5)";
    Runtime isolated(host, other);
    runtime.dispatch("test", "", {1, 1, "Alice"});
    check(runtime.diagnostics().callback_errors == 0, "packages shared settings");
    other.source = "extensions.setting('undeclared')";
    rejects([&] { Runtime bad(host, other); });
    other.source = "for i=1,33 do extensions.setting('count') end";
    rejects([&] { Runtime bad(host, other); });
    other.source = R"(
      extensions.command('budget', {cooldown_ms=0}, function()
        for i=1,33 do extensions.setting('count') end
      end)
    )";
    Runtime budget(host, other);
    budget.dispatch("budget", "", {1, 1, "Alice"});
    check(budget.diagnostics().callback_errors == 1, "settings reads bypassed callback API budget");
    other.settings = extremes;
    other.source = "assert(extensions.setting('min') == math.mininteger and extensions.setting('max') == math.maxinteger)";
    Runtime exact(host, other);
    other.grants = {"storage"};
    other.target_storage_version = 1;
    other.source = R"(
      extensions.migrate(function()
        extensions.storage_set('limit', tostring(extensions.setting('max')))
      end)
    )";
    Runtime migrated(host, other);
    check(migrated.storage().at("limit") == "9223372036854775807", "migration could not read settings");
    other.settings.at("max").value = std::string("wrong");
    rejects([&] { Runtime bad(host, other); });
    std::cout << "Typed settings, bounds, quotas, isolation, Lua copies, budgets and migrations passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
