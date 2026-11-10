#pragma once

#include <cstdint>
#include <array>
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "../Episode3/LobbyMedia.hh"

namespace teaserv::extensions {

struct SceneResult {
  bool ok;
  const char* reason = nullptr;
};

struct SceneCapabilities {
  bool media = false;
  bool models = false;
  bool events = false;
  bool extended_media = false;
};

// Eligibility under the native sender's filters, not placement or rendering proof.
struct SceneAssetSupport {
  bool supported = false;
  const char* reason = "context_unavailable";
  const char* variant = nullptr;
};

using MediaUpdate = Episode3::LobbyMediaUpdate;

// Payloads have already passed NativeAssets' decompression/format checks. The
// registry rechecks transport sizes too, including in standalone tests.
struct SceneAsset {
  std::shared_ptr<const std::string> texture;
  std::shared_ptr<const std::string> model;
  std::shared_ptr<const std::string> animation;
  std::set<uint32_t> model_builds;
  Episode3::LobbyMediaCompatibility compatibility{true, true};
  std::shared_ptr<const SceneAsset> fallback = nullptr;
};

struct SceneClient {
  uint64_t id = 0;
  int64_t lobby_id = 0;
  uint32_t build = 0;
  bool retail_ep3 = false;
  bool card_lobby = false;
  bool proxy = false;
  bool connected = false;
  bool experimental_models = false;
  Episode3::LobbyMediaProfile media_profile = Episode3::LobbyMediaProfile::ORIGINAL;
};

struct MediaPlan {
  uint64_t client_id = 0;
  SceneResult result{true};
  std::vector<MediaUpdate> snapshot;
  std::vector<MediaUpdate> updates;
};

// Value-only administrative snapshots. No payloads, paths, client identities,
// or mutable registry references leave the service.
struct SceneVariantDiagnostics {
  std::array<size_t, 3> compressed_bytes{}; // Texture, model, animation; zero means absent.
  Episode3::LobbyMediaCompatibility compatibility;
  std::set<uint32_t> model_builds;
};
struct SceneAssetDiagnostics {
  std::string id;
  SceneVariantDiagnostics primary;
  std::optional<SceneVariantDiagnostics> fallback;
};
struct SceneObjectDiagnostics {
  int64_t lobby_id;
  std::string key;
  std::string asset;
  uint32_t location;
  uint32_t lifetime_ms = 0; // Configured duration, not a delivery or countdown clock.
};
struct SceneEventDiagnostics {
  int64_t lobby_id;
  uint8_t previous;
  uint8_t applied;
};
struct SceneOwnerDiagnostics {
  std::vector<SceneAssetDiagnostics> assets;
  std::vector<SceneObjectDiagnostics> objects;
  std::vector<SceneEventDiagnostics> events;
};

class SceneService {
public:
  using Clock = std::chrono::steady_clock;
  static constexpr uint32_t MAX_LIFETIME_MS = 3600000;
  static constexpr size_t MAX_MEDIA_BYTES = Episode3::LOBBY_MEDIA_ORIGINAL_COMPRESSED;
  static constexpr size_t MAX_DECOMPRESSED_BYTES = Episode3::LOBBY_MEDIA_ORIGINAL_DECOMPRESSED;
  static constexpr size_t MAX_PC_MEDIA_BYTES = Episode3::LOBBY_MEDIA_PC_COMPRESSED;
  static constexpr size_t MAX_MEDIA_GROUPS = 8;
  static constexpr size_t MAX_ASSETS = 64;
  static constexpr size_t MAX_OBJECTS = 128;
  static constexpr size_t MAX_OBJECTS_PER_OWNER = 32;
  using AssetKey = std::pair<std::string, std::string>;
  using Catalog = std::map<AssetKey, SceneAsset>;

  // Validate first; a rejected catalog leaves objects and assets unchanged.
  void replace_catalog(Catalog catalog);
  SceneResult spawn(const std::string& owner, int64_t lobby_id, const std::string& key,
      const std::string& asset, const std::string& slot, const std::vector<MediaUpdate>& baseline,
      uint32_t lifetime_ms = 0, Clock::time_point now = Clock::now());
  // Caller reconciles these lobbies through the normal recipient planner.
  // Collect affected IDs before erasing, so allocation failure leaves state intact.
  std::set<int64_t> expire(Clock::time_point now = Clock::now());
  SceneResult remove(const std::string& owner, int64_t lobby_id, const std::string& key);
  std::set<int64_t> remove_owner(const std::string& owner);
  void clear_objects();
  void prune_lobbies(const std::set<int64_t>& live);
  size_t object_count() const;
  // Bounded by the existing catalog, object, and event quotas. Never plans or sends media.
  SceneOwnerDiagnostics diagnostics(const std::string& owner) const;
  SceneAssetSupport asset_support(const std::string& owner, const std::string& asset, const SceneClient& client) const;

  // Unsupported recipients never get even a delete/reset packet. The caller
  // commits the plan only after sending its complete update sequence.
  MediaPlan plan(const SceneClient& client, const std::vector<MediaUpdate>& baseline) const;
  static std::vector<MediaUpdate> baseline_for(Episode3::LobbyMediaProfile profile, const std::vector<MediaUpdate>& baseline);
  void sent(const MediaPlan& plan);
  void note_baseline(uint64_t client_id, const std::vector<MediaUpdate>& baseline);
  void forget_client(uint64_t client_id);

  struct EventOverride {
    std::string owner;
    uint8_t previous;
    uint8_t applied;
  };
  SceneResult set_event(const std::string& owner, int64_t lobby_id, uint8_t current, uint8_t event);
  std::optional<uint8_t> release_event(const std::string& owner, int64_t lobby_id, uint8_t current);
  std::vector<int64_t> event_lobbies(const std::string& owner) const;
  static bool valid_event(uint8_t event);

private:
  using ObjectKey = std::tuple<int64_t, std::string, std::string>;
  struct Object {
    std::string asset;
    uint32_t location;
    uint32_t lifetime_ms = 0;
    std::optional<Clock::time_point> expires;
  };
  Catalog assets;
  std::map<ObjectKey, Object> objects;
  std::map<uint64_t, std::vector<MediaUpdate>> client_media;
  std::map<int64_t, EventOverride> events;
  static uint32_t location_for_slot(const std::string& slot);
  static bool valid_name(const std::string& name);
  static const SceneAsset& select_variant(const SceneAsset& primary, const SceneClient& client);
  static SceneAssetSupport check_asset_support(const SceneAsset& primary, const SceneClient& client);
  static SceneResult check_baseline(const std::vector<MediaUpdate>& baseline, size_t& groups, uint32_t& locations);
};

} // namespace teaserv::extensions
