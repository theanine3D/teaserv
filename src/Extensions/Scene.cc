#include "Scene.hh"

#include <algorithm>
#include <stdexcept>

namespace teaserv::extensions {

bool SceneService::valid_name(const std::string& name) {
  return !name.empty() && name.size() <= 64 && std::all_of(name.begin(), name.end(), [](unsigned char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
  });
}

uint32_t SceneService::location_for_slot(const std::string& slot) {
  static const std::map<std::string, uint32_t> slots{
      {"counter-south", 0x00000001}, {"counter-west", 0x00000002}, {"counter-north", 0x00000004}, {"counter-east", 0x00000008},
      {"teleporter-west", 0x00000010}, {"teleporter-east", 0x00000020}, {"banner-south", 0x00000040},
      {"banner-north", 0x00000800}, {"floor-banners", 0x00200000},
      {"south-platform", 0x00800000}, {"jukebox", 0x01000000}, {"battle-west", 0x02000000}, {"battle-east", 0x04000000},
      {"battle-southeast", 0x08000000}, {"battle-southwest", 0x10000000}, {"counter-northwest", 0x20000000},
      {"battle-wall", 0x40000000}, {"inside-counter", 0x80000000}};
  auto it = slots.find(slot);
  return it == slots.end() ? 0 : it->second;
}

void SceneService::replace_catalog(Catalog catalog) {
  if (catalog.size() > MAX_ASSETS) {
    throw std::runtime_error("at most 64 native scene assets may be configured");
  }
  size_t bytes = 0;
  auto validate = [&](const SceneAsset& asset, bool fallback) {
    if (!asset.texture || (asset.animation && !asset.model) || (asset.model && asset.model_builds.empty()) ||
        (fallback && (!asset.compatibility.gc_ok || asset.fallback))) {
      throw std::runtime_error("invalid native scene asset/dependencies/build allowlist/fallback");
    }
    for (const auto& payload : {asset.texture, asset.model, asset.animation}) {
      if (payload) {
        if (payload->empty() || !asset.compatibility.pc_ok || payload->size() > MAX_PC_MEDIA_BYTES ||
            (asset.compatibility.gc_ok && payload->size() > MAX_MEDIA_BYTES)) {
          throw std::runtime_error("native scene payload exceeds B9 compressed-size limits");
        }
        bytes += payload->size();
      }
    }
  };
  for (const auto& [key, asset] : catalog) {
    if (!valid_name(key.first) || !valid_name(key.second)) {
      throw std::runtime_error("invalid native scene asset name");
    }
    validate(asset, false);
    if (asset.fallback) {
      if (static_cast<bool>(asset.model) != static_cast<bool>(asset.fallback->model)) {
        throw std::runtime_error("fallback must use the same texture/model placement category");
      }
      validate(*asset.fallback, true);
    }
  }
  if (bytes > 1024 * 1024) {
    throw std::runtime_error("native scene catalog exceeds 1 MiB of compressed data");
  }
  this->assets.swap(catalog);
  this->objects.clear(); // Reload drops old-generation objects, retaining recipient snapshots for cleanup.
}

SceneResult SceneService::check_baseline(const std::vector<MediaUpdate>& baseline, size_t& groups, uint32_t& locations) {
  std::set<uint32_t> sets;
  std::map<uint32_t, uint32_t> kinds;
  locations = 0;
  if (baseline.size() > MAX_MEDIA_GROUPS * 3) {
    return {false, "upstream media exceeds the shared native media budget"};
  }
  for (const auto& media : baseline) {
    if (media.type < 1 || media.type > 3 || !media.locations || !media.data ||
        media.data->empty() || !media.compatibility.pc_ok || media.data->size() > MAX_PC_MEDIA_BYTES ||
        (media.compatibility.gc_ok && media.data->size() > MAX_MEDIA_BYTES) ||
        (media.fallback_data && (media.fallback_data->empty() || media.fallback_data->size() > MAX_MEDIA_BYTES))) {
      return {false, "invalid upstream media; refusing to clear or rebuild it"};
    }
    auto& types = kinds[media.locations];
    uint32_t bit = 1 << media.type;
    if (types & bit) {
      return {false, "ambiguous duplicate upstream media group"};
    }
    types |= bit;
    if (sets.emplace(media.locations).second) {
      if (locations & media.locations) {
        return {false, "overlapping upstream media locations"};
      }
      locations |= media.locations;
    }
  }
  for (const auto& [slot, types] : kinds) {
    (void)slot;
    if (!(types & (1 << 1)) || ((types & (1 << 3)) && !(types & (1 << 2)))) {
      return {false, "upstream model/animation dependency is missing"};
    }
  }
  groups = sets.size();
  return groups <= MAX_MEDIA_GROUPS ? SceneResult{true} : SceneResult{false, "upstream media fills the native media budget"};
}

SceneResult SceneService::spawn(const std::string& owner, int64_t lobby_id, const std::string& key,
    const std::string& asset_name, const std::string& slot, const std::vector<MediaUpdate>& baseline,
    uint32_t lifetime_ms, Clock::time_point now) {
  if (!valid_name(owner) || !valid_name(key) || lobby_id <= 0) {
    return {false, "invalid scene object identity"};
  }
  if ((lifetime_ms && lifetime_ms < 50) || lifetime_ms > MAX_LIFETIME_MS) {
    return {false, "scene lifetime must be zero or 50..3600000 ms"};
  }
  auto asset = this->assets.find({owner, asset_name});
  if (asset == this->assets.end()) {
    return {false, "asset is not in this extension's catalog"};
  }
  uint32_t location = location_for_slot(slot);
  if (!location || (asset->second.model ? !(location & 0xFF800000) : !(location & 0x007FFFFF))) {
    return {false, "unknown slot or wrong placement category for this asset"};
  }
  size_t groups = 0;
  uint32_t occupied = 0;
  auto baseline_result = check_baseline(baseline, groups, occupied);
  if (!baseline_result.ok) {
    return baseline_result;
  }
  ObjectKey identity{lobby_id, owner, key};
  size_t owner_count = 0;
  for (const auto& [id, object] : this->objects) {
    if (std::get<1>(id) == owner) {
      ++owner_count;
    }
    if (id != identity && std::get<0>(id) == lobby_id) {
      ++groups;
      occupied |= object.location;
    }
  }
  if (occupied & location) {
    return {false, "scene slot is occupied by upstream media or another decoration"};
  }
  if (groups >= MAX_MEDIA_GROUPS) {
    return {false, "all eight native media groups are occupied"};
  }
  if (!this->objects.count(identity) && (this->objects.size() >= MAX_OBJECTS || owner_count >= MAX_OBJECTS_PER_OWNER)) {
    return {false, "extension scene object quota exceeded"};
  }
  this->objects.insert_or_assign(std::move(identity), Object{asset_name, location, lifetime_ms,
      lifetime_ms ? std::optional(now + std::chrono::milliseconds(lifetime_ms)) : std::nullopt});
  return {true};
}

std::set<int64_t> SceneService::expire(Clock::time_point now) {
  std::set<int64_t> affected;
  for (const auto& [id, object] : this->objects) {
    if (object.expires && *object.expires <= now) affected.emplace(std::get<0>(id));
  }
  std::erase_if(this->objects, [&](const auto& entry) {
    return entry.second.expires && *entry.second.expires <= now;
  });
  return affected;
}

SceneResult SceneService::remove(const std::string& owner, int64_t lobby_id, const std::string& key) {
  return this->objects.erase({lobby_id, owner, key}) ? SceneResult{true} : SceneResult{false, "no such owned scene object"};
}

std::set<int64_t> SceneService::remove_owner(const std::string& owner) {
  std::set<int64_t> affected;
  for (auto it = this->objects.begin(); it != this->objects.end();) {
    if (std::get<1>(it->first) == owner) {
      affected.emplace(std::get<0>(it->first));
      it = this->objects.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = this->assets.begin(); it != this->assets.end();) {
    it = it->first.first == owner ? this->assets.erase(it) : std::next(it);
  }
  return affected;
}

void SceneService::clear_objects() { this->objects.clear(); }
size_t SceneService::object_count() const { return this->objects.size(); }

SceneOwnerDiagnostics SceneService::diagnostics(const std::string& owner) const {
  if (!valid_name(owner)) throw std::runtime_error("invalid scene owner");
  SceneOwnerDiagnostics result;
  auto variant = [](const SceneAsset& asset) {
    return SceneVariantDiagnostics{{asset.texture ? asset.texture->size() : 0,
                                       asset.model ? asset.model->size() : 0,
                                       asset.animation ? asset.animation->size() : 0},
        asset.compatibility, asset.model_builds};
  };
  for (const auto& [key, asset] : this->assets) {
    if (key.first == owner) {
      result.assets.emplace_back(SceneAssetDiagnostics{key.second, variant(asset),
          asset.fallback ? std::optional(variant(*asset.fallback)) : std::nullopt});
    }
  }
  for (const auto& [key, object] : this->objects) {
    if (std::get<1>(key) == owner) {
      result.objects.emplace_back(SceneObjectDiagnostics{std::get<0>(key), std::get<2>(key), object.asset, object.location, object.lifetime_ms});
    }
  }
  for (const auto& [id, event] : this->events) {
    if (event.owner == owner) {
      result.events.emplace_back(SceneEventDiagnostics{id, event.previous, event.applied});
    }
  }
  return result;
}

void SceneService::prune_lobbies(const std::set<int64_t>& live) {
  for (auto it = this->objects.begin(); it != this->objects.end();) {
    it = live.count(std::get<0>(it->first)) ? std::next(it) : this->objects.erase(it);
  }
  for (auto it = this->events.begin(); it != this->events.end();) {
    it = live.count(it->first) ? std::next(it) : this->events.erase(it);
  }
}

std::vector<MediaUpdate> SceneService::baseline_for(
    Episode3::LobbyMediaProfile profile, const std::vector<MediaUpdate>& baseline) {
  return Episode3::select_lobby_media(profile, baseline);
}

const SceneAsset& SceneService::select_variant(const SceneAsset& primary, const SceneClient& client) {
  return client.media_profile == Episode3::LobbyMediaProfile::ORIGINAL && primary.fallback ? *primary.fallback : primary;
}

SceneAssetSupport SceneService::check_asset_support(const SceneAsset& primary, const SceneClient& client) {
  const auto& asset = select_variant(primary, client);
  const char* variant = &asset == &primary ? "primary" : "fallback";
  if (!client.retail_ep3 || client.proxy || !client.connected) return {false, "unsupported_client", variant};
  if (!client.card_lobby) return {false, "unsupported_scene", variant};
  if (!asset.compatibility.accepts(client.media_profile)) return {false, "profile_limit", variant};
  if (asset.model && !client.experimental_models) return {false, "models_disabled", variant};
  if (asset.model && !asset.model_builds.count(client.build)) return {false, "build_not_allowed", variant};
  return {true, nullptr, variant};
}

SceneAssetSupport SceneService::asset_support(const std::string& owner, const std::string& asset, const SceneClient& client) const {
  if (!valid_name(owner) || !valid_name(asset)) return {false, "unknown_asset", nullptr};
  auto it = this->assets.find({owner, asset});
  return it == this->assets.end() ? SceneAssetSupport{false, "unknown_asset", nullptr} : check_asset_support(it->second, client);
}

MediaPlan SceneService::plan(const SceneClient& client, const std::vector<MediaUpdate>& baseline) const {
  MediaPlan result;
  result.client_id = client.id;
  if (!client.retail_ep3 || client.proxy || !client.connected) {
    result.result = {false, "client does not support native Episode 3 media"};
    return result;
  }
  size_t groups = 0;
  uint32_t occupied = 0;
  result.result = check_baseline(baseline, groups, occupied);
  if (!result.result.ok) {
    return result;
  }
  auto recipient_baseline = baseline_for(client.media_profile, baseline);
  result.result = check_baseline(recipient_baseline, groups, occupied);
  if (!result.result.ok) {
    return result;
  }
  // Reorder baseline dependencies deterministically before reconstructing.
  for (uint32_t type = 1; type <= 3; ++type) {
    for (const auto& media : recipient_baseline) {
      if (media.type == type) {
        result.snapshot.emplace_back(media);
      }
    }
  }
  if (client.card_lobby) {
    for (const auto& [id, object] : this->objects) {
      if (std::get<0>(id) != client.lobby_id) {
        continue;
      }
      const auto& primary = this->assets.at({std::get<1>(id), object.asset});
      if (!check_asset_support(primary, client).supported) continue;
      const auto& asset = select_variant(primary, client);
      // A changed upstream configuration wins slot conflicts and capacity.
      if ((occupied & object.location) || groups >= MAX_MEDIA_GROUPS) {
        continue;
      }
      occupied |= object.location;
      ++groups;
      result.snapshot.emplace_back(MediaUpdate{1, object.location, asset.texture, asset.compatibility});
      if (asset.model) {
        result.snapshot.emplace_back(MediaUpdate{2, object.location, asset.model, asset.compatibility});
      }
      if (asset.animation) {
        result.snapshot.emplace_back(MediaUpdate{3, object.location, asset.animation, asset.compatibility});
      }
    }
  }
  auto previous = this->client_media.find(client.id);
  if (previous == this->client_media.end() || previous->second != result.snapshot) {
    result.updates.emplace_back(MediaUpdate{4, 0, nullptr});
    result.updates.insert(result.updates.end(), result.snapshot.begin(), result.snapshot.end());
  }
  return result;
}

void SceneService::sent(const MediaPlan& plan) {
  if (plan.result.ok) {
    this->client_media.insert_or_assign(plan.client_id, plan.snapshot);
  }
}

void SceneService::note_baseline(uint64_t client_id, const std::vector<MediaUpdate>& baseline) {
  this->client_media.insert_or_assign(client_id, baseline);
}

void SceneService::forget_client(uint64_t client_id) { this->client_media.erase(client_id); }

bool SceneService::valid_event(uint8_t event) { return event <= 14 && event != 2; }

SceneResult SceneService::set_event(const std::string& owner, int64_t lobby_id, uint8_t current, uint8_t event) {
  if (!valid_name(owner) || lobby_id <= 0 || !valid_event(event)) {
    return {false, "invalid lobby event or owner"};
  }
  auto it = this->events.find(lobby_id);
  if (it != this->events.end() && it->second.owner != owner) {
    return {false, "another extension owns this lobby's event"};
  }
  if (it == this->events.end() && this->events.size() >= MAX_OBJECTS) {
    return {false, "lobby event ownership quota exceeded"};
  }
  // An administrator's intervening change becomes the new restoration value.
  uint8_t previous = it != this->events.end() && current == it->second.applied ? it->second.previous : current;
  this->events.insert_or_assign(lobby_id, EventOverride{owner, previous, event});
  return {true};
}

std::optional<uint8_t> SceneService::release_event(const std::string& owner, int64_t lobby_id, uint8_t current) {
  auto it = this->events.find(lobby_id);
  if (it == this->events.end() || it->second.owner != owner) {
    return {};
  }
  auto result = current == it->second.applied ? std::optional<uint8_t>(it->second.previous) : std::nullopt;
  this->events.erase(it);
  return result;
}

std::vector<int64_t> SceneService::event_lobbies(const std::string& owner) const {
  std::vector<int64_t> result;
  for (const auto& [id, event] : this->events) {
    if (event.owner == owner) {
      result.emplace_back(id);
    }
  }
  return result;
}

} // namespace teaserv::extensions
