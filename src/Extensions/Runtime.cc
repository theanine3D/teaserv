#include "Runtime.hh"
#include "MessageText.hh"
#include "../CaelumExt.h"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <algorithm>
#include <charconv>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

static_assert(LUA_VERSION_NUM == 504, "Teaserv extensions require Lua 5.4");

namespace teaserv::extensions {
namespace {

// Do not call coercing Lua helpers with C++ objects alive: they can longjmp.
const char* string_arg(lua_State* L, int index, size_t max_size) {
  if (lua_type(L, index) != LUA_TSTRING) {
    throw std::runtime_error("expected a string");
  }
  size_t size = 0;
  const char* data = lua_tolstring(L, index, &size);
  if (size > max_size || std::memchr(data, 0, size)) {
    throw std::runtime_error("string is too long or contains a zero byte");
  }
  return data;
}

bool valid_name(const std::string& name) {
  return !name.empty() && name.size() <= 64 && std::all_of(name.begin(), name.end(), [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
  });
}

// Runs inside a protected call. Holds no C++ objects with destructors.
void push_info(lua_State* L, const InfoTable& info) {
  lua_createtable(L, 0, static_cast<int>(info.keys.size()));
  for (size_t z = 0; z < info.keys.size(); z++) {
    const auto& value = info.values[z].value;
    if (const auto* b = std::get_if<bool>(&value)) {
      lua_pushboolean(L, *b);
    } else if (const auto* i = std::get_if<int64_t>(&value)) {
      lua_pushinteger(L, *i);
    } else if (const auto* text = std::get_if<std::string>(&value)) {
      lua_pushlstring(L, text->data(), text->size());
    } else {
      const auto& list = std::get<std::vector<InfoTable>>(value);
      lua_createtable(L, static_cast<int>(list.size()), 0);
      for (size_t y = 0; y < list.size(); y++) {
        push_info(L, list[y]);
        lua_rawseti(L, -2, static_cast<lua_Integer>(y + 1));
      }
    }
    lua_setfield(L, -2, info.keys[z].c_str());
  }
}

void validate_info(const InfoTable& info, size_t depth, size_t& budget) {
  if (depth > 4 || info.keys.size() != info.values.size() || info.keys.size() > 32) {
    throw std::runtime_error("event metadata is too deep or too large");
  }
  for (size_t z = 0; z < info.keys.size(); z++) {
    if (info.keys[z].empty() || info.keys[z].size() > 32 || info.keys[z].find('\0') != std::string::npos || !budget--) {
      throw std::runtime_error("invalid event metadata");
    }
    if (const auto* text = std::get_if<std::string>(&info.values[z].value)) {
      if (text->size() > 256) throw std::runtime_error("event metadata text is too long");
    } else if (const auto* list = std::get_if<std::vector<InfoTable>>(&info.values[z].value)) {
      if (list->size() > 16) throw std::runtime_error("event metadata list is too long");
      for (const auto& item : *list) validate_info(item, depth + 1, budget);
    }
  }
}

// Reads an integer or a dense array of integers from a schedule field.
std::set<int> schedule_field(lua_State* L, int spec, const char* key) {
  std::set<int> result;
  lua_pushstring(L, key);
  lua_rawget(L, spec);
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
    return result;
  }
  auto add = [&](int index) {
    if (!lua_isinteger(L, index)) throw std::runtime_error("schedule fields must be integers or arrays of integers");
    lua_Integer value = lua_tointeger(L, index);
    if (value < -1000 || value > 1000) throw std::runtime_error("schedule field is out of range");
    result.emplace(static_cast<int>(value));
  };
  if (lua_type(L, -1) == LUA_TTABLE) {
    size_t count = lua_rawlen(L, -1);
    if (!count || count > 60) throw std::runtime_error("schedule arrays must have 1..60 entries");
    for (size_t z = 1; z <= count; z++) {
      lua_rawgeti(L, -1, static_cast<lua_Integer>(z));
      add(-1);
      lua_pop(L, 1);
    }
  } else {
    add(-1);
  }
  lua_pop(L, 1);
  return result;
}

bool valid_permission(const std::string& name) {
  return name == "everyone" || name == "announce" || name == "debug" || name == "moderator" || name == "admin" || name == "root" ||
      name == "change-event";
}

} // namespace

ClockTime Host::now() const {
  return clock_time(static_cast<int64_t>(std::time(nullptr)), 0);
}

Runtime::Runtime(Host& host, Package package) : host(host), package(std::move(package)) {
  if (!valid_name(this->package.id) || this->package.source.size() > MAX_SOURCE_BYTES) {
    throw std::runtime_error("invalid extension ID or oversized entry script");
  }
  this->validate_storage();
  validate_settings(this->package.settings);
  size_t data_bytes = 0;
  for (const auto& [name, text] : this->package.data_files) {
    data_bytes += text.size();
    if (!valid_data_file_name(name) || text.size() > MAX_DATA_FILE_BYTES || text.find('\0') != std::string::npos) {
      throw std::runtime_error("invalid package data file " + name);
    }
  }
  if (this->package.data_files.size() > MAX_DATA_FILES || data_bytes > MAX_DATA_BYTES) {
    throw std::runtime_error("too many or too large package data files");
  }
  if (this->package.storage_version > this->package.target_storage_version ||
      this->package.target_storage_version > MAX_STORAGE_VERSION) {
    throw std::runtime_error("extension storage version downgrade or invalid target version");
  }
  if (this->package.storage_version != this->package.target_storage_version) {
    this->require_grant("storage");
  }
  this->state = lua_newstate(allocate, this);
  if (!this->state) {
    throw std::runtime_error("cannot allocate Lua state");
  }
  *static_cast<Runtime**>(lua_getextraspace(this->state)) = this;
  this->reset_budget();
  lua_pushcfunction(this->state, initialize);
  if (lua_pcall(this->state, 0, 0, 0) != LUA_OK) {
    const char* error = lua_type(this->state, -1) == LUA_TSTRING ? lua_tostring(this->state, -1) : "non-string Lua error";
    std::string message(error);
    lua_close(this->state);
    this->state = nullptr;
    throw std::runtime_error("extension initialization failed: " + message);
  }
  if (this->package.storage_version != this->package.target_storage_version) {
    this->package.storage_version = this->package.target_storage_version;
    this->dirty = true;
  }
  this->staging = false;
}

Runtime::~Runtime() {
  if (this->state) {
    lua_close(this->state);
  }
}

const std::string& Runtime::id() const { return this->package.id; }
const std::string& Runtime::version() const { return this->package.version; }
bool Runtime::enabled() const { return this->active; }
size_t Runtime::memory_bytes() const { return this->allocated; }
size_t Runtime::timer_count() const { return this->timers.size(); }
size_t Runtime::schedule_count() const { return this->schedules.size(); }
const std::map<std::string, std::string>& Runtime::storage() const { return this->package.storage; }
bool Runtime::storage_dirty() const { return this->dirty; }
uint32_t Runtime::storage_version() const { return this->package.storage_version; }
void Runtime::storage_saved() { this->dirty = false; }

RuntimeDiagnostics Runtime::diagnostics() const {
  size_t bytes = 0;
  for (const auto& [key, value] : this->package.storage) bytes += key.size() + value.size();
  return {this->callbacks, this->errors, this->peak_allocated, bytes, this->last_error,
      this->last_callback_us, this->peak_callback_us, this->total_callback_us, this->over_budget_callbacks};
}

const std::set<std::string>& Runtime::grants() const { return this->package.grants; }
const Settings& Runtime::settings() const { return this->package.settings; }

std::vector<CommandDetails> Runtime::command_details() const {
  std::vector<CommandDetails> result;
  for (const auto& [name, command] : this->commands) {
    result.emplace_back(CommandDetails{name, command.permission, command.cooldown_ms,
        command.arguments ? command.arguments->usage(name) : "Usage: " + name + " [raw arguments]", command.help});
  }
  return result;
}

std::vector<std::string> Runtime::command_names() const {
  std::vector<std::string> result;
  for (const auto& [name, command] : this->commands) {
    (void)command;
    result.emplace_back(name);
  }
  return result;
}

bool Runtime::has_command(const std::string& name) const { return this->commands.count(name); }

bool Runtime::dispatch(const std::string& name, const std::string& args, Context context) {
  auto it = this->commands.find(name);
  if (it == this->commands.end()) {
    return false;
  }
  context.permission = it->second.permission;
  if (!this->host.authorized(context)) {
    this->host.reply(context, "You cannot use this extension command here or do not have permission.");
    return true;
  }
  if (!this->active) {
    this->host.reply(context, "This extension is disabled after repeated errors.");
    return true;
  }
  if (args.size() > 1024) {
    this->host.reply(context, "Extension command arguments are too long.");
    return true;
  }
  std::optional<ParsedCommandArguments> parsed;
  if (it->second.arguments) {
    parsed = it->second.arguments->parse(args);
    if (!parsed) {
      this->host.reply(context, it->second.arguments->usage(name));
      return true; // Player input errors never fault a package or consume a cooldown.
    }
  }
  auto now = std::chrono::steady_clock::now();
  auto& cooldowns = it->second.cooldowns;
  if (it->second.cooldown_ms) {
    for (auto cooldown = cooldowns.begin(); cooldown != cooldowns.end();) {
      cooldown = cooldown->second <= now ? cooldowns.erase(cooldown) : std::next(cooldown);
    }
    if (cooldowns.count(context.client_id) || cooldowns.size() >= 1024) {
      this->host.reply(context, "Please wait before using this command again.");
      return true;
    }
    cooldowns.emplace(context.client_id, now + std::chrono::milliseconds(it->second.cooldown_ms));
  }
  if (!this->call(it->second.ref, context, args, parsed ? &*parsed : nullptr)) {
    this->host.reply(context, "Extension command failed; see the server log.");
  }
  return true;
}

void Runtime::emit(const std::string& event, Context context) {
  if (event == "package_load" || event == "package_unload" || event == "game_opened" || event == "game_closed" ||
      event == "quest_assigned" || event == "quest_map_loaded" || event == "quest_load_acknowledged" || event == "quest_join_acknowledged" ||
      event == "ep3_battle_ended") return;
  if (event == "game_join_ready" && (!context.client_id || context.lobby_id <= 0 || !context.is_game)) return;
  if (event == "client_login" || event == "client_disconnect" || event == "game_join_ready") {
    context.allow_player_actions = false;
  }
  auto it = this->events.find(event);
  if (!this->active || it == this->events.end()) {
    return;
  }
  auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(10);
  for (int ref : it->second) {
    if (!this->active || std::chrono::steady_clock::now() >= end) {
      break;
    }
    this->call(ref, context, event, nullptr, true);
  }
}

void Runtime::emit_game(const std::string& event, const GameSnapshot& snapshot) {
  if (event != "game_opened" && event != "game_closed" && event != "quest_assigned" && event != "quest_map_loaded") {
    throw std::runtime_error("unsupported game observation event");
  }
  validate_game_snapshot(snapshot);
  if ((event == "quest_assigned" || event == "quest_map_loaded") && !snapshot.quest_number) {
    throw std::runtime_error("quest observation requires an assigned quest");
  }
  this->emit_game_observation(event, snapshot, nullptr);
}

void Runtime::emit_quest_acknowledgement(const QuestLoadAcknowledgement& snapshot) {
  validate_quest_acknowledgement(snapshot);
  this->emit_game_observation(snapshot.kind == QuestAcknowledgementKind::INITIAL_LOAD
          ? "quest_load_acknowledged" : "quest_join_acknowledged", snapshot.game, &snapshot);
}

void Runtime::emit_game_observation(const std::string& event, const GameSnapshot& snapshot, const QuestLoadAcknowledgement* acknowledgement) {
  auto it = this->events.find(event);
  if (this->staging || !this->active || it == this->events.end()) return;
  Context context;
  context.lobby_id = snapshot.lobby_id;
  context.is_game = true;
  context.allow_player_actions = false;
  this->current_game = &snapshot;
  this->current_quest_acknowledgement = acknowledgement;
  auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(10);
  try {
    for (int ref : it->second) {
      if (!this->active || std::chrono::steady_clock::now() >= end) break;
      this->call(ref, context, event, nullptr, true);
    }
  } catch (...) {
    this->current_game = nullptr;
    this->current_quest_acknowledgement = nullptr;
    throw;
  }
  this->current_game = nullptr;
  this->current_quest_acknowledgement = nullptr;
}

void Runtime::call_info(int ref, const std::string& event, const InfoTable& info) {
  Context context;
  context.allow_player_actions = false;
  this->current_info = &info;
  try {
    this->call(ref, context, event, nullptr, true);
  } catch (...) {
    this->current_info = nullptr;
    throw;
  }
  this->current_info = nullptr;
}

void Runtime::emit_info(const std::string& event, const InfoTable& info) {
  if (event != "ep3_battle_ended") throw std::runtime_error("unsupported metadata event");
  size_t budget = 256;
  validate_info(info, 0, budget);
  auto it = this->events.find(event);
  if (this->staging || !this->active || it == this->events.end()) return;
  auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(10);
  for (int ref : it->second) {
    if (!this->active || std::chrono::steady_clock::now() >= end) break;
    this->call_info(ref, event, info);
  }
}

void Runtime::run_schedules(const ClockTime& time) {
  if (this->staging || !this->active) return;
  auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(10);
  // Schedules are fixed after initialization, so indexes stay valid across Lua.
  for (size_t z = 0; z < this->schedules.size(); z++) {
    if (!this->active || std::chrono::steady_clock::now() >= end) break;
    if (!this->schedules[z].spec.matches(time)) continue;
    InfoTable info;
    info.add("event", InfoValue{std::string("schedule")});
    info.add("name", InfoValue{this->schedules[z].name});
    info.add("utc", InfoValue{time.utc});
    this->call_info(this->schedules[z].ref, "schedule", info);
  }
}

void Runtime::emit_lifecycle(Lifecycle event, const std::string& reason) {
  auto it = this->events.find(event == Lifecycle::LOAD ? "package_load" : "package_unload");
  if (!this->active || it == this->events.end()) return;
  Context context;
  context.allow_player_actions = false;
  this->lifecycle = event;
  auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(10);
  try {
    for (int ref : it->second) {
      if (!this->active || std::chrono::steady_clock::now() >= end) break;
      this->call(ref, context, reason, nullptr, event == Lifecycle::LOAD);
    }
  } catch (...) {
    this->lifecycle = Lifecycle::NONE;
    throw;
  }
  this->lifecycle = Lifecycle::NONE;
}

void Runtime::activate(const std::string& reason) {
  if (reason != "startup" && reason != "reload") throw std::runtime_error("invalid package activation reason");
  if (this->activated || this->deactivated) return;
  this->activated = true;
  this->emit_lifecycle(Lifecycle::LOAD, reason);
}

void Runtime::deactivate(const std::string& reason) {
  if (reason != "reload" && reason != "disable" && reason != "shutdown") throw std::runtime_error("invalid package unload reason");
  if (!this->activated || this->deactivated) return;
  this->deactivated = true;
  try {
    this->emit_lifecycle(Lifecycle::UNLOAD, reason);
  } catch (...) {
    this->stop();
    throw;
  }
  this->stop();
}

void Runtime::stop() {
  this->active = false;
  for (const auto& [id, timer] : this->timers) {
    (void)id;
    luaL_unref(this->state, LUA_REGISTRYINDEX, timer.ref);
  }
  this->timers.clear();
}

void Runtime::tick() {
  // Earliest deadline first; map order breaks ties by ID. One dispatch per tick
  // prevents catch-up bursts, including when callbacks schedule more work.
  auto selected = this->timers.end();
  auto now = std::chrono::steady_clock::now();
  for (auto it = this->timers.begin(); it != this->timers.end(); ++it) {
    if (it->second.due <= now && (selected == this->timers.end() || it->second.due < selected->second.due)) {
      selected = it;
    }
  }
  if (selected == this->timers.end()) return;
  const uint64_t id = selected->first;
  Timer timer = selected->second;
  if (!this->active || (!timer.service && !this->host.authorized(timer.context))) {
    this->timers.erase(selected);
    luaL_unref(this->state, LUA_REGISTRYINDEX, timer.ref);
    return;
  }
  if (!timer.interval_ms) this->timers.erase(selected);
  const bool succeeded = this->call(timer.ref, timer.context, "", nullptr, timer.service);
  if (!timer.interval_ms) {
    luaL_unref(this->state, LUA_REGISTRYINDEX, timer.ref);
    return;
  }
  // A repeating callback can cancel itself, cancel other timers, or fault its
  // package. Never hold/reuse an iterator across Lua or unref a removed entry.
  auto current = this->timers.find(id);
  if (current == this->timers.end()) return;
  if (!succeeded || !this->active || !this->host.authorized(timer.context)) {
    luaL_unref(this->state, LUA_REGISTRYINDEX, current->second.ref);
    this->timers.erase(current);
  } else {
    current->second.due = std::chrono::steady_clock::now() + std::chrono::milliseconds(timer.interval_ms);
  }
}

void Runtime::cancel_client(uint64_t client_id) {
  for (auto it = this->timers.begin(); it != this->timers.end();) {
    if (!it->second.service && it->second.context.client_id == client_id) {
      luaL_unref(this->state, LUA_REGISTRYINDEX, it->second.ref);
      it = this->timers.erase(it);
    } else {
      ++it;
    }
  }
  for (auto& [name, command] : this->commands) {
    (void)name;
    command.cooldowns.erase(client_id);
  }
}

void Runtime::require_grant(const char* grant) const {
  if (!this->package.grants.count(grant)) {
    throw std::runtime_error(std::string("missing extension grant: ") + grant);
  }
}

void Runtime::require_context(bool require_live_player) {
  if (this->staging || !this->current_context ||
      (require_live_player && (!this->current_context->allow_player_actions || !this->host.authorized(*this->current_context)))) {
    throw std::runtime_error("operation requires a live authorized player in the original lobby");
  }
  if (++this->api_calls > 32) {
    throw std::runtime_error("callback API call budget exceeded");
  }
}

void Runtime::account_output(size_t size) {
  if (size > 512 || (this->output_bytes += size) > 4096) {
    throw std::runtime_error("callback output budget exceeded");
  }
}

void Runtime::validate_storage() const {
  size_t size = 0;
  for (const auto& [key, value] : this->package.storage) {
    if (key.empty() || key.size() > 64 || value.size() > 4096 || key.find('\0') != std::string::npos || value.find('\0') != std::string::npos) {
      throw std::runtime_error("invalid storage key or value");
    }
    size += key.size() + value.size();
  }
  if (size > MAX_STORAGE_BYTES || this->package.storage.size() > 256) {
    throw std::runtime_error("extension storage quota exceeded");
  }
}

void Runtime::require_storage_access() {
  this->require_grant("storage");
  if (this->migrating) {
    if (++this->api_calls > 32) {
      throw std::runtime_error("migration API call budget exceeded");
    }
  } else {
    this->require_context(false);
  }
}

void Runtime::reset_budget() {
  this->instructions = 0;
  this->api_calls = 0;
  this->output_bytes = 0;
  this->deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(CALLBACK_TIME_BUDGET_MS);
  lua_sethook(this->state, instruction_hook, LUA_MASKCOUNT, 1000);
}

bool Runtime::call(int ref, const Context& context, const std::string& args, const ParsedCommandArguments* parsed,
    bool event_authority) {
  ++this->callbacks;
  this->current_context = &context;
  this->current_event_authority = event_authority;
  this->current_ref = ref;
  this->current_args = &args;
  this->current_parsed_args = parsed;
  this->reset_budget();
  const auto started = this->deadline - std::chrono::milliseconds(CALLBACK_TIME_BUDGET_MS);
  // All allocating Lua operations, including construction of the context table,
  // occur inside this protected C entry point.
  lua_pushcfunction(this->state, invoke);
  int status = lua_pcall(this->state, 0, 0, 0);
  this->current_context = nullptr;
  this->current_event_authority = false;
  this->current_args = nullptr;
  this->current_parsed_args = nullptr;
  this->current_lobby_view.reset();
  this->current_command_catalog.reset();
  this->current_board_top.reset();
  const auto finished = std::chrono::steady_clock::now();
  this->last_callback_us = std::chrono::duration_cast<std::chrono::microseconds>(finished - started).count();
  this->peak_callback_us = std::max(this->peak_callback_us, this->last_callback_us);
  this->total_callback_us += std::min(this->last_callback_us, UINT64_MAX - this->total_callback_us);
  if (finished >= this->deadline && this->over_budget_callbacks != UINT64_MAX) ++this->over_budget_callbacks;
  if (status != LUA_OK) {
    const char* error = lua_type(this->state, -1) == LUA_TSTRING ? lua_tostring(this->state, -1) : "non-string Lua error";
    std::string message(error);
    lua_pop(this->state, 1);
    this->record_error(message);
    return false;
  }
  return true;
}

void Runtime::record_error(const std::string& error) {
  ++this->errors;
  this->last_error = error.substr(0, 1024);
  this->host.log(this->id(), error.substr(0, 1024));
  if (this->errors >= 3) {
    this->stop();
    this->host.log(this->id(), "Disabled after three callback failures; reload to re-enable.");
  }
}

Runtime& Runtime::self(lua_State* L) { return **static_cast<Runtime**>(lua_getextraspace(L)); }

void* Runtime::allocate(void* user, void* ptr, size_t old_size, size_t new_size) {
  auto& runtime = *static_cast<Runtime*>(user);
  size_t previous = ptr ? old_size : 0;
  if (!new_size) {
    std::free(ptr);
    runtime.allocated -= previous;
    return nullptr;
  }
  if (new_size > MAX_MEMORY_BYTES || runtime.allocated - previous > MAX_MEMORY_BYTES - new_size) {
    return nullptr;
  }
  void* result = std::realloc(ptr, new_size);
  if (result) {
    runtime.allocated = runtime.allocated - previous + new_size;
    runtime.peak_allocated = std::max(runtime.peak_allocated, runtime.allocated);
  }
  return result;
}

void Runtime::instruction_hook(lua_State* L, lua_Debug*) {
  auto& runtime = self(L);
  runtime.instructions += 1000;
  if (runtime.instructions >= 100000 || std::chrono::steady_clock::now() >= runtime.deadline) {
    luaL_error(L, "callback instruction/time budget exceeded");
  }
}

int Runtime::initialize(lua_State* L) {
  luaL_requiref(L, "_G", luaopen_base, 1);
  lua_pop(L, 1);
  // No protected calls/coroutines: scripts cannot catch and loop past a budget
  // error. No metatables/finalizers, dynamic loading, filesystem, OS, or debug API.
  for (const char* name : {"collectgarbage", "dofile", "load", "loadfile", "getmetatable", "setmetatable", "pcall", "xpcall", "print", "warn"}) {
    lua_pushnil(L);
    lua_setglobal(L, name);
  }
  luaL_requiref(L, "math", luaopen_math, 1);
  lua_pop(L, 1);
  luaL_requiref(L, "table", luaopen_table, 1);
  for (const char* name : {"move", "sort"}) {
    lua_pushnil(L);
    lua_setfield(L, -2, name);
  }
  lua_pop(L, 1);
  luaL_requiref(L, "utf8", luaopen_utf8, 1);
  lua_pop(L, 1);
  luaL_requiref(L, "string", luaopen_string, 1);
  for (const char* name : {"dump", "find", "gmatch", "gsub", "match", "rep"}) {
    lua_pushnil(L);
    lua_setfield(L, -2, name);
  }
  lua_pop(L, 1);

  lua_newtable(L);
  const luaL_Reg functions[] = {
      {"command", api<&Runtime::register_command>},
      {"on", api<&Runtime::register_event>},
      {"after", api<&Runtime::after>},
      {"every", api<&Runtime::every>},
      {"cancel", api<&Runtime::cancel>},
      {"reply", api<&Runtime::reply>},
      {"broadcast", api<&Runtime::broadcast>},
      {"announce", api<&Runtime::announce>},
      {"whisper", api<&Runtime::whisper>},
      {"log", api<&Runtime::log>},
      {"storage_get", api<&Runtime::storage_get>},
      {"storage_set", api<&Runtime::storage_set>},
      {"migrate", api<&Runtime::register_migration>},
      {"setting", api<&Runtime::setting>},
      {"package_text", api<&Runtime::package_text>},
      {"has_permission", api<&Runtime::has_permission>},
      {"lobby", api<&Runtime::lobby>},
      {"scene_capabilities", api<&Runtime::scene_capabilities>},
      {"scene_asset_support", api<&Runtime::scene_asset_support>},
      {"commands", api<&Runtime::command_list>},
      {"scene_spawn", api<&Runtime::scene_spawn>},
      {"scene_remove", api<&Runtime::scene_remove>},
      {"scene_event", api<&Runtime::scene_event>},
      {"now", api<&Runtime::now>},
      {"schedule", api<&Runtime::schedule>},
      {"service_after", api<&Runtime::service_after>},
      {"player_get", api<&Runtime::player_get>},
      {"player_set", api<&Runtime::player_set>},
      {"board_add", api<&Runtime::board_add>},
      {"board_get", api<&Runtime::board_get>},
      {"board_top", api<&Runtime::board_top>},
      {"board_reset", api<&Runtime::board_reset>},
      {"meseta_grant", api<&Runtime::meseta_grant>},
      {"caelum_object", api<&Runtime::caelum_object>},
      {"caelum_remove", api<&Runtime::caelum_remove>},
      {"caelum_music", api<&Runtime::caelum_music>},
      {"caelum_clear", api<&Runtime::caelum_clear>},
      {"caelum_listeners", api<&Runtime::caelum_listeners>},
      {"caelum_lobby", api<&Runtime::caelum_lobby>},
      {"position", api<&Runtime::position>},
      {nullptr, nullptr}};
  luaL_setfuncs(L, functions, 0);
  lua_setglobal(L, "extensions");
  auto& runtime = self(L);
  if (luaL_loadbufferx(L, runtime.package.source.data(), runtime.package.source.size(), runtime.id().c_str(), "t") != LUA_OK) {
    return lua_error(L);
  }
  lua_call(L, 0, 0);
  if (runtime.package.storage_version != runtime.package.target_storage_version) {
    if (!runtime.migration_ref) {
      return luaL_error(L, "storage upgrade requires extensions.migrate(handler)");
    }
    runtime.reset_budget();
    runtime.migrating = true;
    lua_rawgeti(L, LUA_REGISTRYINDEX, runtime.migration_ref);
    lua_pushinteger(L, runtime.package.storage_version);
    lua_pushinteger(L, runtime.package.target_storage_version);
    lua_call(L, 2, 0);
    runtime.migrating = false;
  }
  return 0;
}

int Runtime::invoke(lua_State* L) {
  auto& runtime = self(L);
  lua_rawgeti(L, LUA_REGISTRYINDEX, runtime.current_ref);
  if (runtime.current_info) {
    push_info(L, *runtime.current_info);
    lua_call(L, 1, 0);
    return 0;
  }
  if (runtime.current_game) {
    const auto& game = *runtime.current_game;
    lua_createtable(L, 0, runtime.current_quest_acknowledgement ? 11 : 9);
    char id[32];
    std::snprintf(id, sizeof(id), "%lld", static_cast<long long>(game.lobby_id));
    lua_pushstring(L, id);
    lua_setfield(L, -2, "lobby_id");
    lua_pushlstring(L, game.name.data(), game.name.size());
    lua_setfield(L, -2, "name");
    lua_pushinteger(L, game.episode);
    lua_setfield(L, -2, "episode");
    lua_pushinteger(L, game.mode);
    lua_setfield(L, -2, "mode");
    lua_pushinteger(L, game.difficulty);
    lua_setfield(L, -2, "difficulty");
    if (game.quest_number) lua_pushinteger(L, *game.quest_number);
    else lua_pushnil(L);
    lua_setfield(L, -2, "quest_number");
    if (game.quest_number) lua_pushboolean(L, game.quest_joinable);
    else lua_pushnil(L);
    lua_setfield(L, -2, "quest_joinable");
    if (runtime.current_quest_acknowledgement) {
      const auto& ack = *runtime.current_quest_acknowledgement;
      std::snprintf(id, sizeof(id), "%llu", static_cast<unsigned long long>(ack.client_id));
      lua_pushstring(L, id);
      lua_setfield(L, -2, "client_id");
      lua_pushlstring(L, ack.client_version.data(), ack.client_version.size());
      lua_setfield(L, -2, "client_version");
    }
    lua_pushlstring(L, runtime.current_args->data(), runtime.current_args->size());
    lua_setfield(L, -2, "event");
    lua_pushstring(L, *runtime.current_args == "game_opened" ? "first_join" :
        *runtime.current_args == "game_closed" ? "removed" :
        *runtime.current_args == "quest_assigned" ? "assigned" :
        *runtime.current_args == "quest_load_acknowledged" ? "load_acknowledged" :
        *runtime.current_args == "quest_join_acknowledged" ?
            (runtime.current_quest_acknowledgement->kind == QuestAcknowledgementKind::JOINABLE_BB_READY ? "bb_ready" : "player_visible") : "map_loaded");
    lua_setfield(L, -2, "reason");
    lua_call(L, 1, 0);
    return 0;
  }
  if (runtime.lifecycle != Lifecycle::NONE) {
    lua_createtable(L, 0, 4);
    lua_pushlstring(L, runtime.id().data(), runtime.id().size());
    lua_setfield(L, -2, "id");
    lua_pushlstring(L, runtime.version().data(), runtime.version().size());
    lua_setfield(L, -2, "version");
    lua_pushstring(L, runtime.lifecycle == Lifecycle::LOAD ? "package_load" : "package_unload");
    lua_setfield(L, -2, "event");
    lua_pushlstring(L, runtime.current_args->data(), runtime.current_args->size());
    lua_setfield(L, -2, "reason");
    lua_call(L, 1, 0);
    return 0;
  }
  lua_newtable(L);
  // IDs are strings so no Lua number precision or signedness is involved.
  char id[32];
  std::snprintf(id, sizeof(id), "%llu", static_cast<unsigned long long>(runtime.current_context->client_id));
  lua_pushstring(L, id);
  lua_setfield(L, -2, "client_id");
  std::snprintf(id, sizeof(id), "%lld", static_cast<long long>(runtime.current_context->lobby_id));
  lua_pushstring(L, id);
  lua_setfield(L, -2, "lobby_id");
  lua_pushstring(L, runtime.current_context->player_name.c_str());
  lua_setfield(L, -2, "player_name");
  lua_pushlstring(L, runtime.current_context->client_version.data(), runtime.current_context->client_version.size());
  lua_setfield(L, -2, "client_version");
  lua_pushboolean(L, runtime.current_context->is_game);
  lua_setfield(L, -2, "is_game");
  lua_pushboolean(L, runtime.current_context->allow_player_actions);
  lua_setfield(L, -2, "can_act");
  if (!runtime.current_context->player_key.empty()) {
    lua_pushlstring(L, runtime.current_context->player_key.data(), runtime.current_context->player_key.size());
    lua_setfield(L, -2, "player_key");
  }
  lua_pushlstring(L, runtime.current_args->data(), runtime.current_args->size());
  if (runtime.current_parsed_args) {
    lua_newtable(L);
    for (const auto& [name, value] : *runtime.current_parsed_args) {
      if (auto integer = std::get_if<int64_t>(&value)) {
        static_assert(sizeof(lua_Integer) >= sizeof(int64_t));
        lua_pushinteger(L, *integer);
      } else {
        const auto& text = std::get<std::string>(value);
        lua_pushlstring(L, text.data(), text.size());
      }
      lua_setfield(L, -2, name.c_str());
    }
  } else {
    lua_pushnil(L);
  }
  lua_call(L, 3, 0);
  return 0;
}

template <int (Runtime::*Fn)(lua_State*)>
int Runtime::api(lua_State* L) {
  auto& runtime = self(L);
  try {
    return (runtime.*Fn)(L);
  } catch (const std::exception& e) {
    std::snprintf(runtime.error_buffer, sizeof(runtime.error_buffer), "%s", e.what());
  } catch (...) {
    std::snprintf(runtime.error_buffer, sizeof(runtime.error_buffer), "unknown host exception");
  }
  lua_pushstring(L, runtime.error_buffer);
  return lua_error(L);
}

int Runtime::register_command(lua_State* L) {
  if (!this->staging || this->migrating || this->commands.size() >= MAX_REGISTRATIONS) {
    throw std::runtime_error("commands must be registered during initialization (maximum 32)");
  }
  this->require_grant("commands");
  const char* name = string_arg(L, 1, 64);
  if (lua_type(L, 2) != LUA_TTABLE || lua_type(L, 3) != LUA_TFUNCTION) {
    throw std::runtime_error("command expects name, options table, and function");
  }
  // raw access avoids user metamethods while C++ locals are alive.
  lua_pushliteral(L, "permission");
  lua_rawget(L, 2);
  const char* permission = lua_isnil(L, -1) ? "everyone" : string_arg(L, -1, 32);
  lua_pushliteral(L, "cooldown_ms");
  lua_rawget(L, 2);
  lua_Integer cooldown = lua_isnil(L, -1) ? 1000 : lua_tointeger(L, -1);
  if ((!lua_isnil(L, -1) && !lua_isinteger(L, -1)) || cooldown < 0 || cooldown > 3600000) {
    throw std::runtime_error("cooldown_ms must be an integer from 0 to 3600000");
  }
  if (!valid_name(name) || !valid_permission(permission) || this->commands.count(name) || this->host.command_reserved(name)) {
    throw std::runtime_error("invalid/reserved/duplicate command name or permission");
  }
  lua_pushliteral(L, "help");
  lua_rawget(L, 2);
  std::string help = lua_isnil(L, -1) ? "" : string_arg(L, -1, 160);
  if (std::any_of(help.begin(), help.end(), [](unsigned char c) { return c < 0x20 || c == 0x7F; })) {
    throw std::runtime_error("command help must be single-line plain text");
  }
  std::optional<CommandArguments> arguments;
  lua_pushliteral(L, "arguments");
  lua_rawget(L, 2);
  if (!lua_isnil(L, -1)) {
    if (lua_type(L, -1) != LUA_TTABLE || lua_rawlen(L, -1) > 8) {
      throw std::runtime_error("arguments must be a dense array of at most eight definitions");
    }
    int schema = lua_gettop(L);
    size_t count = lua_rawlen(L, schema);
    size_t seen = 0;
    lua_pushnil(L);
    while (lua_next(L, schema)) {
      if (!lua_isinteger(L, -2) || lua_tointeger(L, -2) < 1 ||
          static_cast<uint64_t>(lua_tointeger(L, -2)) > count || lua_type(L, -1) != LUA_TTABLE) {
        throw std::runtime_error("arguments must be a dense array of definitions");
      }
      ++seen;
      lua_pop(L, 1);
    }
    if (seen != count) {
      throw std::runtime_error("arguments array has holes");
    }
    std::vector<CommandArgument> fields;
    for (size_t z = 1; z <= count; ++z) {
      lua_rawgeti(L, schema, z);
      int definition = lua_gettop(L);
      static const std::set<std::string> keys{"name", "type", "optional", "max_bytes", "min", "max"};
      lua_pushnil(L);
      while (lua_next(L, definition)) {
        if (!keys.count(string_arg(L, -2, 32))) {
          throw std::runtime_error("unknown argument schema option");
        }
        lua_pop(L, 1);
      }
      auto get = [&](const char* key) {
        lua_pushstring(L, key);
        lua_rawget(L, definition);
      };
      CommandArgument field;
      get("name");
      field.name = string_arg(L, -1, 32);
      lua_pop(L, 1);
      get("type");
      std::string type = string_arg(L, -1, 16);
      lua_pop(L, 1);
      if (type == "word") field.type = CommandArgument::Type::WORD;
      else if (type == "text") field.type = CommandArgument::Type::TEXT;
      else if (type == "integer") field.type = CommandArgument::Type::INTEGER;
      else if (type == "client_id") {
        field.type = CommandArgument::Type::CLIENT_ID;
        field.max_bytes = 20;
      }
      else throw std::runtime_error("argument type must be word, text, integer, or client_id");
      get("optional");
      if (!lua_isnil(L, -1) && lua_type(L, -1) != LUA_TBOOLEAN) {
        throw std::runtime_error("argument optional must be a boolean");
      }
      field.optional = lua_toboolean(L, -1);
      lua_pop(L, 1);
      for (const char* key : {"max_bytes", "min", "max"}) {
        get(key);
        if (!lua_isnil(L, -1)) {
          if (!lua_isinteger(L, -1)) throw std::runtime_error("argument bounds must be integers");
          auto bound = lua_tointeger(L, -1);
          if (!std::strcmp(key, "max_bytes")) {
            if (bound < 1 || bound > 1024) throw std::runtime_error("max_bytes must be 1..1024");
            field.max_bytes = bound;
          } else {
            if (field.type != CommandArgument::Type::INTEGER) throw std::runtime_error("min/max require an integer argument");
            if (!std::strcmp(key, "min")) field.minimum = bound;
            else field.maximum = bound;
          }
        }
        lua_pop(L, 1);
      }
      fields.emplace_back(std::move(field));
      lua_pop(L, 1);
    }
    arguments.emplace(std::move(fields));
    if (arguments->usage(name).size() > 200) {
      throw std::runtime_error("generated command usage must fit 200 bytes; shorten argument names");
    }
  }
  lua_pushvalue(L, 3);
  int ref = luaL_ref(L, LUA_REGISTRYINDEX);
  this->commands.emplace(name, Command{ref, permission, static_cast<uint32_t>(cooldown), {}, std::move(arguments), std::move(help)});
  return 0;
}

int Runtime::register_event(lua_State* L) {
  this->require_grant("events");
  const char* name = string_arg(L, 1, 32);
  if (!this->staging || this->migrating || lua_type(L, 2) != LUA_TFUNCTION ||
      (std::strcmp(name, "lobby_join") && std::strcmp(name, "lobby_leave") &&
          std::strcmp(name, "client_login") && std::strcmp(name, "client_disconnect") &&
          std::strcmp(name, "game_join_ready") &&
          std::strcmp(name, "package_load") && std::strcmp(name, "package_unload") &&
          std::strcmp(name, "game_opened") && std::strcmp(name, "game_closed") &&
          std::strcmp(name, "quest_assigned") && std::strcmp(name, "quest_map_loaded") && std::strcmp(name, "quest_load_acknowledged") &&
          std::strcmp(name, "quest_join_acknowledged") && std::strcmp(name, "ep3_battle_ended"))) {
    throw std::runtime_error("on expects a supported event and an initialization-time function");
  }
  size_t count = 0;
  for (const auto& [event, refs] : this->events) {
    (void)event;
    count += refs.size();
  }
  if (count >= MAX_REGISTRATIONS) {
    throw std::runtime_error("event registration quota exceeded");
  }
  lua_pushvalue(L, 2);
  int ref = luaL_ref(L, LUA_REGISTRYINDEX);
  this->events[name].emplace_back(ref);
  return 0;
}

int Runtime::after(lua_State* L) {
  return this->schedule_timer(L, false);
}

int Runtime::every(lua_State* L) {
  return this->schedule_timer(L, true);
}

int Runtime::schedule_timer(lua_State* L, bool repeating) {
  this->require_grant("timers");
  this->require_context();
  lua_Integer ms = lua_tointeger(L, 1);
  if (!lua_isinteger(L, 1) || ms < 50 || ms > 3600000 || lua_type(L, 2) != LUA_TFUNCTION) {
    throw std::runtime_error(repeating ? "every expects 50..3600000 milliseconds and a function" :
        "after expects 50..3600000 milliseconds and a function");
  }
  if (this->timers.size() >= MAX_TIMERS) {
    lua_pushnil(L);
    lua_pushliteral(L, "timer quota exceeded");
    return 2;
  }
  lua_pushvalue(L, 2);
  int ref = luaL_ref(L, LUA_REGISTRYINDEX);
  uint64_t id = this->next_timer_id++;
  this->timers.emplace(id, Timer{ref, *this->current_context, std::chrono::steady_clock::now() + std::chrono::milliseconds(ms),
      repeating ? static_cast<uint32_t>(ms) : 0});
  lua_pushinteger(L, id);
  return 1;
}

int Runtime::cancel(lua_State* L) {
  if (!lua_isinteger(L, 1)) {
    throw std::runtime_error("cancel expects an integer timer ID");
  }
  auto it = this->timers.find(lua_tointeger(L, 1));
  bool found = it != this->timers.end();
  if (found && it->second.service) {
    // Service timers have no player; any callback of the owning package may cancel them.
    this->require_grant("schedule");
    this->require_context(false);
  } else {
    this->require_grant("timers");
    this->require_context();
  }
  if (found) {
    luaL_unref(L, LUA_REGISTRYINDEX, it->second.ref);
    this->timers.erase(it);
  }
  lua_pushboolean(L, found);
  return 1;
}

int Runtime::reply(lua_State* L) {
  this->require_grant("chat.reply");
  this->require_context();
  const char* text = string_arg(L, 1, 512);
  this->account_output(std::strlen(text));
  this->host.reply(*this->current_context, text);
  return 0;
}

int Runtime::broadcast(lua_State* L) {
  this->require_grant("chat.broadcast");
  this->require_context();
  const char* text = string_arg(L, 1, 512);
  this->account_output(std::strlen(text));
  this->host.broadcast(*this->current_context, text);
  return 0;
}

int Runtime::announce(lua_State* L) {
  this->require_grant("chat.announce");
  this->require_context(false);
  if (this->lifecycle == Lifecycle::UNLOAD) {
    throw std::runtime_error("announcements are unavailable during package unload");
  }
  if (lua_gettop(L) != 1) throw std::runtime_error("announce expects exactly one string");
  const char* text = string_arg(L, 1, 512);
  this->account_output(std::strlen(text));
  // This grant permits trusted event automation, not public command escalation.
  // Timer callbacks use their captured player's authority, never event authority.
  bool authorized = this->current_event_authority;
  if (!authorized && this->current_context->allow_player_actions && this->host.authorized(*this->current_context)) {
    auto context = *this->current_context;
    context.permission = "announce";
    authorized = this->host.authorized(context);
  }
  AnnouncementResult result{false, "not_authorized"};
  if (authorized) {
    if (plain_message_text(text).find_first_not_of(" \n") == std::string::npos) {
      result = {false, "empty_message"};
    } else {
      result = this->host.announce(this->id(), text);
    }
  }
  lua_pushboolean(L, result.ok);
  if (result.reason) lua_pushstring(L, result.reason);
  else lua_pushnil(L);
  return 2;
}

int Runtime::whisper(lua_State* L) {
  this->require_grant("chat.whisper");
  this->require_context();
  const char* target = string_arg(L, 1, 20);
  const char* text = string_arg(L, 2, 512);
  uint64_t target_id = 0;
  auto result = std::from_chars(target, target + std::strlen(target), target_id);
  if (result.ec != std::errc() || *result.ptr) {
    lua_pushboolean(L, false);
    return 1;
  }
  this->account_output(std::strlen(text));
  bool sent = this->host.whisper(*this->current_context, target_id, text);
  lua_pushboolean(L, sent);
  return 1;
}

int Runtime::log(lua_State* L) {
  this->require_context(false);
  const char* text = string_arg(L, 1, 512);
  this->account_output(std::strlen(text));
  this->host.log(this->id(), text);
  return 0;
}

int Runtime::lobby(lua_State* L) {
  this->require_grant("lobby.read");
  this->require_context(false);
  if (!this->current_context->allow_player_actions || !this->host.authorized(*this->current_context)) {
    lua_pushnil(L);
    return 1;
  }
  this->current_lobby_view = this->host.lobby_view(*this->current_context);
  if (!this->current_lobby_view) {
    lua_pushnil(L);
    return 1;
  }
  const auto& view = *this->current_lobby_view;
  validate_lobby_view(view, this->current_context->client_id, this->current_context->lobby_id);
  lua_createtable(L, 0, 6);
  char id[32];
  std::snprintf(id, sizeof(id), "%lld", static_cast<long long>(view.lobby_id));
  lua_pushstring(L, id);
  lua_setfield(L, -2, "lobby_id");
  lua_pushlstring(L, view.name.data(), view.name.size());
  lua_setfield(L, -2, "name");
  lua_pushboolean(L, view.is_game);
  lua_setfield(L, -2, "is_game");
  lua_pushinteger(L, view.event);
  lua_setfield(L, -2, "event");
  lua_pushinteger(L, view.capacity);
  lua_setfield(L, -2, "capacity");
  lua_createtable(L, view.players.size(), 0);
  for (size_t z = 0; z < view.players.size(); z++) {
    const auto& player = view.players[z];
    lua_createtable(L, 0, 6);
    std::snprintf(id, sizeof(id), "%llu", static_cast<unsigned long long>(player.client_id));
    lua_pushstring(L, id);
    lua_setfield(L, -2, "client_id");
    lua_pushinteger(L, player.slot);
    lua_setfield(L, -2, "slot");
    lua_pushlstring(L, player.player_name.data(), player.player_name.size());
    lua_setfield(L, -2, "player_name");
    lua_pushlstring(L, player.client_version.data(), player.client_version.size());
    lua_setfield(L, -2, "client_version");
    lua_pushboolean(L, player.client_id == this->current_context->client_id);
    lua_setfield(L, -2, "is_self");
    lua_pushboolean(L, player.is_leader);
    lua_setfield(L, -2, "is_leader");
    lua_rawseti(L, -2, z + 1);
  }
  lua_setfield(L, -2, "players");
  this->current_lobby_view.reset();
  return 1;
}

int Runtime::command_list(lua_State* L) {
  this->require_grant("commands.read");
  this->require_context(false);
  if (lua_gettop(L)) throw std::runtime_error("commands expects no arguments");
  if (!this->current_context->allow_player_actions || !this->host.authorized(*this->current_context)) {
    lua_pushnil(L);
    return 1;
  }
  this->current_command_catalog = this->host.command_catalog(*this->current_context);
  if (!this->current_command_catalog) {
    lua_pushnil(L);
    return 1;
  }
  validate_command_catalog(*this->current_command_catalog);
  std::erase_if(*this->current_command_catalog, [&](const auto& command) {
    auto context = *this->current_context;
    context.permission = command.permission;
    return !this->host.authorized(context);
  });
  std::sort(this->current_command_catalog->begin(), this->current_command_catalog->end(),
      [](const auto& a, const auto& b) { return a.name < b.name; });
  // Native snapshot ownership stays on Runtime across any Lua allocation error.
  lua_createtable(L, this->current_command_catalog->size(), 0);
  for (size_t z = 0; z < this->current_command_catalog->size(); z++) {
    const auto& command = this->current_command_catalog->at(z);
    lua_createtable(L, 0, 6);
    lua_pushlstring(L, command.name.data(), command.name.size());
    lua_setfield(L, -2, "name");
    lua_pushlstring(L, command.package_id.data(), command.package_id.size());
    lua_setfield(L, -2, "package_id");
    lua_pushlstring(L, command.permission.data(), command.permission.size());
    lua_setfield(L, -2, "permission");
    lua_pushinteger(L, command.cooldown_ms);
    lua_setfield(L, -2, "cooldown_ms");
    lua_pushlstring(L, command.usage.data(), command.usage.size());
    lua_setfield(L, -2, "usage");
    lua_pushlstring(L, command.help.data(), command.help.size());
    lua_setfield(L, -2, "help");
    lua_rawseti(L, -2, z + 1);
  }
  this->current_command_catalog.reset();
  return 1;
}

bool Runtime::valid_data_file_name(const std::string& name) {
  if (name.size() < 5 || name.size() > 64 || name[0] == '.' || name.substr(name.size() - 4) != ".txt") return false;
  for (char ch : name) {
    if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.')) return false;
  }
  return true;
}

int Runtime::has_permission(lua_State* L) {
  // Whether the player of this callback holds a permission level (the same levels and account flags as a command's
  // permission): for choices that only some players may use. false for a departed or unauthorized player.
  this->require_context(false);
  std::string level = string_arg(L, 1, 16);
  if (!valid_permission(level)) {
    throw std::runtime_error("permission must be everyone, announce, debug, moderator, admin, root or change-event");
  }
  Context copy = *this->current_context;
  copy.permission = level;
  lua_pushboolean(L, this->current_context->allow_player_actions && this->host.authorized(copy));
  return 1;
}

int Runtime::package_text(lua_State* L) {
  // A text file of this package (manifest "data_files"), read when the package was staged; nil for a name the
  // manifest does not list. Available everywhere, initialization included; no grant (the package's own files).
  if (++this->api_calls > 32) throw std::runtime_error("callback API call budget exceeded");
  const char* name = string_arg(L, 1, 64);
  auto it = this->package.data_files.find(name);
  if (it == this->package.data_files.end()) lua_pushnil(L);
  else lua_pushlstring(L, it->second.data(), it->second.size());
  return 1;
}

int Runtime::setting(lua_State* L) {
  // Available during initialization and migration, without a player context.
  // Reads share the API budget; scalar copies cannot mutate host configuration.
  if (++this->api_calls > 32) throw std::runtime_error("callback API call budget exceeded");
  const char* name = string_arg(L, 1, 64);
  auto it = this->package.settings.find(name);
  if (it == this->package.settings.end()) throw std::runtime_error("undeclared extension setting");
  if (!it->second.value) {
    lua_pushnil(L);
  } else if (const auto* value = std::get_if<bool>(&*it->second.value)) {
    lua_pushboolean(L, *value);
  } else if (const auto* value = std::get_if<int64_t>(&*it->second.value)) {
    lua_pushinteger(L, *value);
  } else {
    const auto& text = std::get<std::string>(*it->second.value);
    lua_pushlstring(L, text.data(), text.size());
  }
  return 1;
}

int Runtime::storage_get(lua_State* L) {
  this->require_storage_access();
  const char* key = string_arg(L, 1, 64);
  auto it = this->package.storage.find(key);
  if (it == this->package.storage.end()) {
    lua_pushnil(L);
  } else {
    lua_pushlstring(L, it->second.data(), it->second.size());
  }
  return 1;
}

int Runtime::storage_set(lua_State* L) {
  this->require_storage_access();
  if (this->lifecycle == Lifecycle::UNLOAD) throw std::runtime_error("package_unload storage is read-only");
  const char* key = string_arg(L, 1, 64);
  const char* value = lua_isnil(L, 2) ? nullptr : string_arg(L, 2, 4096);
  if (!*key) {
    throw std::runtime_error("storage key must not be empty");
  }
  size_t size = std::strlen(key) + (value ? std::strlen(value) : 0);
  for (const auto& [k, v] : this->package.storage) {
    if (k != key) {
      size += k.size() + v.size();
    }
  }
  if (value && (size > MAX_STORAGE_BYTES || (!this->package.storage.count(key) && this->package.storage.size() >= 256))) {
    throw std::runtime_error("extension storage quota exceeded");
  }
  if (value) {
    this->package.storage[key] = value;
  } else {
    this->package.storage.erase(key);
  }
  this->dirty = true;
  return 0;
}

int Runtime::register_migration(lua_State* L) {
  this->require_grant("storage");
  if (!this->staging || this->migrating || this->migration_ref || lua_type(L, 1) != LUA_TFUNCTION) {
    throw std::runtime_error("migrate expects one initialization-time handler per package");
  }
  lua_pushvalue(L, 1);
  this->migration_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  return 0;
}

int Runtime::scene_capabilities(lua_State* L) {
  this->require_context(false);
  auto caps = this->current_context->allow_player_actions && this->host.authorized(*this->current_context)
      ? this->host.scene_capabilities(*this->current_context)
      : SceneCapabilities{};
  lua_newtable(L);
  lua_pushboolean(L, caps.media);
  lua_setfield(L, -2, "native_ep3_media");
  lua_pushboolean(L, caps.models);
  lua_setfield(L, -2, "experimental_models");
  lua_pushboolean(L, caps.events);
  lua_setfield(L, -2, "lobby_events");
  lua_pushboolean(L, caps.extended_media);
  lua_setfield(L, -2, "extended_ep3_media");
  auto media_profile = caps.extended_media ? Episode3::LobbyMediaProfile::PC_PORT : Episode3::LobbyMediaProfile::ORIGINAL;
  lua_pushinteger(L, caps.media ? Episode3::lobby_media_compressed_limit(media_profile) : 0);
  lua_setfield(L, -2, "max_media_compressed_bytes");
  lua_pushinteger(L, caps.media ? Episode3::lobby_media_decompressed_limit(media_profile) : 0);
  lua_setfield(L, -2, "max_media_decompressed_bytes");
  lua_pushboolean(L, caps.media);
  lua_setfield(L, -2, "fixed_slots");
  lua_pushboolean(L, false);
  lua_setfield(L, -2, "arbitrary_transforms");
  return 1;
}

int Runtime::scene_asset_support(lua_State* L) {
  this->require_grant("scene.media");
  this->require_context(false);
  if (lua_gettop(L) != 1) throw std::runtime_error("scene_asset_support expects one asset ID");
  const char* asset = string_arg(L, 1, 64);
  auto support = this->current_context->allow_player_actions && this->host.authorized(*this->current_context)
      ? this->host.scene_asset_support(*this->current_context, this->id(), asset)
      : SceneAssetSupport{};
  lua_createtable(L, 0, 3);
  lua_pushboolean(L, support.supported);
  lua_setfield(L, -2, "supported");
  if (support.reason) lua_pushstring(L, support.reason);
  else lua_pushnil(L);
  lua_setfield(L, -2, "reason");
  if (support.variant) lua_pushstring(L, support.variant);
  else lua_pushnil(L);
  lua_setfield(L, -2, "variant");
  return 1;
}

int Runtime::scene_spawn(lua_State* L) {
  this->require_grant("scene.media");
  this->require_context();
  const char* key = string_arg(L, 1, 64);
  const char* asset = string_arg(L, 2, 64);
  const char* slot = string_arg(L, 3, 64);
  lua_Integer lifetime = 0;
  if (!lua_isnoneornil(L, 4)) {
    if (!lua_isinteger(L, 4)) throw std::runtime_error("scene lifetime must be an integer");
    lifetime = lua_tointeger(L, 4);
    if (lifetime < 0 || (lifetime && lifetime < 50) || lifetime > SceneService::MAX_LIFETIME_MS) {
      throw std::runtime_error("scene lifetime must be zero or 50..3600000 ms");
    }
  }
  auto result = this->host.scene_spawn(*this->current_context, this->id(), key, asset, slot, static_cast<uint32_t>(lifetime));
  lua_pushboolean(L, result.ok);
  if (result.reason) {
    lua_pushstring(L, result.reason);
  } else {
    lua_pushnil(L);
  }
  return 2;
}

int Runtime::scene_remove(lua_State* L) {
  this->require_grant("scene.media");
  this->require_context();
  const char* key = string_arg(L, 1, 64);
  auto result = this->host.scene_remove(*this->current_context, this->id(), key);
  lua_pushboolean(L, result.ok);
  if (result.reason) {
    lua_pushstring(L, result.reason);
  } else {
    lua_pushnil(L);
  }
  return 2;
}

int Runtime::scene_event(lua_State* L) {
  this->require_grant("scene.event");
  this->require_context();
  int event = -1;
  if (!lua_isnil(L, 1)) {
    if (!lua_isinteger(L, 1) || lua_tointeger(L, 1) < 0 || lua_tointeger(L, 1) > 14) {
      throw std::runtime_error("scene_event expects an event number 0..14 or nil to release ownership");
    }
    event = static_cast<int>(lua_tointeger(L, 1));
  }
  auto result = this->host.scene_event(*this->current_context, this->id(), event);
  lua_pushboolean(L, result.ok);
  if (result.reason) {
    lua_pushstring(L, result.reason);
  } else {
    lua_pushnil(L);
  }
  return 2;
}

int Runtime::now(lua_State* L) {
  // Available everywhere, including initialization. Shares the API budget.
  if (++this->api_calls > 32) throw std::runtime_error("callback API call budget exceeded");
  if (lua_gettop(L)) throw std::runtime_error("now expects no arguments");
  const ClockTime t = this->host.now();
  lua_createtable(L, 0, 13);
  const std::pair<const char*, int64_t> fields[] = {
      {"utc", t.utc}, {"offset_seconds", t.offset_seconds}, {"year", t.year}, {"month", t.month}, {"day", t.day},
      {"hour", t.hour}, {"minute", t.minute}, {"second", t.second}, {"weekday", t.weekday}, {"yday", t.yday},
      {"iso_year", t.iso_year}, {"iso_week", t.iso_week}, {"day_number", t.day_number}};
  for (const auto& [name, value] : fields) {
    lua_pushinteger(L, value);
    lua_setfield(L, -2, name);
  }
  return 1;
}

int Runtime::schedule(lua_State* L) {
  this->require_grant("schedule");
  if (!this->staging || this->migrating || this->schedules.size() >= MAX_SCHEDULES) {
    throw std::runtime_error("schedules must be registered during initialization (maximum 16)");
  }
  std::string name = string_arg(L, 1, 64);
  if (!valid_name(name) || lua_type(L, 2) != LUA_TTABLE || lua_type(L, 3) != LUA_TFUNCTION) {
    throw std::runtime_error("schedule expects a name, a rule table, and a function");
  }
  for (const auto& existing : this->schedules) {
    if (existing.name == name) throw std::runtime_error("duplicate schedule name");
  }
  static const std::set<std::string> keys{"minute", "hour", "weekday", "day", "month", "every_minutes"};
  lua_pushnil(L);
  while (lua_next(L, 2)) {
    if (lua_type(L, -2) != LUA_TSTRING || !keys.count(string_arg(L, -2, 32))) {
      throw std::runtime_error("unknown schedule rule field");
    }
    lua_pop(L, 1);
  }
  ScheduleSpec spec;
  spec.minutes = schedule_field(L, 2, "minute");
  spec.hours = schedule_field(L, 2, "hour");
  spec.weekdays = schedule_field(L, 2, "weekday");
  spec.days = schedule_field(L, 2, "day");
  spec.months = schedule_field(L, 2, "month");
  auto every = schedule_field(L, 2, "every_minutes");
  if (every.size() > 1) throw std::runtime_error("every_minutes must be one integer");
  if (!every.empty()) {
    if (*every.begin() < 1) throw std::runtime_error("schedule every_minutes must be 1..1440");
    spec.every_minutes = *every.begin();
  }
  spec.validate();
  lua_pushvalue(L, 3);
  int ref = luaL_ref(L, LUA_REGISTRYINDEX);
  this->schedules.emplace_back(Schedule{std::move(name), std::move(spec), ref});
  return 0;
}

int Runtime::service_after(lua_State* L) {
  this->require_grant("schedule");
  this->require_context(false);
  if (this->lifecycle == Lifecycle::UNLOAD) throw std::runtime_error("service timers are unavailable during package unload");
  lua_Integer ms = lua_tointeger(L, 1);
  if (!lua_isinteger(L, 1) || ms < 50 || ms > MAX_SERVICE_TIMER_MS || lua_type(L, 2) != LUA_TFUNCTION) {
    throw std::runtime_error("service_after expects 50..21600000 milliseconds and a function");
  }
  if (this->timers.size() >= MAX_TIMERS) {
    lua_pushnil(L);
    lua_pushliteral(L, "timer quota exceeded");
    return 2;
  }
  lua_pushvalue(L, 2);
  int ref = luaL_ref(L, LUA_REGISTRYINDEX);
  uint64_t id = this->next_timer_id++;
  Context context;
  context.allow_player_actions = false;
  this->timers.emplace(id, Timer{ref, std::move(context), std::chrono::steady_clock::now() + std::chrono::milliseconds(ms), 0, true});
  lua_pushinteger(L, id);
  return 1;
}

PlayerData& Runtime::require_player_data(const char* grant) {
  this->require_grant(grant);
  this->require_context(false);
  auto* data = this->host.player_data(this->id());
  if (!data) throw std::runtime_error("player data is unavailable");
  return *data;
}

int Runtime::player_get(lua_State* L) {
  auto& data = this->require_player_data("players.store");
  const char* key = string_arg(L, 1, 25);
  const char* name = string_arg(L, 2, PlayerData::MAX_KEY_BYTES);
  // Copy into the Lua state before any further C++ object is created: lua_pushlstring can raise.
  const std::string* found = nullptr;
  if (!valid_player_key(key)) throw std::runtime_error("invalid player key");
  auto player = data.players.find(key);
  if (player != data.players.end()) {
    auto value = player->second.values.find(name);
    if (value != player->second.values.end()) found = &value->second;
  }
  if (found) lua_pushlstring(L, found->data(), found->size());
  else lua_pushnil(L);
  return 1;
}

int Runtime::player_set(lua_State* L) {
  auto& data = this->require_player_data("players.store");
  if (this->lifecycle == Lifecycle::UNLOAD) throw std::runtime_error("package_unload storage is read-only");
  const char* key = string_arg(L, 1, 25);
  const char* name = string_arg(L, 2, PlayerData::MAX_KEY_BYTES);
  if (lua_isnoneornil(L, 3)) {
    data.set(key, name, std::nullopt);
  } else {
    data.set(key, name, std::string(string_arg(L, 3, PlayerData::MAX_VALUE_BYTES)));
  }
  return 0;
}

int Runtime::board_add(lua_State* L) {
  auto& data = this->require_player_data("boards");
  if (this->lifecycle == Lifecycle::UNLOAD) throw std::runtime_error("package_unload storage is read-only");
  const char* board = string_arg(L, 1, PlayerData::MAX_KEY_BYTES);
  const char* key = string_arg(L, 2, 25);
  if (!lua_isinteger(L, 3)) throw std::runtime_error("board_add expects an integer amount");
  const char* name = lua_isnoneornil(L, 4) ? "" : string_arg(L, 4, 256);
  lua_Integer score = data.board_add(board, key, lua_tointeger(L, 3), std::string(name).substr(0, PlayerData::MAX_NAME_BYTES));
  lua_pushinteger(L, score);
  return 1;
}

int Runtime::board_get(lua_State* L) {
  auto& data = this->require_player_data("boards");
  const char* board = string_arg(L, 1, PlayerData::MAX_KEY_BYTES);
  const char* key = string_arg(L, 2, 25);
  auto score = data.board_get(board, key);
  if (score) lua_pushinteger(L, *score);
  else lua_pushnil(L);
  return 1;
}

int Runtime::board_top(lua_State* L) {
  auto& data = this->require_player_data("boards");
  const char* board = string_arg(L, 1, PlayerData::MAX_KEY_BYTES + 9);
  if (!lua_isinteger(L, 2)) throw std::runtime_error("board_top expects a board name and a count");
  lua_Integer count = lua_tointeger(L, 2);
  if (count < 1 || count > static_cast<lua_Integer>(PlayerData::MAX_TOP)) throw std::runtime_error("board_top count must be 1..50");
  // Owned by Runtime across Lua allocation errors; cleared after every callback.
  this->current_board_top = data.board_top(board, static_cast<size_t>(count));
  const auto& top = *this->current_board_top;
  lua_createtable(L, static_cast<int>(top.size()), 0);
  for (size_t z = 0; z < top.size(); z++) {
    lua_createtable(L, 0, 4);
    lua_pushinteger(L, static_cast<lua_Integer>(z + 1));
    lua_setfield(L, -2, "rank");
    lua_pushlstring(L, top[z].player_key.data(), top[z].player_key.size());
    lua_setfield(L, -2, "player_key");
    lua_pushlstring(L, top[z].name.data(), top[z].name.size());
    lua_setfield(L, -2, "name");
    lua_pushinteger(L, top[z].score);
    lua_setfield(L, -2, "score");
    lua_rawseti(L, -2, static_cast<lua_Integer>(z + 1));
  }
  this->current_board_top.reset();
  return 1;
}

int Runtime::board_reset(lua_State* L) {
  auto& data = this->require_player_data("boards");
  if (this->lifecycle == Lifecycle::UNLOAD) throw std::runtime_error("package_unload storage is read-only");
  const char* board = string_arg(L, 1, PlayerData::MAX_KEY_BYTES);
  lua_pushboolean(L, data.board_reset(board));
  return 1;
}

int Runtime::meseta_grant(lua_State* L) {
  this->require_grant("economy.meseta");
  this->require_context(false);
  if (this->lifecycle == Lifecycle::UNLOAD) throw std::runtime_error("Meseta grants are unavailable during package unload");
  const char* target = string_arg(L, 1, 25);
  if (!lua_isinteger(L, 2)) throw std::runtime_error("meseta_grant expects an integer amount");
  lua_Integer amount = lua_tointeger(L, 2);
  if (amount < 1 || amount > 1000000000) throw std::runtime_error("meseta_grant amount must be 1..1000000000");
  const char* grant_id = string_arg(L, 3, PlayerData::MAX_GRANT_ID_BYTES);
  if (!*grant_id) throw std::runtime_error("meseta_grant needs a grant ID");
  const char* reason = lua_isnoneornil(L, 4) ? "" : string_arg(L, 4, 100);
  MesetaResult result;
  if (!std::strcmp(target, "self")) {
    // The caller's own account: only in a player callback. Chat mutes do not block rewards;
    // the host still decides online delivery versus queueing.
    if (this->current_context->allow_player_actions && !this->current_context->player_key.empty()) {
      result = this->host.meseta_grant(this->id(), this->current_context->player_key, amount, grant_id, reason);
    }
  } else {
    if (!valid_player_key(target)) throw std::runtime_error("meseta_grant target must be \"self\" or a player key");
    result = this->host.meseta_grant(this->id(), target, amount, grant_id, reason);
  }
  lua_pushboolean(L, result.ok);
  lua_pushstring(L, result.status ? result.status : "unavailable");
  if (result.balance >= 0) lua_pushinteger(L, result.balance);
  else lua_pushnil(L);
  return 3;
}

uint8_t Runtime::caelum_lobby_arg(lua_State* L, int index) {
  // A lobby: its number (0-63: 0-19 the stock lobbies, 20-63 the Caelum extra lobbies) or its menu name. A name
  // the server does not have gives CaelumScenes::NO_LOBBY: objects and music refuse it ("no_such_lobby").
  this->require_grant("caelum.scene");
  this->require_context(false);
  if (lua_type(L, index) == LUA_TSTRING) {
    std::string name = string_arg(L, index, 64);
    return name.empty() ? CaelumScenes::NO_LOBBY : this->host.caelum_lobby_named(name);
  }
  if (!lua_isinteger(L, index)) throw std::runtime_error("expected a lobby number or a lobby name");
  lua_Integer lobby = lua_tointeger(L, index);
  if (lobby < CaelumScenes::FIRST_LOBBY || lobby > CaelumScenes::LAST_LOBBY) {
    throw std::runtime_error("lobby numbers are 0..63");
  }
  return static_cast<uint8_t>(lobby);
}

int Runtime::caelum_object(lua_State* L) {
  uint8_t lobby = this->caelum_lobby_arg(L, 1);
  if (this->lifecycle == Lifecycle::UNLOAD) throw std::runtime_error("Caelum objects cannot be changed during package unload");
  std::string key = string_arg(L, 2, CaelumScenes::MAX_KEY_BYTES);
  if (lua_type(L, 3) != LUA_TTABLE) throw std::runtime_error("caelum_object expects a lobby number, a key, and a table");
  static const std::set<std::string> keys{"model", "model_part", "texture", "motion", "x", "y", "z", "rot_y", "scale", "start_utc_ms",
      "loop", "visible", "rot_x", "rot_z", "attach", "bone", "upright", "follow", "move_motion"};
  lua_pushnil(L);
  while (lua_next(L, 3)) {
    if (lua_type(L, -2) != LUA_TSTRING || !keys.count(string_arg(L, -2, 32))) {
      throw std::runtime_error("unknown caelum_object field");
    }
    lua_pop(L, 1);
  }
  CaelumObjectSpec spec;
  auto text = [&](const char* name, std::string& out, bool required) {
    lua_pushstring(L, name);
    lua_rawget(L, 3);
    if (!lua_isnil(L, -1)) out = string_arg(L, -1, 64);
    else if (required) throw std::runtime_error("caelum_object needs a model file name");
    lua_pop(L, 1);
  };
  auto number = [&](const char* name, double& out) {
    lua_pushstring(L, name);
    lua_rawget(L, 3);
    if (!lua_isnil(L, -1)) {
      if (lua_type(L, -1) != LUA_TNUMBER) throw std::runtime_error("caelum_object positions, rotation and scale must be numbers");
      out = lua_tonumber(L, -1);
    }
    lua_pop(L, 1);
  };
  auto flag = [&](const char* name, bool& out) {
    lua_pushstring(L, name);
    lua_rawget(L, 3);
    if (!lua_isnil(L, -1)) {
      if (lua_type(L, -1) != LUA_TBOOLEAN) throw std::runtime_error("caelum_object loop, visible, upright and follow must be booleans");
      out = lua_toboolean(L, -1);
    }
    lua_pop(L, 1);
  };
  text("model", spec.model, true);
  text("model_part", spec.part, false);
  text("texture", spec.texture, false);
  text("motion", spec.motion, false);
  number("x", spec.x);
  number("y", spec.y);
  number("z", spec.z);
  number("rot_y", spec.rot_y);
  number("scale", spec.scale);
  flag("loop", spec.loop);
  flag("visible", spec.visible);
  text("move_motion", spec.move_motion, false);
  number("rot_x", spec.rot_x);
  number("rot_z", spec.rot_z);
  flag("upright", spec.upright);
  flag("follow", spec.follow);
  // attach: a player's session ID (ctx.client_id, a decimal string, or an integer)
  lua_pushliteral(L, "attach");
  lua_rawget(L, 3);
  if (lua_type(L, -1) == LUA_TSTRING) {
    std::string id = string_arg(L, -1, 20);
    if (id.empty() || id.find_first_not_of("0123456789") != std::string::npos) throw std::runtime_error("attach must be a player's client_id");
    spec.attach = std::stoull(id);
  } else if (lua_isinteger(L, -1) && lua_tointeger(L, -1) > 0) {
    spec.attach = static_cast<uint64_t>(lua_tointeger(L, -1));
  } else if (!lua_isnil(L, -1)) {
    throw std::runtime_error("attach must be a player's client_id");
  }
  lua_pop(L, 1);
  // bone: "root", "head" (node 58), "head_top" (57) or a node number 0-63
  lua_pushliteral(L, "bone");
  lua_rawget(L, 3);
  if (lua_type(L, -1) == LUA_TSTRING) {
    std::string bone = string_arg(L, -1, 16);
    if (bone == "root") spec.bone = -1;
    else if (bone == "head") spec.bone = CEXT_HEAD_BONE;
    else if (bone == "head_top") spec.bone = CEXT_HEAD_BONE - 1;
    else throw std::runtime_error("bone must be \"root\", \"head\", \"head_top\" or a node number 0-63");
  } else if (lua_isinteger(L, -1)) {
    spec.bone = static_cast<int>(std::clamp<lua_Integer>(lua_tointeger(L, -1), -2, 64));   // out of range: invalid_value
  } else if (!lua_isnil(L, -1)) {
    throw std::runtime_error("bone must be \"root\", \"head\", \"head_top\" or a node number 0-63");
  }
  lua_pop(L, 1);
  lua_pushliteral(L, "start_utc_ms");
  lua_rawget(L, 3);
  if (!lua_isnil(L, -1)) {
    if (!lua_isinteger(L, -1) || lua_tointeger(L, -1) < 0) throw std::runtime_error("start_utc_ms must be a non-negative integer");
    spec.start_ms = lua_tointeger(L, -1);
  }
  lua_pop(L, 1);
  auto result = this->host.caelum_object(this->id(), lobby, key, spec);
  lua_pushboolean(L, result.ok);
  if (result.reason) lua_pushstring(L, result.reason);
  else lua_pushnil(L);
  return 2;
}

int Runtime::caelum_remove(lua_State* L) {
  uint8_t lobby = this->caelum_lobby_arg(L, 1);
  if (this->lifecycle == Lifecycle::UNLOAD) throw std::runtime_error("Caelum objects cannot be changed during package unload");
  const char* key = string_arg(L, 2, CaelumScenes::MAX_KEY_BYTES);
  lua_pushboolean(L, this->host.caelum_remove(this->id(), lobby, key));
  return 1;
}

int Runtime::caelum_music(lua_State* L) {
  uint8_t lobby = this->caelum_lobby_arg(L, 1);
  if (this->lifecycle == Lifecycle::UNLOAD) throw std::runtime_error("Caelum music cannot be changed during package unload");
  CaelumResult result;
  if (lua_isnoneornil(L, 2)) {
    result = this->host.caelum_music(this->id(), lobby, nullptr);
  } else {
    CaelumMusicSpec spec;
    spec.file = string_arg(L, 2, 64);
    if (!lua_isnoneornil(L, 3)) {
      if (lua_type(L, 3) != LUA_TTABLE) throw std::runtime_error("caelum_music options must be a table");
      lua_pushnil(L);
      while (lua_next(L, 3)) {
        if (lua_type(L, -2) != LUA_TSTRING) throw std::runtime_error("unknown caelum_music option");
        std::string name = string_arg(L, -2, 32);
        if (name == "loop") {
          if (lua_type(L, -1) != LUA_TBOOLEAN) throw std::runtime_error("caelum_music loop must be a boolean");
          spec.loop = lua_toboolean(L, -1);
        } else if (name == "start_utc_ms") {
          if (!lua_isinteger(L, -1) || lua_tointeger(L, -1) < 0) throw std::runtime_error("start_utc_ms must be a non-negative integer");
          spec.start_ms = lua_tointeger(L, -1);
        } else {
          throw std::runtime_error("unknown caelum_music option");
        }
        lua_pop(L, 1);
      }
    }
    result = this->host.caelum_music(this->id(), lobby, &spec);
  }
  lua_pushboolean(L, result.ok);
  if (result.reason) lua_pushstring(L, result.reason);
  else lua_pushnil(L);
  return 2;
}

int Runtime::caelum_clear(lua_State* L) {
  uint8_t lobby = this->caelum_lobby_arg(L, 1);
  if (this->lifecycle == Lifecycle::UNLOAD) throw std::runtime_error("Caelum objects cannot be changed during package unload");
  lua_pushboolean(L, this->host.caelum_clear(this->id(), lobby));
  return 1;
}

int Runtime::caelum_listeners(lua_State* L) {
  uint8_t lobby = this->caelum_lobby_arg(L, 1);
  int count = this->host.caelum_listeners(lobby);
  if (count < 0) lua_pushnil(L);
  else lua_pushinteger(L, count);
  return 1;
}

int Runtime::caelum_lobby(lua_State* L) {
  // {number, name, listeners} of a lobby given by number or name, or nil when the server has no such lobby.
  uint8_t lobby = this->caelum_lobby_arg(L, 1);
  auto name = lobby == CaelumScenes::NO_LOBBY ? std::nullopt : this->host.caelum_lobby_name(lobby);
  if (!name) {
    lua_pushnil(L);
    return 1;
  }
  int listeners = this->host.caelum_listeners(lobby);
  lua_createtable(L, 0, 3);
  lua_pushinteger(L, lobby);
  lua_setfield(L, -2, "number");
  lua_pushlstring(L, name->data(), name->size());
  lua_setfield(L, -2, "name");
  lua_pushinteger(L, listeners < 0 ? 0 : listeners);
  lua_setfield(L, -2, "listeners");
  return 1;
}

int Runtime::position(lua_State* L) {
  // The caller's own position (lobby.read): {x, y, z, floor, is_game, lobby_number, lobby_name} or nil.
  this->require_grant("lobby.read");
  this->require_context(false);
  if (lua_gettop(L)) throw std::runtime_error("position expects no arguments");
  if (!this->current_context->allow_player_actions || !this->host.authorized(*this->current_context)) {
    lua_pushnil(L);
    return 1;
  }
  auto where = this->host.player_position(*this->current_context);
  if (!where) {
    lua_pushnil(L);
    return 1;
  }
  lua_createtable(L, 0, 7);
  lua_pushnumber(L, where->x);
  lua_setfield(L, -2, "x");
  lua_pushnumber(L, where->y);
  lua_setfield(L, -2, "y");
  lua_pushnumber(L, where->z);
  lua_setfield(L, -2, "z");
  lua_pushinteger(L, where->floor);
  lua_setfield(L, -2, "floor");
  lua_pushboolean(L, where->is_game);
  lua_setfield(L, -2, "is_game");
  if (where->lobby_number != CaelumScenes::NO_LOBBY) {
    lua_pushinteger(L, where->lobby_number);
    lua_setfield(L, -2, "lobby_number");
    lua_pushlstring(L, where->lobby_name.data(), where->lobby_name.size());
    lua_setfield(L, -2, "lobby_name");
  }
  return 1;
}

} // namespace teaserv::extensions
