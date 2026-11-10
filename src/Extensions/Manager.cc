#include "Manager.hh"
#include "MessageText.hh"
#include "NativeAssets.hh"
#include "LiveMembership.hh"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <phosg/JSON.hh>

#include "../ChatCommands.hh"
#include "../Client.hh"
#include "../GameServer.hh"
#include "../Compression.hh"
#include "../Loggers.hh"
#include "../SendCommands.hh"
#include "../ServerState.hh"
#include "../Text.hh"

#ifdef _WIN32
#include <windows.h>
#endif

namespace teaserv::extensions {
namespace {

bool valid_id(const std::string& id) {
  return !id.empty() && id.size() <= 64 && std::all_of(id.begin(), id.end(), [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
  });
}

std::string diagnostic_text(const std::string& text) {
  std::string line = text.substr(0, 1024);
  for (auto& ch : line) {
    if (static_cast<unsigned char>(ch) < 0x20 || ch == 0x7F) ch = ' ';
  }
  return line;
}

std::string name_list(const std::set<std::string>& names) {
  std::string result;
  for (const auto& name : names) {
    if (!result.empty()) result += ", ";
    result += name;
  }
  return result.empty() ? "(none)" : result;
}

std::filesystem::path contained_path(const std::filesystem::path& root, const std::filesystem::path& relative) {
  if (relative.is_absolute()) {
    throw std::runtime_error("extension paths must be relative");
  }
  auto base = std::filesystem::weakly_canonical(root);
  auto path = std::filesystem::weakly_canonical(root / relative);
  auto [base_end, path_end] = std::mismatch(base.begin(), base.end(), path.begin(), path.end());
  (void)path_end;
  if (base_end != base.end() || path == base) {
    throw std::runtime_error("extension path escapes its directory");
  }
  return path;
}

std::string read_bounded(const std::filesystem::path& path, size_t limit) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("cannot read " + path.string());
  }
  std::string result(limit + 1, '\0');
  file.read(result.data(), result.size());
  result.resize(file.gcount());
  if (file.bad() || result.size() > limit) {
    throw std::runtime_error("file is unreadable or too large: " + path.string());
  }
  return result;
}

std::filesystem::path storage_path(const std::string& id) {
  return contained_path("system/extensions-state", id + ".json");
}

struct StoredData {
  std::map<std::string, std::string> values;
  uint32_t version = 0;
};

int64_t setting_integer(const phosg::JSON& value) {
  if (!value.is_int()) throw std::runtime_error("setting bounds and integer values must be JSON integers");
  return value.as_int();
}

SettingValue setting_value(const phosg::JSON& value) {
  if (value.is_bool()) return value.as_bool();
  if (value.is_int()) return value.as_int();
  if (value.is_string()) return value.as_string();
  throw std::runtime_error("setting values must be booleans, integers, or strings");
}

Settings read_settings(const phosg::JSON& manifest, const phosg::JSON& entry) {
  auto definitions = manifest.get("settings", phosg::JSON::dict());
  auto overrides = entry.get("Settings", phosg::JSON::dict());
  if (definitions.as_dict().size() > 64 || overrides.as_dict().size() > 64) {
    throw std::runtime_error("at most 64 extension settings are allowed");
  }
  SettingsSchema schema;
  for (const auto& [name, json] : definitions.as_dict()) {
    SettingDefinition definition;
    definition.type = json->at("type").as_string();
    for (const auto& [key, value] : json->as_dict()) {
      if (key == "type") continue;
      if (key == "default") definition.default_value = setting_value(*value);
      else if (key == "required") definition.required = value->as_bool();
      else if ((key == "min" || key == "max") && definition.type == "integer") {
        (key == "min" ? definition.min : definition.max) = setting_integer(*value);
      } else if (key == "max_bytes" && definition.type == "string") {
        auto max_bytes = setting_integer(*value);
        if (max_bytes < 1 || max_bytes > 1024) throw std::runtime_error("setting max_bytes must be 1..1024");
        definition.max_bytes = max_bytes;
      } else {
        throw std::runtime_error("unknown or inapplicable setting schema option");
      }
    }
    schema.emplace(name, std::move(definition));
  }
  std::map<std::string, SettingValue> supplied;
  for (const auto& [name, value] : overrides.as_dict()) supplied.emplace(name, setting_value(*value));
  return resolve_settings(schema, supplied);
}

StoredData read_storage(const std::string& id) {
  auto path = storage_path(id);
  if (!std::filesystem::exists(path)) {
    return {};
  }
  auto json = phosg::JSON::parse(read_bounded(path, 512 * 1024));
  if (json.at("schema_version").as_int() != 1) {
    throw std::runtime_error("unsupported extension storage schema");
  }
  auto version = json.get_int("data_version", 0);
  if (version < 0 || version > Runtime::MAX_STORAGE_VERSION) {
    throw std::runtime_error("invalid extension storage data_version");
  }
  StoredData result{{}, static_cast<uint32_t>(version)};
  for (const auto& [key, value] : json.at("values").as_dict()) {
    result.values.emplace(key, value->as_string());
  }
  return result;
}

void write_storage(const std::string& id, const std::map<std::string, std::string>& storage, uint32_t version) {
  std::filesystem::create_directories("system/extensions-state");
  auto path = storage_path(id);
  auto temporary = contained_path("system/extensions-state", id + ".json.tmp");
  auto values = phosg::JSON::dict();
  for (const auto& [key, value] : storage) {
    values.emplace(key, phosg::JSON(value));
  }
  auto json = phosg::JSON::dict({{"schema_version", 1}, {"data_version", static_cast<int64_t>(version)}, {"values", std::move(values)}});
  {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file) {
      throw std::runtime_error("cannot create extension storage temporary file");
    }
    file << json.serialize();
    file.close();
    if (!file) {
      throw std::runtime_error("cannot write extension storage");
    }
  }
#ifdef _WIN32
  if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    throw std::runtime_error("cannot replace extension storage file");
  }
#else
  std::filesystem::rename(temporary, path);
#endif
}

// Extension messages are plain text. Strip protocol control characters and
// truncate conservatively; let Newserv perform each recipient's text encoding.
std::string message_text(const std::string& text) {
  // send_text_message applies add_color. Escape its $, #, and % syntax so
  // formatting cannot be introduced by extension text or player input.
  return remove_color(plain_message_text(text));
}

bool native_card_lobby(std::shared_ptr<Client> client, std::shared_ptr<Lobby> lobby) {
  // Deliberately support the canonical CARD layouts only, without duplicating
  // SendCommands' custom/overridden lobby-number selection logic.
  return lobby && !lobby->is_game() && lobby->is_ep3() && lobby->block >= 16 && lobby->block <= 20 &&
      client->override_lobby_number == 0x80;
}

GameAdmission game_admission(std::shared_ptr<ServerState> s, std::shared_ptr<Client> client, std::shared_ptr<Lobby> lobby) {
  if (!s || !client || !lobby) return {};
  auto registered_lobby = s->id_to_lobby.find(lobby->lobby_id);
  auto registered_client = s->client_for_id.find(client->id);
  bool current_member = registered_lobby != s->id_to_lobby.end() && registered_lobby->second == lobby &&
      registered_client != s->client_for_id.end() && registered_client->second == client && client->lobby.lock() == lobby &&
      client->lobby_client_id < lobby->clients.size() && lobby->clients[client->lobby_client_id] == client;
  return GameAdmission{
      .is_game = lobby->is_game() && !lobby->check_flag(Lobby::Flag::IS_CLIENT_CUSTOMIZATION),
      .spectator = lobby->check_flag(Lobby::Flag::IS_SPECTATOR_TEAM),
      .playback = static_cast<bool>(lobby->battle_player),
      .authenticated = static_cast<bool>(client->login),
      .connected = client->channel->connected(),
      .proxy = static_cast<bool>(client->proxy_session),
      .local_session = !s->is_replay && client->server_behavior == ServerBehavior::GAME_SERVER && !is_patch(client->version()) &&
          client->version() != Version::UNKNOWN && (!is_v4(client->version()) || client->bb_connection_phase >= 4),
      .current_member = current_member};
}

GameSnapshot game_snapshot(std::shared_ptr<Lobby> lobby) {
  GameSnapshot snapshot{lobby->lobby_id, lobby->name, static_cast<uint8_t>(lobby->episode),
      static_cast<uint8_t>(lobby->mode), static_cast<uint8_t>(lobby->difficulty)};
  if (lobby->quest) {
    snapshot.quest_number = lobby->quest->meta.quest_number;
    snapshot.quest_joinable = lobby->quest->meta.joinable;
  }
  validate_game_snapshot(snapshot);
  return snapshot;
}

} // namespace

Manager::Manager(std::shared_ptr<ServerState> server)
    : server(server), io_context(server->io_context), timer(*this->io_context) {}

std::shared_ptr<Manager> Manager::create(std::shared_ptr<ServerState> server) {
  if (server->is_replay) {
    return nullptr;
  }
  auto manager = std::shared_ptr<Manager>(new Manager(server));
  try {
    manager->reload_with_reason("startup");
  } catch (const std::exception& e) {
    manager->log("host", std::string("No extensions loaded: ") + e.what());
  }
  manager->schedule_tick();
  return manager;
}

Manager::~Manager() {
  this->timer.cancel();
  for (auto it = this->package_order.rbegin(); it != this->package_order.rend(); ++it) {
    auto runtime = this->packages.find(*it);
    if (runtime != this->packages.end()) this->notify_lifecycle(*runtime->second, false, "shutdown");
  }
  try {
    this->flush_storage();
  } catch (const std::exception& e) {
    this->log("host", e.what());
  }
}

void Manager::reload() {
  this->reload_with_reason("reload");
}

void Manager::reload_with_reason(const std::string& reason) {
  try {
    this->stage_config(true, reason);
    this->last_reload_error.clear();
  } catch (const std::exception& e) {
    this->last_reload_error = diagnostic_text(e.what());
    throw;
  }
}

std::deque<std::string> Manager::check() {
  try {
    auto result = this->stage_config(false);
    this->last_check_error.clear();
    return result;
  } catch (const std::exception& e) {
    this->last_check_error = diagnostic_text(e.what());
    throw;
  }
}

void Manager::notify_lifecycle(Runtime& runtime, bool loading, const std::string& reason) {
  try {
    if (loading) runtime.activate(reason);
    else runtime.deactivate(reason);
  } catch (const std::exception& e) {
    this->log(runtime.id(), std::string("Package lifecycle host failure: ") + e.what());
  } catch (...) {
    this->log(runtime.id(), "Unknown package lifecycle host failure");
  }
}

std::deque<std::string> Manager::stage_config(bool activate, const std::string& reason) {
  auto s = this->server.lock();
  if (!s) {
    throw std::runtime_error("server has shut down");
  }
  // Reload uses the current config snapshot; run 'reload config' first after
  // editing config.json. Package manifests/scripts are always read from disk.
  if (activate) this->flush_storage();
  auto config = s->data->config_json->get("Extensions", phosg::JSON::dict());
  std::map<std::string, std::unique_ptr<Runtime>> staged;
  std::map<std::string, std::set<std::string>> staged_dependencies;
  std::vector<std::string> staged_order;
  SceneService::Catalog staged_assets;
  auto scene_config = config.get("Scenes", phosg::JSON::dict());
  bool enable_scenes = config.get_bool("Enabled", false) && scene_config.get_bool("Enabled", false);
  bool enable_models = enable_scenes && scene_config.get_bool("ExperimentalModels", false);
  std::set<std::string> names;
  this->staging_names = &names;
  try {
    if (config.get_bool("Enabled", false)) {
      const auto& entries = config.at("Packages").as_list();
      if (entries.size() > 16) {
        throw std::runtime_error("at most 16 extension packages may be enabled");
      }
      for (const auto& entry : entries) {
        auto id = entry->at("id").as_string();
        if (!valid_id(id) || staged.count(id)) {
          throw std::runtime_error("invalid or duplicate configured extension ID");
        }
        auto root = contained_path("system/extensions", id);
        auto manifest = phosg::JSON::parse(read_bounded(contained_path(root, "extension.json"), 16384));
        if (manifest.at("id").as_string() != id || manifest.at("api_version").as_int() != 1) {
          throw std::runtime_error("manifest ID mismatch or unsupported API version: " + id);
        }
        auto dependencies = manifest.get("dependencies", phosg::JSON::list());
        for (const auto& dependency : dependencies.as_list()) {
          if (!staged.count(dependency->as_string())) {
            throw std::runtime_error("dependencies must be enabled earlier in Packages: " + id);
          }
          staged_dependencies[id].emplace(dependency->as_string());
        }
        static const std::set<std::string> known_grants{
            "commands", "commands.read", "events", "timers", "storage", "chat.reply", "chat.broadcast", "chat.whisper", "chat.announce", "scene.media", "scene.event", "lobby.read"};
        std::set<std::string> grants;
        for (const auto& grant : entry->at("grants").as_list()) {
          auto name = grant->as_string();
          if (!known_grants.count(name) || !grants.emplace(name).second) {
            throw std::runtime_error("unknown or duplicate extension grant: " + name);
          }
        }
        std::set<std::string> requested;
        for (const auto& permission : manifest.at("permissions").as_list()) {
          if (!grants.count(permission->as_string())) {
            throw std::runtime_error("requested extension permission was not granted: " + id);
          }
          requested.emplace(permission->as_string());
        }
        auto assets = manifest.get("assets", phosg::JSON::dict());
        if (!assets.as_dict().empty() && !requested.count("scene.media")) {
          throw std::runtime_error("native asset catalogs require the scene.media grant");
        }
        for (const auto& [name, definition] : assets.as_dict()) {
          if (!valid_id(name)) {
            throw std::runtime_error("invalid scene asset name");
          }
          if (staged_assets.size() >= SceneService::MAX_ASSETS) {
            throw std::runtime_error("at most 64 native scene assets may be loaded");
          }
          uint32_t texture_index = 0x100 + staged_assets.size();
          auto load_variant = [&](const phosg::JSON& variant, bool fallback) {
            SceneAsset asset;
            if (fallback && !variant.get("fallback", phosg::JSON()).is_null()) {
              throw std::runtime_error("fallback variants cannot contain another fallback");
            }
            auto profile_name = variant.get_string("media_profile", "original");
            if (profile_name != "original" && profile_name != "pc-port") {
              throw std::runtime_error("asset media_profile must be original or pc-port");
            }
            if (fallback && profile_name != "original") {
              throw std::runtime_error("fallback media_profile must be original");
            }
            auto media_profile = profile_name == "pc-port"
                ? Episode3::LobbyMediaProfile::PC_PORT : Episode3::LobbyMediaProfile::ORIGINAL;
            auto load_media = [&](const std::string& field, uint32_t type) {
              auto path = contained_path(root, variant.at(field).as_string());
              auto data = prepare_native_media(read_bounded(path, media_profile == Episode3::LobbyMediaProfile::PC_PORT
                          ? Episode3::LOBBY_MEDIA_PC_DECOMPRESSED : 512 * 1024),
                  path.string(), type, texture_index, media_profile);
              auto compatibility = Episode3::lobby_media_compatibility(data->size(),
                  prs_decompress_size(*data, Episode3::LOBBY_MEDIA_PC_DECOMPRESSED));
              asset.compatibility.gc_ok &= compatibility.gc_ok;
              asset.compatibility.pc_ok &= compatibility.pc_ok;
              return data;
            };
            asset.texture = load_media("texture", 1);
            auto model = variant.get("model", phosg::JSON());
            auto animation = variant.get("animation", phosg::JSON());
            if ((!model.is_null() && !model.is_string()) || (!animation.is_null() && !animation.is_string())) {
              throw std::runtime_error("model and animation asset paths must be strings");
            }
            if (model.is_string()) {
              asset.model = load_media("model", 2);
              const auto& builds = variant.at("model_builds").as_list();
              if (builds.empty() || builds.size() > 16) {
                throw std::runtime_error("model assets require 1..16 exact native client builds");
              }
              for (const auto& build : builds) {
                auto version = build->as_string();
                if (version.size() != 4 || !version.starts_with("3S") ||
                    !std::all_of(version.begin(), version.end(), [](unsigned char ch) {
                      return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z');
                    })) {
                  throw std::runtime_error("native models require exact retail Episode 3 build identifiers");
                }
                asset.model_builds.emplace(specific_version_for_str(version));
              }
            }
            if (animation.is_string()) {
              asset.animation = load_media("animation", 3);
            }
            return asset;
          };
          auto asset = load_variant(*definition, false);
          auto fallback = definition->get("fallback", phosg::JSON());
          if (!fallback.is_null()) {
            asset.fallback = std::make_shared<const SceneAsset>(load_variant(fallback, true));
          }
          staged_assets.emplace(SceneService::AssetKey{id, name}, std::move(asset));
        }
        auto source_path = contained_path(root, manifest.at("entry").as_string());
        if (source_path.extension() != ".lua") {
          throw std::runtime_error("entry script must have a .lua extension");
        }
        auto old = this->packages.find(id);
        auto stored = old == this->packages.end() ? read_storage(id) : StoredData{old->second->storage(), old->second->storage_version()};
        auto target_version = manifest.get_int("storage_version", 0);
        if (target_version < 0 || target_version > Runtime::MAX_STORAGE_VERSION) {
          throw std::runtime_error("manifest storage_version must be 0..2147483647");
        }
        Package package{id, manifest.at("version").as_string(), read_bounded(source_path, Runtime::MAX_SOURCE_BYTES),
            std::move(requested), std::move(stored.values), stored.version, static_cast<uint32_t>(target_version),
            read_settings(manifest, *entry)};
        if (package.version.empty() || package.version.size() > 64) {
          throw std::runtime_error("invalid extension version");
        }
        auto runtime = std::make_unique<Runtime>(*this, std::move(package));
        for (const auto& name : runtime->command_names()) {
          names.emplace(name);
        }
        staged.emplace(id, std::move(runtime));
        staged_order.emplace_back(id);
      }
    }
  } catch (...) {
    this->staging_names = nullptr;
    throw;
  }
  this->staging_names = nullptr;
  SceneService validation;
  validation.replace_catalog(staged_assets); // All asset checks precede active-state changes.
  if (!activate) {
    std::deque<std::string> report{std::format(
        "Validation passed: {} packages, {} commands, {} assets. Active state unchanged.",
        staged.size(), names.size(), staged_assets.size())};
    for (const auto& [id, runtime] : staged) {
      report.emplace_back(std::format("Candidate {} {}: {} commands, storage v{}, {} Lua bytes", id, runtime->version(),
          runtime->command_names().size(), runtime->storage_version(), runtime->memory_bytes()));
    }
    return report;
  }
  for (const auto& [id, runtime] : this->packages) {
    (void)runtime;
    this->cleanup_scene_owner(id);
  }
  this->scenes.replace_catalog(std::move(staged_assets));
  this->scenes_enabled = enable_scenes;
  this->experimental_models = enable_models;
  this->packages.swap(staged);
  this->dependencies.swap(staged_dependencies);
  this->package_order.swap(staged_order);
  // Candidates are now active. Old unload handlers are read-only, so they cannot
  // overwrite the storage copies used by the replacement runtimes.
  for (auto it = staged_order.rbegin(); it != staged_order.rend(); ++it) {
    this->notify_lifecycle(*staged.at(*it), false, "reload");
  }
  for (const auto& id : this->package_order) this->notify_lifecycle(*this->packages.at(id), true, reason);
  try {
    this->refresh_baseline();
    this->sync_all_scenes();
    this->dirty_scene_lobbies.clear();
  } catch (const std::exception& e) {
    // Activation has committed. Normal periodic scene reconciliation retries;
    // reporting a rejected reload here would incorrectly imply rollback.
    this->log("host", std::string("Packages activated; scene synchronization will retry: ") + e.what());
  }
  this->log("host", std::format("Activated {} extension packages", this->packages.size()));
  return {};
}

void Manager::disable(const std::string& id) {
  if (!this->packages.count(id)) {
    throw std::runtime_error("no such loaded extension");
  }
  for (const auto& [dependent, required] : this->dependencies) {
    if (this->packages.count(dependent) && required.count(id)) {
      throw std::runtime_error("unload dependent extension first: " + dependent);
    }
  }
  this->flush_storage();
  this->cleanup_scene_owner(id);
  this->flush_scene_updates(true);
  this->notify_lifecycle(*this->packages.at(id), false, "disable");
  this->packages.erase(id);
  this->dependencies.erase(id);
  std::erase(this->package_order, id);
  this->log(id, "Unloaded; commands and timers removed. A reload can enable it again.");
}

std::deque<std::string> Manager::describe() const {
  std::deque<std::string> lines;
  for (const auto& [id, runtime] : this->packages) {
    lines.emplace_back(std::format("{} {}: {}, {} commands, {} timers, {} Lua bytes, storage v{} ({} keys)", id, runtime->version(),
        runtime->enabled() ? "enabled" : "faulted", runtime->command_names().size(), runtime->timer_count(), runtime->memory_bytes(),
        runtime->storage_version(), runtime->storage().size()));
  }
  if (lines.empty()) {
    lines.emplace_back("No extensions loaded (opt in with Extensions in config.json)");
  }
  lines.emplace_back(std::format("Native scenes: {}, {} owned media objects; experimental models: {}",
      this->scenes_enabled ? "enabled" : "disabled", this->scenes.object_count(), this->experimental_models ? "enabled" : "disabled"));
  if (!this->last_reload_error.empty()) lines.emplace_back("Last reload failure: " + this->last_reload_error);
  if (!this->last_check_error.empty()) lines.emplace_back("Last check failure: " + this->last_check_error);
  return lines;
}

std::deque<std::string> Manager::info(const std::string& id) const {
  if (!valid_id(id)) throw std::runtime_error("invalid extension ID");
  auto it = this->packages.find(id);
  if (it == this->packages.end()) throw std::runtime_error("no such loaded extension: " + id);
  const auto& runtime = *it->second;
  auto diagnostics = runtime.diagnostics();
  auto dependencies = this->dependencies.find(id);
  std::deque<std::string> lines{
      std::format("Extension {} {}: {}", id, runtime.version(), runtime.enabled() ? "enabled" : "faulted"),
      std::format("Lua: {} current, {} peak, {} limit bytes; timers: {}/{}", runtime.memory_bytes(),
          diagnostics.peak_memory_bytes, Runtime::MAX_MEMORY_BYTES, runtime.timer_count(), Runtime::MAX_TIMERS),
      std::format("Storage: v{}, {} keys, {}/{} bytes; pending save: {}", runtime.storage_version(),
          runtime.storage().size(), diagnostics.storage_bytes, Runtime::MAX_STORAGE_BYTES, runtime.storage_dirty() ? "yes" : "no"),
      std::format("Callbacks: {}; failures: {} (fault limit: 3)", diagnostics.callbacks, diagnostics.callback_errors),
      std::format("Callback time (us): last={}; peak={}; total={}; over {} ms={}",
          diagnostics.last_callback_us, diagnostics.peak_callback_us, diagnostics.total_callback_us,
          Runtime::CALLBACK_TIME_BUDGET_MS, diagnostics.over_budget_callbacks),
      "Grants: " + name_list(runtime.grants()),
      "Dependencies: " + (dependencies == this->dependencies.end() ? std::string("(none)") : name_list(dependencies->second))};
  lines.emplace_back(std::format("Settings: {} declared (values hidden)", runtime.settings().size()));
  for (const auto& [name, setting] : runtime.settings()) {
    lines.emplace_back("  " + name + ": " + setting.type + (setting.value ? " (set)" : " (unset)"));
  }
  for (const auto& command : runtime.command_details()) {
    lines.emplace_back(std::format("Command {}: permission={}, cooldown={} ms", command.name, command.permission, command.cooldown_ms));
    lines.emplace_back("  " + command.usage);
    if (!command.help.empty()) lines.emplace_back("  " + diagnostic_text(command.help));
  }
  if (!diagnostics.last_error.empty()) lines.emplace_back("Last callback failure: " + diagnostic_text(diagnostics.last_error));
  return lines;
}

std::deque<std::string> Manager::scene_info(const std::string& id) const {
  if (!valid_id(id)) throw std::runtime_error("invalid extension ID");
  if (!this->packages.count(id)) throw std::runtime_error("no such loaded extension: " + id);
  const auto snapshot = this->scenes.diagnostics(id);
  std::deque<std::string> lines{
      std::format("Scenes for {}: backend {}; experimental models {}", id,
          this->scenes_enabled ? "enabled" : "disabled", this->experimental_models ? "enabled" : "disabled"),
      "Registry/prepared metadata only; delivery and rendering are unconfirmed.",
      std::format("Assets: {}; owned objects: {}; event overrides: {}",
          snapshot.assets.size(), snapshot.objects.size(), snapshot.events.size())};
  auto append_variant = [&](const char* label, const SceneVariantDiagnostics& variant) {
    lines.emplace_back(std::format("  {}: compressed bytes texture={} model={} animation={}; compatible original={} pc={}",
        label, variant.compressed_bytes[0], variant.compressed_bytes[1], variant.compressed_bytes[2],
        variant.compatibility.gc_ok ? "yes" : "no", variant.compatibility.pc_ok ? "yes" : "no"));
    if (variant.compressed_bytes[1]) {
      std::string builds;
      for (uint32_t build : variant.model_builds) {
        if (!builds.empty()) builds += ", ";
        builds += std::format("{:08X}", build);
      }
      lines.emplace_back("  Model build allowlist: " + builds);
    }
  };
  for (const auto& asset : snapshot.assets) {
    lines.emplace_back("Asset " + asset.id);
    append_variant("Primary", asset.primary);
    if (asset.fallback) append_variant("Original-client fallback", *asset.fallback);
    else lines.emplace_back("  Original-client fallback: none");
  }
  for (const auto& object : snapshot.objects) {
    lines.emplace_back(std::format("Object {}: lobby={} asset={} location=0x{:08X}",
        object.key, object.lobby_id, object.asset, object.location));
    lines.emplace_back(object.lifetime_ms ? std::format("  Lifetime: {} ms (configured duration)", object.lifetime_ms) : "  Lifetime: persistent");
  }
  for (const auto& event : snapshot.events) {
    lines.emplace_back(std::format("Event override: lobby={} previous={} applied={}",
        event.lobby_id, event.previous, event.applied));
  }
  return lines;
}

Context Manager::context_for(std::shared_ptr<Client> client, std::shared_ptr<Lobby> lobby) const {
  std::string name = client->login ? client->login->account->last_player_name : "";
  return {client->id, lobby ? lobby->lobby_id : 0, std::move(name), "everyone",
      phosg::name_for_enum(client->version()), lobby && lobby->is_game(), true};
}

std::shared_ptr<Client> Manager::live_client(const Context& context) const {
  auto s = this->server.lock();
  if (!s) {
    return nullptr;
  }
  auto it = s->client_for_id.find(context.client_id);
  if (it == s->client_for_id.end()) {
    return nullptr;
  }
  auto c = it->second;
  if (!c) return nullptr;
  auto l = c->lobby.lock();
  if (!l) return nullptr;
  auto registered_lobby = s->id_to_lobby.find(context.lobby_id);
  LiveMembership membership{
      context.client_id, context.lobby_id,
      static_cast<bool>(c->login), c->channel->connected(), static_cast<bool>(c->proxy_session),
      c->server_behavior == ServerBehavior::GAME_SERVER,
      c->version() != Version::UNKNOWN && !is_patch(c->version()) && !is_pre_v1(c->version()),
      c->version() != Version::BB_V4 || c->bb_connection_phase >= 4,
      c->id == context.client_id,
      registered_lobby != s->id_to_lobby.end() && registered_lobby->second == l,
      l->lobby_id == context.lobby_id && c->lobby_client_id < l->clients.size() && l->clients[c->lobby_client_id] == c};
  return eligible_live_member(membership) ? c : nullptr;
}

bool Manager::authorized(const Context& context) const {
  if (!context.allow_player_actions) {
    return false;
  }
  auto c = this->live_client(context);
  if (!c || is_pre_v1(c->version()) || !c->can_chat || !c->can_use_chat_commands()) {
    return false;
  }
  uint32_t mask = 0;
  if (context.permission == "announce") {
    mask = static_cast<uint32_t>(Account::Flag::ANNOUNCE);
  } else if (context.permission == "debug") {
    mask = static_cast<uint32_t>(Account::Flag::DEBUG);
  } else if (context.permission == "moderator") {
    mask = static_cast<uint32_t>(Account::Flag::MODERATOR);
  } else if (context.permission == "root") {
    mask = static_cast<uint32_t>(Account::Flag::ROOT);
  } else if (context.permission == "change-event") {
    mask = static_cast<uint32_t>(Account::Flag::CHANGE_EVENT);
  } else if (context.permission != "everyone") {
    return false;
  }
  return (c->login->account->flags & mask) == mask;
}

std::optional<LobbyView> Manager::lobby_view(const Context& context) const {
  if (!this->authorized(context)) return std::nullopt;
  auto caller = this->live_client(context);
  auto s = this->server.lock();
  auto lobby = caller ? caller->lobby.lock() : nullptr;
  if (!s || !lobby) return std::nullopt;
  LobbyView view{lobby->lobby_id, lobby->name, lobby->is_game(), lobby->event, lobby->max_clients, {}};
  bool caller_present = false;
  for (size_t slot = 0; slot < lobby->clients.size(); slot++) {
    const auto& c = lobby->clients[slot];
    if (!c || c->lobby_client_id != slot) continue;
    auto recipient = context;
    recipient.client_id = c->id;
    if (this->live_client(recipient) != c) continue;
    view.players.emplace_back(LobbyPlayerView{c->id, slot, c->login->account->last_player_name,
        phosg::name_for_enum(c->version()), slot == lobby->leader_id});
    caller_present |= c == caller;
  }
  return caller_present ? std::optional<LobbyView>(std::move(view)) : std::nullopt;
}

std::optional<std::vector<CatalogCommand>> Manager::command_catalog(const Context& context) const {
  if (!context.allow_player_actions || !this->authorized(context)) return std::nullopt;
  std::vector<CatalogCommand> result;
  for (const auto& [id, runtime] : this->packages) {
    if (!runtime->enabled()) continue;
    for (const auto& command : runtime->command_details()) {
      result.emplace_back(CatalogCommand{id, command.name, command.permission, command.cooldown_ms, command.usage, command.help});
    }
  }
  return result;
}

void Manager::reply(const Context& context, const std::string& text) {
  // Error replies must work even when a command's user permission was denied.
  if (auto c = this->live_client(context)) {
    send_text_message(c, message_text(text));
  }
}

void Manager::broadcast(const Context& context, const std::string& text) {
  if (!this->authorized(context)) {
    throw std::runtime_error("broadcast context is no longer authorized");
  }
  auto lobby = this->live_client(context)->require_lobby();
  for (size_t slot = 0; slot < lobby->clients.size(); slot++) {
    const auto& c = lobby->clients[slot];
    if (!c || c->lobby_client_id != slot) continue;
    auto recipient = context;
    recipient.client_id = c->id;
    if (this->live_client(recipient) == c) {
      send_text_message(c, message_text(text));
    }
  }
}

AnnouncementResult Manager::announce(const std::string& owner, const std::string& text) {
  auto s = this->server.lock();
  auto it = this->packages.find(owner);
  if (!s || !s->game_server || it == this->packages.end() || !it->second->enabled() ||
      !it->second->grants().count("chat.announce")) return {false, "unavailable"};
  const auto escaped = message_text(text);
  auto result = this->announcement_gate.admit(owner);
  if (!result.ok) return result;
  size_t failures = 0;
  for (const auto& c : s->game_server->all_clients()) {
    // Use native presentation, with the extension host's conservative local
    // session/version gates. No lobby membership is required for an announcement.
    if (!c || !eligible_announcement_recipient({static_cast<bool>(c->login), c->channel->connected(),
                  static_cast<bool>(c->proxy_session), c->server_behavior == ServerBehavior::GAME_SERVER,
                  c->version() != Version::UNKNOWN && !is_patch(c->version()) && !is_pre_v1(c->version()),
                  c->version() != Version::BB_V4 || c->bb_connection_phase >= 4})) continue;
    try {
      send_text_or_scrolling_message(c, escaped, escaped);
    } catch (const std::exception&) {
      ++failures; // A single recipient failure must not suppress all later recipients.
    }
  }
  if (failures) this->log(owner, "Announcement delivery failed for " + std::to_string(failures) + " recipients.");
  return {true, nullptr}; // Accepted fanout attempt, not a delivery/rendering acknowledgement.
}

bool Manager::whisper(const Context& context, uint64_t target, const std::string& text) {
  if (!this->authorized(context)) {
    throw std::runtime_error("whisper context is no longer authorized");
  }
  Context recipient = context;
  recipient.client_id = target;
  auto c = this->live_client(recipient);
  if (!c) {
    return false;
  }
  send_text_message(c, message_text(text));
  return true;
}

void Manager::log(const std::string& id, const std::string& text) {
  config_log.info_f("[extension:{}] {}", id, diagnostic_text(text));
}

bool Manager::command_reserved(const std::string& name) const {
  return is_builtin_chat_command("$" + name) || (this->staging_names && this->staging_names->count(name));
}

bool Manager::dispatch(std::shared_ptr<Client> c, const std::string& name, const std::string& args) {
  if (c->proxy_session || !c->login || is_pre_v1(c->version()) || name.size() < 2) {
    return false;
  }
  auto context = this->context_for(c, c->lobby.lock());
  try {
    for (auto& [id, runtime] : this->packages) {
      (void)id;
      if (runtime->dispatch(name.substr(1), args, context)) {
        if (!runtime->enabled()) {
          this->cleanup_scene_owner(id);
        }
        this->flush_scene_updates();
        return true;
      }
    }
  } catch (const std::exception& e) {
    this->log("host", e.what());
    this->reply(context, "Extension host failed; see the server log.");
    return true;
  }
  return false;
}

void Manager::lobby_event(const std::string& event, std::shared_ptr<Client> client, std::shared_ptr<Lobby> lobby) {
  if (event == "lobby_leave" && lobby) this->games.member_left(lobby->lobby_id, client->id);
  if (client->proxy_session || !client->login) {
    return;
  }
  if (event == "lobby_leave" && client->lobby.lock() == lobby) {
    this->prepare_scene(client, nullptr); // Clear the departing lobby's objects before its scene unloads.
  } else if (event == "lobby_join") {
    // Game creation can deliberately skip send_lobby_join_notifications; still
    // restore that client's baseline before the caller sends the game join.
    this->prepare_scene(client, lobby);
  }
  auto context = this->context_for(client, lobby);
  this->sessions.observe(context);
  for (auto& [id, runtime] : this->packages) {
    try {
      runtime->cancel_client(client->id);
      runtime->emit(event, context);
      if (!runtime->enabled()) {
        this->cleanup_scene_owner(id);
      }
      if (event == "lobby_leave") {
        runtime->cancel_client(client->id);
      }
    } catch (const std::exception& e) {
      this->log(id, e.what());
    }
  }
  if (event != "lobby_leave") {
    if (event == "lobby_join") this->game_opened(client, lobby);
    this->flush_scene_updates();
  }
}

void Manager::game_opened(std::shared_ptr<Client> client, std::shared_ptr<Lobby> lobby) {
  try {
    auto s = this->server.lock();
    if (!s || !lobby) return;
    auto admission = game_admission(s, client, lobby);
    if (!GameLifecycle::eligible(admission)) return;
    auto snapshot = game_snapshot(lobby);
    bool opened = this->games.begin(lobby->lobby_id, admission);
    this->games.member_joined(lobby->lobby_id, client->id);
    if (opened) this->emit_game_event("game_opened", snapshot);
  } catch (const std::exception& e) {
    this->log("games", e.what());
  }
}

void Manager::game_join_ready(std::shared_ptr<Client> client, std::shared_ptr<Lobby> lobby,
    bool was_loading, bool initial_ready) {
  if (!was_loading || !initial_ready || !client || !lobby) return;
  try {
    auto admission = game_admission(this->server.lock(), client, lobby);
    if (!GameLifecycle::eligible(admission)) return;
    auto context = this->context_for(client, lobby);
    context.allow_player_actions = false;
    if (this->games.join_ready(lobby->lobby_id, client->id, admission, was_loading, initial_ready)) {
      this->emit_session_event("game_join_ready", context);
    }
  } catch (const std::exception& e) {
    this->log("games", e.what());
  }
}

void Manager::game_closed(std::shared_ptr<Lobby> lobby) {
  try {
    if (!lobby || !this->games.end(lobby->lobby_id)) return;
    this->emit_game_event("game_closed", game_snapshot(lobby));
  } catch (const std::exception& e) {
    this->log("games", e.what());
  }
}

void Manager::quest_assigned(std::shared_ptr<Lobby> lobby) {
  this->quest_event(lobby, false);
}

void Manager::quest_map_loaded(std::shared_ptr<Lobby> lobby) {
  this->quest_event(lobby, true);
}

void Manager::quest_load_acknowledged(std::shared_ptr<Client> client, std::shared_ptr<Lobby> lobby) {
  this->quest_acknowledged(client, lobby, QuestAcknowledgementKind::INITIAL_LOAD);
}

void Manager::quest_join_acknowledged(std::shared_ptr<Client> client, std::shared_ptr<Lobby> lobby, bool bb_ready) {
  if (!client || (bb_ready != (client->version() == Version::BB_V4)) ||
      !lobby || !lobby->check_flag(Lobby::Flag::JOINABLE_QUEST_IN_PROGRESS)) return;
  this->quest_acknowledged(client, lobby, bb_ready ? QuestAcknowledgementKind::JOINABLE_BB_READY : QuestAcknowledgementKind::JOINABLE_PLAYER_VISIBLE);
}

void Manager::quest_acknowledged(std::shared_ptr<Client> client, std::shared_ptr<Lobby> lobby, QuestAcknowledgementKind kind) {
  if (!client || !lobby || !lobby->quest) return;
  try {
    auto admission = game_admission(this->server.lock(), client, lobby);
    if (!GameLifecycle::eligible(admission)) return;
    QuestLoadAcknowledgement snapshot{game_snapshot(lobby), client->id, phosg::name_for_enum(client->version()), kind};
    validate_quest_acknowledgement(snapshot);
    bool notify = kind == QuestAcknowledgementKind::INITIAL_LOAD
        ? this->games.acknowledge_quest(lobby->lobby_id, client->id, *snapshot.game.quest_number, admission)
        : this->games.acknowledge_joinable_quest(lobby->lobby_id, client->id, *snapshot.game.quest_number, admission);
    if (!notify) return;
    for (auto& [id, runtime] : this->packages) {
      try {
        runtime->emit_quest_acknowledgement(snapshot);
        if (!runtime->enabled()) this->cleanup_scene_owner(id);
      } catch (const std::exception& e) {
        this->log(id, e.what());
      }
    }
    this->flush_scene_updates();
  } catch (const std::exception& e) {
    this->log("quests", e.what());
  }
}

void Manager::quest_event(std::shared_ptr<Lobby> lobby, bool map_loaded) {
  try {
    auto s = this->server.lock();
    if (!s || !lobby || !lobby->is_game() || !lobby->quest || lobby->battle_player ||
        lobby->check_flag(Lobby::Flag::IS_SPECTATOR_TEAM) || lobby->check_flag(Lobby::Flag::IS_CLIENT_CUSTOMIZATION)) return;
    auto registered = s->id_to_lobby.find(lobby->lobby_id);
    if (registered == s->id_to_lobby.end() || registered->second != lobby) return;
    auto snapshot = game_snapshot(lobby);
    bool notify = map_loaded ? this->games.load_quest_map(lobby->lobby_id, *snapshot.quest_number) :
                              this->games.assign_quest(lobby->lobby_id, *snapshot.quest_number, snapshot.quest_joinable);
    if (notify) this->emit_game_event(map_loaded ? "quest_map_loaded" : "quest_assigned", snapshot);
  } catch (const std::exception& e) {
    this->log("quests", e.what());
  }
}

void Manager::emit_game_event(const std::string& event, const GameSnapshot& snapshot) {
  for (auto& [id, runtime] : this->packages) {
    try {
      runtime->emit_game(event, snapshot);
      if (!runtime->enabled()) this->cleanup_scene_owner(id);
    } catch (const std::exception& e) {
      this->log(id, e.what());
    }
  }
  try {
    this->flush_scene_updates();
  } catch (const std::exception& e) {
    this->log("games", e.what());
  }
}

void Manager::disconnected(uint64_t client_id) {
  this->games.forget_client(client_id);
  this->scenes.forget_client(client_id);
  for (auto& [id, runtime] : this->packages) {
    (void)id;
    runtime->cancel_client(client_id);
  }
  if (auto context = this->sessions.end(client_id)) {
    this->emit_session_event("client_disconnect", *context);
  }
}

void Manager::client_logged_in(std::shared_ptr<Client> client) {
  // This hook is in the local game-server procedure, after authentication and
  // version selection. BB data-server phases and proxy logins are excluded.
  if (!client->login || client->proxy_session || !client->channel->connected() ||
      client->server_behavior != ServerBehavior::GAME_SERVER || is_patch(client->version()) ||
      client->version() == Version::UNKNOWN || (is_v4(client->version()) && client->bb_connection_phase < 4)) {
    return;
  }
  auto context = this->context_for(client, nullptr);
  context.allow_player_actions = false;
  if (this->sessions.begin(context)) {
    this->emit_session_event("client_login", context);
  }
}

void Manager::proxy_started(uint64_t client_id) {
  // GameServer clears proxy_session before on_disconnect. Forget admission now
  // so that later proxy cleanup cannot masquerade as a local-session event.
  this->sessions.forget(client_id);
  this->disconnected(client_id); // Cancel timers/cache without emitting an event.
}

void Manager::emit_session_event(const std::string& event, const Context& context) {
  for (auto& [id, runtime] : this->packages) {
    try {
      runtime->emit(event, context);
      if (!runtime->enabled()) this->cleanup_scene_owner(id);
    } catch (const std::exception& e) {
      this->log(id, e.what());
    }
  }
  try {
    this->flush_scene_updates();
  } catch (const std::exception& e) {
    this->log("sessions", e.what());
  }
}

void Manager::flush_storage() {
  std::string failures;
  for (auto& [id, runtime] : this->packages) {
    if (runtime->storage_dirty()) {
      try {
        write_storage(id, runtime->storage(), runtime->storage_version());
        runtime->storage_saved();
      } catch (const std::exception& e) {
        if (!failures.empty()) failures += "; ";
        failures += id + ": " + e.what();
      }
    }
  }
  if (!failures.empty()) {
    throw std::runtime_error("extension storage flush failed: " + failures);
  }
}

void Manager::refresh_baseline() {
  auto s = this->server.lock();
  if (!s) {
    return;
  }
  auto current = s->data->ep3_lobby_banners;
  if (current != this->baseline_media) {
    this->baseline_media = std::move(current);
  }
}

SceneClient Manager::scene_client(std::shared_ptr<Client> client, std::shared_ptr<Lobby> lobby) const {
  return {client->id, lobby ? lobby->lobby_id : 0, client->specific_version,
      client->version() == Version::GC_EP3,
      this->scenes_enabled && native_card_lobby(client, lobby),
      static_cast<bool>(client->proxy_session), client->channel->connected(), this->experimental_models,
      ep3_media_profile_for_client(client)};
}

void Manager::send_login_media(std::shared_ptr<Client> client) {
  // Preserve upstream's initial media behavior; this is only called inside its
  // existing retail-Episode-3/HAS_EP3_MEDIA_UPDATES guard.
  if (client->version() != Version::GC_EP3 || client->proxy_session || !client->channel->connected()) {
    return;
  }
  this->refresh_baseline();
  std::vector<MediaUpdate> sent;
  for (const auto& media : SceneService::baseline_for(ep3_media_profile_for_client(client), this->baseline_media)) {
    if (send_ep3_media_update(client, media.type, media.locations, *media.data)) {
      client->set_flag(Client::Flag::HAS_EP3_MEDIA_UPDATES);
      sent.emplace_back(media);
    }
  }
  this->scenes.note_baseline(client->id, sent);
}

void Manager::prepare_scene(std::shared_ptr<Client> client, std::shared_ptr<Lobby> lobby) {
  // A final recipient gate surrounds ALL extension-driven B9 sends, including
  // reset packets. Never infer retail support merely from is_ep3().
  if (client->version() != Version::GC_EP3 || client->proxy_session || !client->login || !client->channel->connected()) {
    this->scenes.forget_client(client->id);
    return;
  }
  try {
    this->expire_scenes();
    auto plan = this->scenes.plan(this->scene_client(client, lobby), this->baseline_media);
    if (!plan.result.ok) {
      this->log("scenes", plan.result.reason);
      return;
    }
    for (const auto& media : plan.updates) {
      if (!send_ep3_media_update(client, media.type, media.locations, media.data ? *media.data : "")) {
        throw std::runtime_error("native media snapshot failed the final recipient-size gate");
      }
    }
    this->scenes.sent(plan); // Receipt/rendering is not asserted by sending a snapshot.
  } catch (const std::exception& e) {
    this->scenes.forget_client(client->id); // Force a complete retry after a partial send.
    this->log("scenes", e.what());
  }
}

void Manager::sync_lobby(int64_t lobby_id) {
  auto s = this->server.lock();
  if (!s) {
    return;
  }
  auto it = s->id_to_lobby.find(lobby_id);
  if (it == s->id_to_lobby.end()) {
    return;
  }
  for (const auto& c : it->second->clients) {
    if (c) {
      this->prepare_scene(c, it->second);
    }
  }
}

void Manager::sync_all_scenes() {
  auto s = this->server.lock();
  if (!s) {
    return;
  }
  std::set<int64_t> live;
  for (const auto& [id, lobby] : s->id_to_lobby) {
    (void)lobby;
    live.emplace(id);
  }
  this->scenes.prune_lobbies(live);
  for (const auto& [id, c] : s->client_for_id) {
    (void)id;
    if (c->login && c->version() == Version::GC_EP3 && c->lobby.lock()) {
      this->prepare_scene(c, c->lobby.lock());
    }
  }
}

void Manager::expire_scenes() {
  auto affected = this->scenes.expire();
  this->dirty_scene_lobbies.merge(affected); // Transfer nodes without allocating after expiration.
}

void Manager::flush_scene_updates(bool force) {
  auto now = std::chrono::steady_clock::now();
  if (!force && now - this->last_scene_flush < std::chrono::milliseconds(250)) {
    return;
  }
  auto pending = std::move(this->dirty_scene_lobbies);
  this->dirty_scene_lobbies.clear();
  for (int64_t lobby_id : pending) {
    this->sync_lobby(lobby_id);
  }
  this->last_scene_flush = now;
}

void Manager::cleanup_scene_owner(const std::string& owner) {
  auto s = this->server.lock();
  if (s) {
    for (int64_t id : this->scenes.event_lobbies(owner)) {
      auto lobby = s->id_to_lobby.find(id);
      uint8_t current = lobby == s->id_to_lobby.end() ? 0 : lobby->second->event;
      auto previous = this->scenes.release_event(owner, id, current);
      if (previous && lobby != s->id_to_lobby.end()) {
        lobby->second->event = *previous;
        try {
          send_change_event(lobby->second, *previous);
        } catch (const std::exception& e) {
          this->log("scenes", e.what());
        }
      }
    }
  }
  auto affected = this->scenes.remove_owner(owner);
  this->dirty_scene_lobbies.insert(affected.begin(), affected.end());
}

SceneCapabilities Manager::scene_capabilities(const Context& context) const {
  auto c = this->live_client(context);
  if (!c || !this->scenes_enabled) {
    return {};
  }
  auto lobby = c->lobby.lock();
  bool media = native_card_lobby(c, lobby) && c->version() == Version::GC_EP3;
  bool events = lobby && !lobby->is_game() && (c->version() == Version::GC_V3 || c->version() == Version::GC_EP3 || c->version() == Version::GC_EP3_NTE || c->version() == Version::XB_V3 || (c->version() == Version::BB_V4 && c->bb_connection_phase >= 4));
  return {media, media && this->experimental_models, events,
      media && ep3_media_profile_for_client(c) == Episode3::LobbyMediaProfile::PC_PORT};
}

SceneAssetSupport Manager::scene_asset_support(const Context& context, const std::string& owner, const std::string& asset) const {
  if (!this->authorized(context)) return {};
  if (!this->scenes_enabled) return {false, "scenes_disabled", nullptr};
  auto client = this->live_client(context);
  if (!client) return {};
  return this->scenes.asset_support(owner, asset, this->scene_client(client, client->lobby.lock()));
}

SceneResult Manager::scene_spawn(const Context& context, const std::string& owner, const std::string& key,
    const std::string& asset, const std::string& slot, uint32_t lifetime_ms) {
  if (!this->authorized(context) || !this->scene_capabilities(context).media) {
    return {false, "native media requires a retail Episode 3 CARD lobby and Scenes.Enabled"};
  }
  this->expire_scenes();
  auto result = this->scenes.spawn(owner, context.lobby_id, key, asset, slot, this->baseline_media, lifetime_ms);
  if (result.ok) {
    this->dirty_scene_lobbies.emplace(context.lobby_id);
  }
  return result;
}

SceneResult Manager::scene_remove(const Context& context, const std::string& owner, const std::string& key) {
  if (!this->authorized(context) || !this->scene_capabilities(context).media) {
    return {false, "native media is unavailable in this lobby"};
  }
  auto result = this->scenes.remove(owner, context.lobby_id, key);
  if (result.ok) {
    this->dirty_scene_lobbies.emplace(context.lobby_id);
  }
  return result;
}

SceneResult Manager::scene_event(const Context& context, const std::string& owner, int event) {
  if (!this->authorized(context) || !this->scene_capabilities(context).events) {
    return {false, "live lobby events are unavailable for this client/scene"};
  }
  auto c = this->live_client(context);
  if (!c->login->account->check_flag(Account::Flag::CHANGE_EVENT)) {
    return {false, "CHANGE_EVENT account permission is required"};
  }
  auto lobby = c->require_lobby();
  if (event < 0) {
    auto previous = this->scenes.release_event(owner, lobby->lobby_id, lobby->event);
    if (previous) {
      lobby->event = *previous;
      send_change_event(lobby, *previous);
    }
    return {true};
  }
  auto result = this->scenes.set_event(owner, lobby->lobby_id, lobby->event, event);
  if (result.ok) {
    lobby->event = event;
    send_change_event(lobby, event);
  }
  return result;
}

void Manager::schedule_tick() {
  this->timer.expires_after(std::chrono::milliseconds(50));
  this->timer.async_wait([weak = this->weak_from_this()](const asio::error_code& error) {
    auto manager = weak.lock();
    if (!manager || error) {
      return;
    }
    try {
      manager->expire_scenes(); // Independent of the creating player's presence or authority.
    } catch (const std::exception& e) {
      manager->log("scenes", e.what());
    }
    auto start = std::chrono::steady_clock::now();
    std::vector<Runtime*> runtimes;
    for (auto& [id, runtime] : manager->packages) {
      (void)id;
      runtimes.emplace_back(runtime.get());
    }
    for (size_t z = 0; z < runtimes.size(); z++) {
      auto* runtime = runtimes[(z + manager->ticks) % runtimes.size()];
      try {
        runtime->tick();
        if (!runtime->enabled()) {
          manager->cleanup_scene_owner(runtime->id());
        }
      } catch (const std::exception& e) {
        manager->log(runtime->id(), e.what());
      }
      if (std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(10)) {
        break;
      }
    }
    manager->flush_scene_updates();
    // Coalesce small storage writes; persist explicitly before reload/unload.
    if (++manager->ticks % 100 == 0) {
      try {
        manager->flush_storage();
      } catch (const std::exception& e) {
        manager->log("storage", e.what());
      }
      try {
        manager->refresh_baseline();
        manager->sync_all_scenes();
      } catch (const std::exception& e) {
        manager->log("scene", e.what());
      }
    }
    manager->schedule_tick();
  });
}

} // namespace teaserv::extensions
