#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "Scene.hh"
#include "Announcements.hh"
#include "CommandArguments.hh"
#include "CommandCatalog.hh"
#include "Settings.hh"
#include "LobbyView.hh"
#include "GameLifecycle.hh"

struct lua_State;
struct lua_Debug;

namespace teaserv::extensions {

// The runtime has no dependency on Newserv types or wire formats. A context is a
// snapshot; the adapter must revalidate its session, lobby, and permission on use.
struct Context {
  uint64_t client_id = 0;
  int64_t lobby_id = 0;
  std::string player_name;
  std::string permission = "everyone";
  std::string client_version = "";
  bool is_game = false;
  bool allow_player_actions = true;
};

struct Host {
  virtual ~Host() = default;
  virtual bool authorized(const Context& context) const = 0;
  virtual void reply(const Context& context, const std::string& text) = 0;
  virtual void broadcast(const Context& context, const std::string& text) = 0;
  virtual AnnouncementResult announce(const std::string&, const std::string&) { return {}; }
  virtual bool whisper(const Context& context, uint64_t target, const std::string& text) = 0;
  virtual void log(const std::string& extension_id, const std::string& text) = 0;
  virtual bool command_reserved(const std::string& name) const = 0;
  virtual std::optional<LobbyView> lobby_view(const Context&) const { return std::nullopt; }
  virtual std::optional<std::vector<CatalogCommand>> command_catalog(const Context&) const { return std::nullopt; }
  virtual SceneCapabilities scene_capabilities(const Context&) const { return {}; }
  virtual SceneAssetSupport scene_asset_support(const Context&, const std::string&, const std::string&) const { return {}; }
  virtual SceneResult scene_spawn(const Context&, const std::string&, const std::string&, const std::string&, const std::string&, uint32_t) {
    return {false, "native scene backend is unavailable"};
  }
  virtual SceneResult scene_remove(const Context&, const std::string&, const std::string&) {
    return {false, "native scene backend is unavailable"};
  }
  virtual SceneResult scene_event(const Context&, const std::string&, int) {
    return {false, "lobby event backend is unavailable"};
  }
};

struct Package {
  std::string id;
  std::string version;
  std::string source;
  std::set<std::string> grants;
  std::map<std::string, std::string> storage;
  uint32_t storage_version = 0;
  uint32_t target_storage_version = 0;
  Settings settings = {};
};

struct RuntimeDiagnostics {
  uint64_t callbacks = 0;
  size_t callback_errors = 0;
  size_t peak_memory_bytes = 0;
  size_t storage_bytes = 0;
  std::string last_error;
  uint64_t last_callback_us = 0;
  uint64_t peak_callback_us = 0;
  uint64_t total_callback_us = 0;
  uint64_t over_budget_callbacks = 0;
};

struct CommandDetails {
  std::string name;
  std::string permission;
  uint32_t cooldown_ms;
  std::string usage;
  std::string help;
};

class Runtime {
public:
  static constexpr size_t MAX_SOURCE_BYTES = 256 * 1024;
  static constexpr size_t MAX_STORAGE_BYTES = 64 * 1024;
  static constexpr size_t MAX_MEMORY_BYTES = 4 * 1024 * 1024;
  static constexpr size_t MAX_TIMERS = 32;
  static constexpr size_t MAX_REGISTRATIONS = 32;
  static constexpr uint32_t MAX_STORAGE_VERSION = 0x7FFFFFFF;
  static constexpr uint32_t CALLBACK_TIME_BUDGET_MS = 10;

  Runtime(Host& host, Package package);
  ~Runtime();
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;

  const std::string& id() const;
  const std::string& version() const;
  bool enabled() const;
  size_t memory_bytes() const;
  size_t timer_count() const;
  std::vector<std::string> command_names() const;
  bool has_command(const std::string& name) const;
  // Returns true if this package owns the command, even if it cannot execute it.
  bool dispatch(const std::string& name, const std::string& args, Context context);
  void emit(const std::string& event, Context context);
  void emit_game(const std::string& event, const GameSnapshot& snapshot);
  void emit_quest_acknowledgement(const QuestLoadAcknowledgement& snapshot);
  // Explicit activation only: constructing/checking a candidate emits nothing.
  void activate(const std::string& reason);
  void deactivate(const std::string& reason);
  void tick();
  void cancel_client(uint64_t client_id);
  const std::map<std::string, std::string>& storage() const;
  bool storage_dirty() const;
  uint32_t storage_version() const;
  void storage_saved();
  RuntimeDiagnostics diagnostics() const;
  std::vector<CommandDetails> command_details() const;
  const std::set<std::string>& grants() const;
  const Settings& settings() const;

private:
  struct Command {
    int ref;
    std::string permission;
    uint32_t cooldown_ms;
    std::map<uint64_t, std::chrono::steady_clock::time_point> cooldowns;
    std::optional<CommandArguments> arguments;
    std::string help;
  };
  struct Timer {
    int ref;
    Context context;
    std::chrono::steady_clock::time_point due;
    uint32_t interval_ms = 0; // Zero is a one-shot; repeating timers retain their slot while running.
  };

  Host& host;
  Package package;
  lua_State* state = nullptr;
  size_t allocated = 0;
  size_t peak_allocated = 0;
  uint64_t callbacks = 0;
  uint64_t last_callback_us = 0;
  uint64_t peak_callback_us = 0;
  uint64_t total_callback_us = 0;
  uint64_t over_budget_callbacks = 0;
  std::string last_error;
  bool active = true;
  bool staging = true;
  bool dirty = false;
  bool migrating = false;
  bool activated = false;
  bool deactivated = false;
  enum class Lifecycle { NONE, LOAD, UNLOAD };
  Lifecycle lifecycle = Lifecycle::NONE;
  int migration_ref = 0;
  size_t errors = 0;
  size_t instructions = 0;
  size_t api_calls = 0;
  size_t output_bytes = 0;
  std::chrono::steady_clock::time_point deadline;
  const Context* current_context = nullptr;
  // Native invocation scope, never derived from a mutable Lua context table.
  bool current_event_authority = false;
  int current_ref = 0;
  const std::string* current_args = nullptr;
  const ParsedCommandArguments* current_parsed_args = nullptr;
  // Kept outside the Lua C stack so an allocation error cannot skip a C++
  // snapshot destructor. Cleared after every protected callback, including OOM.
  std::optional<LobbyView> current_lobby_view;
  std::optional<std::vector<CatalogCommand>> current_command_catalog;
  const GameSnapshot* current_game = nullptr;
  const QuestLoadAcknowledgement* current_quest_acknowledgement = nullptr;
  void emit_game_observation(const std::string& event, const GameSnapshot& snapshot, const QuestLoadAcknowledgement* acknowledgement);
  std::map<std::string, Command> commands;
  std::map<std::string, std::vector<int>> events;
  std::map<uint64_t, Timer> timers;
  uint64_t next_timer_id = 1;
  char error_buffer[1024] = {};

  void require_grant(const char* grant) const;
  void require_context(bool require_live_player = true);
  void account_output(size_t size);
  void validate_storage() const;
  void require_storage_access();
  bool call(int ref, const Context& context, const std::string& args, const ParsedCommandArguments* parsed = nullptr,
      bool event_authority = false);
  void reset_budget();
  void record_error(const std::string& error);
  void stop();
  void emit_lifecycle(Lifecycle event, const std::string& reason);
  static Runtime& self(lua_State* L);
  static void* allocate(void* user, void* ptr, size_t old_size, size_t new_size);
  static void instruction_hook(lua_State* L, lua_Debug* debug);
  static int initialize(lua_State* L);
  static int invoke(lua_State* L);
  // C++ exceptions are converted to Lua errors only after C++ locals unwind.
  template <int (Runtime::*Fn)(lua_State*)>
  static int api(lua_State* L);
  int register_command(lua_State* L);
  int register_event(lua_State* L);
  int after(lua_State* L);
  int every(lua_State* L);
  int schedule_timer(lua_State* L, bool repeating);
  int cancel(lua_State* L);
  int reply(lua_State* L);
  int broadcast(lua_State* L);
  int announce(lua_State* L);
  int whisper(lua_State* L);
  int log(lua_State* L);
  int storage_get(lua_State* L);
  int storage_set(lua_State* L);
  int register_migration(lua_State* L);
  int setting(lua_State* L);
  int lobby(lua_State* L);
  int command_list(lua_State* L);
  int scene_capabilities(lua_State* L);
  int scene_asset_support(lua_State* L);
  int scene_spawn(lua_State* L);
  int scene_remove(lua_State* L);
  int scene_event(lua_State* L);
};

} // namespace teaserv::extensions
