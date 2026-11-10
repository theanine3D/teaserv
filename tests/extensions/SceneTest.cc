#include "Scene.hh"
#include "NativeMediaFormat.hh"

#include <functional>
#include <iostream>
#include <stdexcept>

using namespace teaserv::extensions;

static void check(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

static void rejects(const std::function<void()>& fn) {
  bool failed = false;
  try {
    fn();
  } catch (const std::exception&) {
    failed = true;
  }
  check(failed, "malformed asset or catalog was accepted");
}

static auto payload(const std::string& data) { return std::make_shared<const std::string>(data); }
static SceneAsset texture() { return {payload("test-prs-texture"), nullptr, nullptr, {}}; }

static void put16(std::string& data, size_t offset, uint16_t value, bool big = false) {
  data[offset] = big ? value >> 8 : value;
  data[offset + 1] = big ? value : value >> 8;
}

static void put32(std::string& data, size_t offset, uint32_t value) {
  put16(data, offset, value);
  put16(data, offset + 2, value >> 16);
}

static std::string gvm() {
  std::string data(0xE0, '\0');
  data.replace(0, 4, "GVMH");
  put32(data, 4, 0x48);
  put16(data, 8, 0xF, true);
  put16(data, 10, 1, true);
  data[43] = 4;
  put16(data, 44, 0x11, true);
  data.replace(0x50, 4, "GVRT");
  put32(data, 0x54, 8 + 128);
  data[0x5B] = 4;
  put16(data, 0x5C, 8, true);
  put16(data, 0x5E, 8, true);
  return data;
}

int main() {
  try {
    SceneService scenes;
    auto model = texture();
    model.model = payload("test-prs-model");
    model.animation = payload("test-prs-animation");
    model.model_builds.emplace(0x33534530); // 3SE0
    SceneService::Catalog catalog{{{"alpha", "banner"}, texture()}, {{"beta", "banner"}, texture()}, {{"alpha", "statue"}, model}};
    scenes.replace_catalog(catalog);
    std::vector<MediaUpdate> baseline{{1, 0x40, payload("upstream-banner")}};
    SceneClient client{1, 10, 0x33534530, true, true, false, true, false};
    scenes.note_baseline(client.id, baseline);
    check(scenes.plan(client, baseline).updates.empty(), "unchanged baseline retransmitted");
    {
      SceneService support;
      auto pc = texture();
      pc.compatibility = {false, true};
      pc.texture = payload(std::string(0x3801, 'x'));
      auto fallback = pc;
      fallback.fallback = std::make_shared<const SceneAsset>(texture());
      auto high_model = model;
      high_model.compatibility = {false, true};
      high_model.model_builds = {0x33535030};
      high_model.fallback = std::make_shared<const SceneAsset>(model);
      support.replace_catalog({{{"alpha", "banner"}, texture()}, {{"alpha", "pc"}, pc},
          {{"alpha", "fallback"}, fallback}, {{"alpha", "model"}, high_model}, {{"beta", "private"}, texture()}});
      auto recipient = client;
      auto result = support.asset_support("alpha", "banner", recipient);
      check(result.supported && !result.reason && std::string(result.variant) == "primary", "compatible texture was unavailable");
      for (const auto& asset : {"private", "missing", "../banner", "beta:private", ""}) {
        result = support.asset_support("alpha", asset, recipient);
        check(!result.supported && std::string(result.reason) == "unknown_asset" && !result.variant,
            "asset lookup escaped package ownership or accepted invalid name");
      }
      result = support.asset_support("../alpha", "banner", recipient);
      check(!result.supported && std::string(result.reason) == "unknown_asset", "invalid owner could query assets");
      result = support.asset_support("alpha", "pc", recipient);
      check(!result.supported && std::string(result.reason) == "profile_limit", "original recipient accepted PC-only asset");
      result = support.asset_support("alpha", "fallback", recipient);
      check(result.supported && std::string(result.variant) == "fallback", "original fallback was not selected");
      recipient.media_profile = Episode3::LobbyMediaProfile::PC_PORT;
      result = support.asset_support("alpha", "fallback", recipient);
      check(result.supported && std::string(result.variant) == "primary", "PC recipient selected an original fallback");
      result = support.asset_support("alpha", "model", recipient);
      check(!result.supported && std::string(result.reason) == "models_disabled", "model query ignored experimental switch");
      recipient.experimental_models = true;
      result = support.asset_support("alpha", "model", recipient);
      check(!result.supported && std::string(result.reason) == "build_not_allowed" && std::string(result.variant) == "primary",
          "model query silently downgraded a PC recipient to a fallback build");
      recipient.build = 0x33535030;
      result = support.asset_support("alpha", "model", recipient);
      check(result.supported && !result.reason && std::string(result.variant) == "primary", "allowlisted PC model was unavailable");
      support.spawn("alpha", 10, "model", "model", "south-platform", {});
      auto planned = support.plan(recipient, {});
      check(planned.snapshot.size() == 3 && planned.snapshot[0].data == high_model.texture,
          "support query and planner disagreed on primary dependencies");
      support.sent(planned);
      support.asset_support("alpha", "model", recipient);
      check(support.plan(recipient, {}).updates.empty() && support.object_count() == 1,
          "support inspection changed registry or recipient cache");
      recipient.media_profile = Episode3::LobbyMediaProfile::ORIGINAL;
      recipient.build = client.build;
      result = support.asset_support("alpha", "model", recipient);
      check(result.supported && std::string(result.variant) == "fallback" && support.plan(recipient, {}).snapshot.size() == 3,
          "support query and planner disagreed on fallback dependencies");
      for (size_t z = 0; z < 4; z++) {
        auto unavailable = recipient;
        if (z == 0) unavailable.retail_ep3 = false;
        if (z == 1) unavailable.proxy = true;
        if (z == 2) unavailable.connected = false;
        if (z == 3) unavailable.card_lobby = false;
        result = support.asset_support("alpha", "model", unavailable);
        check(!result.supported && std::string(result.reason) == (z == 3 ? "unsupported_scene" : "unsupported_client") &&
                support.plan(unavailable, {}).snapshot.empty(), "unsupported query/planner recipient rules disagreed");
      }
      support.spawn("alpha", 10, "occupied", "banner", "teleporter-west", {});
      check(!support.spawn("alpha", 10, "conflict", "banner", "teleporter-west", {}).ok &&
              support.asset_support("alpha", "banner", recipient).supported,
          "asset support incorrectly promised or depended on slot registration");
      support.remove_owner("alpha");
      check(std::string(support.asset_support("alpha", "banner", recipient).reason) == "unknown_asset",
          "unloaded asset retained support metadata");
    }
    {
      using namespace std::chrono;
      const auto start = SceneService::Clock::time_point{};
      SceneService temporary;
      temporary.replace_catalog(catalog);
      check(temporary.spawn("alpha", 10, "short", "banner", "teleporter-west", baseline, 50, start).ok &&
              temporary.spawn("beta", 10, "short", "banner", "teleporter-east", baseline, 50, start).ok &&
              temporary.spawn("alpha", 10, "model", "statue", "south-platform", baseline, 50, start).ok &&
              temporary.spawn("alpha", 10, "permanent", "banner", "counter-west", baseline, 0, start).ok,
          "temporary scene fixtures failed");
      check(temporary.diagnostics("alpha").objects[1].lifetime_ms == 0 &&
              temporary.diagnostics("alpha").objects[2].lifetime_ms == 50,
          "persistent or temporary duration diagnostics changed");
      auto recipient = client;
      recipient.experimental_models = true;
      auto before = temporary.plan(recipient, baseline);
      check(before.snapshot.size() == 7, "temporary model dependencies were missing");
      temporary.sent(before);
      check(temporary.expire(start + milliseconds(49)).empty() && temporary.object_count() == 4,
          "decoration expired before deadline");
      check(temporary.expire(start + milliseconds(50)) == std::set<int64_t>{10} && temporary.object_count() == 1,
          "deadline did not expire all due owners/dependencies in one lobby");
      auto after = temporary.plan(recipient, baseline);
      check(after.updates.size() == 3 && after.updates[0].type == 4 && after.snapshot.size() == 2 &&
              after.snapshot[0] == baseline[0], "expiration did not reset/rebuild surviving media and baseline");
      temporary.sent(after);
      check(temporary.plan(recipient, baseline).updates.empty() && temporary.expire(start + hours(2)).empty(),
          "expiration repeated or removed a persistent object");
      recipient.retail_ep3 = false;
      check(temporary.plan(recipient, baseline).updates.empty(), "expiration planned traffic for unsupported recipient");
      recipient = client;
      recipient.media_profile = Episode3::LobbyMediaProfile::PC_PORT;
      check(temporary.plan(recipient, baseline).snapshot.size() == 2, "PC snapshot retained expired objects");

      check(temporary.spawn("alpha", 11, "renew", "banner", "teleporter-west", {}, 100, start).ok &&
              temporary.spawn("alpha", 11, "renew", "banner", "teleporter-west", {}, 200, start + milliseconds(50)).ok,
          "temporary replacement failed");
      check(temporary.expire(start + milliseconds(100)).empty(), "old deadline removed renewed object");
      check(!temporary.spawn("alpha", 11, "renew", "banner", "teleporter-west", {}, 49, start).ok &&
              !temporary.spawn("alpha", 11, "renew", "banner", "teleporter-west", {}, 3600001, start).ok &&
              !temporary.spawn("alpha", 11, "renew", "missing", "teleporter-west", {}, 50, start).ok,
          "invalid replacement lifetime or asset accepted");
      check(temporary.expire(start + milliseconds(249)).empty() &&
              temporary.expire(start + milliseconds(250)) == std::set<int64_t>{11},
          "rejected replacement changed original deadline");
      check(temporary.spawn("alpha", 11, "renew", "banner", "teleporter-west", {}, 50, start).ok &&
              temporary.spawn("alpha", 11, "renew", "banner", "teleporter-west", {}, 0, start).ok &&
              temporary.expire(start + hours(2)).empty(), "persistent replacement retained expiration");
      temporary.remove("alpha", 11, "renew");
      temporary.spawn("alpha", 11, "renew", "banner", "teleporter-west", {}, 50, start);
      temporary.remove("alpha", 11, "renew");
      temporary.spawn("beta", 11, "reuse", "banner", "teleporter-west", {}, 0, start);
      check(temporary.expire(start + hours(2)).empty(), "old removed key expired a new slot owner");
      temporary.spawn("alpha", 12, "max", "banner", "teleporter-west", {}, SceneService::MAX_LIFETIME_MS, start);
      check(temporary.expire(start + milliseconds(SceneService::MAX_LIFETIME_MS - 1)).empty() &&
              temporary.expire(start + milliseconds(SceneService::MAX_LIFETIME_MS)) == std::set<int64_t>{12},
          "maximum lifetime boundary failed");
      temporary.spawn("alpha", 13, "cleanup", "banner", "teleporter-west", {}, 50, start);
      temporary.prune_lobbies({10, 11});
      check(temporary.expire(start + hours(2)).empty(), "pruned object retained an expiration");
      temporary.spawn("alpha", 13, "cleanup", "banner", "teleporter-west", {}, 50, start);
      auto invalid = catalog;
      invalid.at({"alpha", "banner"}).texture.reset();
      rejects([&] { temporary.replace_catalog(invalid); });
      check(temporary.expire(start + milliseconds(50)) == std::set<int64_t>{13}, "failed staging lost active deadline");
      temporary.spawn("alpha", 13, "cleanup", "banner", "teleporter-west", {}, 50, start);
      temporary.replace_catalog(catalog);
      check(temporary.expire(start + hours(2)).empty(), "catalog replacement retained old deadline");
      temporary.spawn("alpha", 13, "cleanup", "banner", "teleporter-west", {}, 50, start);
      temporary.remove_owner("alpha");
      check(temporary.expire(start + hours(2)).empty(), "owner cleanup retained deadline");
    }
    {
      const auto start = SceneService::Clock::time_point{};
      SceneService quota;
      quota.replace_catalog({{{"alpha", "banner"}, texture()}});
      for (size_t z = 1; z <= SceneService::MAX_OBJECTS_PER_OWNER; z++) {
        check(quota.spawn("alpha", z, "timed", "banner", "teleporter-west", {}, 50, start).ok, "temporary quota fixture failed");
      }
      check(!quota.spawn("alpha", 100, "extra", "banner", "teleporter-west", {}, 50, start).ok,
          "temporary objects bypassed owner quota");
      check(quota.expire(start + std::chrono::milliseconds(50)).size() == SceneService::MAX_OBJECTS_PER_OWNER &&
              quota.spawn("alpha", 100, "extra", "banner", "teleporter-west", {}, 0, start).ok,
          "expiration did not reclaim quota and slots");
    }
    {
      SceneService inspected;
      auto pc_model = model;
      pc_model.texture = payload(std::string(0x3801, 'x'));
      pc_model.compatibility = {false, true};
      pc_model.fallback = std::make_shared<const SceneAsset>(model);
      SceneService::Catalog inspected_catalog{{{"alpha", "statue"}, pc_model},
          {{"alpha", "banner"}, texture()}, {{"beta", "private"}, texture()}};
      inspected.replace_catalog(inspected_catalog);
      check(inspected.spawn("alpha", 10, "statue", "statue", "south-platform", {}).ok &&
              inspected.spawn("alpha", 11, "banner", "banner", "teleporter-west", {}).ok &&
              inspected.spawn("beta", 10, "other", "private", "teleporter-east", {}).ok,
          "diagnostic fixtures could not spawn");
      check(inspected.set_event("alpha", 10, 0, 1).ok && inspected.set_event("beta", 11, 3, 4).ok,
          "diagnostic event fixtures failed");
      auto ready = inspected.plan(client, {});
      inspected.sent(ready);
      auto snapshot = inspected.diagnostics("alpha");
      check(snapshot.assets.size() == 2 && snapshot.objects.size() == 2 && snapshot.events.size() == 1,
          "diagnostics leaked another owner's records or omitted owned state");
      const auto& asset = snapshot.assets.at(1);
      check(asset.id == "statue" && asset.primary.compressed_bytes[0] == 0x3801 &&
              asset.primary.compressed_bytes[1] == model.model->size() &&
              asset.primary.compressed_bytes[2] == model.animation->size() &&
              !asset.primary.compatibility.gc_ok && asset.primary.compatibility.pc_ok &&
              asset.primary.model_builds == model.model_builds && asset.fallback &&
              asset.fallback->compatibility.gc_ok && asset.fallback->compressed_bytes[0] == model.texture->size() &&
              asset.fallback->model_builds == model.model_builds,
          "diagnostics lost primary/fallback sizes, compatibility or exact builds");
      check(snapshot.assets[0].primary.compressed_bytes[1] == 0 && !snapshot.assets[0].fallback &&
              snapshot.objects[0].lobby_id == 10 && snapshot.objects[0].key == "statue" &&
              snapshot.objects[0].asset == "statue" && snapshot.objects[0].location == 0x00800000 &&
              snapshot.events[0].lobby_id == 10 && snapshot.events[0].previous == 0 && snapshot.events[0].applied == 1,
          "diagnostic metadata does not match registry");
      snapshot.assets[1].primary.model_builds.clear();
      snapshot.objects[0].key = "forged";
      snapshot.events[0].applied = 14;
      auto unchanged = inspected.diagnostics("alpha");
      check(!unchanged.assets[1].primary.model_builds.empty() && unchanged.objects[0].key == "statue" &&
              unchanged.events[0].applied == 1 && inspected.plan(client, {}).updates.empty(),
          "inspection changed registry or recipient synchronization state");
      check(inspected.diagnostics("missing").assets.empty() && inspected.diagnostics("missing").objects.empty() &&
              inspected.diagnostics("missing").events.empty(), "unknown scene owner leaked records");
      rejects([&]() { inspected.diagnostics("../alpha"); });
      auto invalid_catalog = inspected_catalog;
      invalid_catalog.at({"alpha", "banner"}).texture.reset();
      rejects([&]() { inspected.replace_catalog(invalid_catalog); });
      check(inspected.diagnostics("alpha").objects.size() == 2, "failed staging changed inspected registry");
      inspected.remove("alpha", 10, "statue");
      check(inspected.diagnostics("alpha").objects.size() == 1 && snapshot.objects.size() == 2,
          "removal did not update diagnostics or mutated old copy");
      inspected.prune_lobbies({11});
      check(inspected.diagnostics("alpha").events.empty(), "pruned events remain in diagnostics");
      inspected.replace_catalog(inspected_catalog);
      check(inspected.diagnostics("alpha").objects.empty() && inspected.diagnostics("alpha").assets.size() == 2,
          "replacement did not refresh diagnostic state");
      inspected.remove_owner("alpha");
      check(inspected.diagnostics("alpha").assets.empty() && inspected.diagnostics("beta").assets.size() == 1,
          "owner cleanup left assets or removed another owner's catalog");
    }
    {
      SceneService bounded;
      SceneService::Catalog full_catalog;
      for (size_t z = 0; z < SceneService::MAX_ASSETS; z++) {
        full_catalog.emplace(SceneService::AssetKey{"alpha", "asset-" + std::to_string(z)}, texture());
      }
      bounded.replace_catalog(full_catalog);
      for (size_t z = 1; z <= SceneService::MAX_OBJECTS; z++) {
        check(bounded.set_event("alpha", z, 0, 1).ok, "event diagnostic quota fixture failed");
        if (z <= SceneService::MAX_OBJECTS_PER_OWNER) {
          check(bounded.spawn("alpha", z, "key", "asset-0", "teleporter-west", {}).ok,
              "object diagnostic quota fixture failed");
        }
      }
      const auto full = bounded.diagnostics("alpha");
      check(full.assets.size() == SceneService::MAX_ASSETS && full.objects.size() == SceneService::MAX_OBJECTS_PER_OWNER &&
              full.events.size() == SceneService::MAX_OBJECTS, "quota-bound diagnostics truncated valid state");
    }
    {
      SceneService mixed;
      auto pc_asset = texture();
      pc_asset.texture = payload(std::string(0x3801, 'x'));
      pc_asset.compatibility = {false, true};
      mixed.replace_catalog({{{"pc", "large"}, pc_asset}, {{"gc", "small"}, texture()}});
      check(mixed.spawn("pc", 10, "large", "large", "teleporter-east", {}).ok, "PC asset registration failed");
      check(mixed.spawn("gc", 10, "small", "small", "teleporter-west", {}).ok, "GC asset registration failed");
      std::vector<MediaUpdate> mixed_baseline{{1, 0x40, payload("small-baseline")},
          {1, 0x800, payload("small-compressed-large-output"), {false, true}}};
      auto gc_plan = mixed.plan(client, mixed_baseline);
      check(gc_plan.result.ok && gc_plan.snapshot.size() == 2, "GC received PC-only compressed or expanded media");
      auto pc_client = client;
      pc_client.media_profile = Episode3::LobbyMediaProfile::PC_PORT;
      auto pc_plan = mixed.plan(pc_client, mixed_baseline);
      check(pc_plan.result.ok && pc_plan.snapshot.size() == 4, "PC did not receive both compatible media classes");
      mixed.sent(pc_plan);
      gc_plan = mixed.plan(client, mixed_baseline);
      check(gc_plan.updates.front().type == 4 && gc_plan.snapshot.size() == 2, "profile downgrade did not clear large media");
      mixed.sent(gc_plan);
      check(mixed.plan(client, mixed_baseline).updates.empty(), "filtered baseline caused repeated resets");
      auto dependencies = mixed_baseline;
      dependencies.emplace_back(MediaUpdate{2, 0x40, payload("pc-model"), {false, true}});
      check(SceneService::baseline_for(Episode3::LobbyMediaProfile::ORIGINAL, dependencies).empty(),
          "incompatible model left its dependent baseline texture on an original client");
      check(SceneService::baseline_for(Episode3::LobbyMediaProfile::PC_PORT, dependencies).size() == 3,
          "PC baseline lost a compatible dependency group");
      std::vector<MediaUpdate> fallback_baseline{
          {1, 0x40, payload("high-texture"), {true, true}, payload("low-texture")},
          {2, 0x40, payload("high-model"), {false, true}, payload("low-model")}};
      auto baseline_plan = mixed.plan(client, fallback_baseline);
      check(baseline_plan.result.ok && *baseline_plan.snapshot[0].data == "low-texture" &&
              *baseline_plan.snapshot[1].data == "low-model",
          "configured baseline fallback mixed dependencies or lost the fallback");
      mixed.sent(baseline_plan);
      check(mixed.plan(client, fallback_baseline).updates.empty(), "unchanged baseline fallback caused repeated resets");
      fallback_baseline[1].fallback_data = payload("replacement-low-model");
      check(mixed.plan(client, fallback_baseline).updates.front().type == 4, "fallback replacement did not rebuild the baseline");
      mixed.remove_owner("pc");
      check(mixed.plan(pc_client, mixed_baseline).snapshot.size() == 3, "removing PC media damaged other owners/baseline");
    }
    {
      SceneService variants;
      auto high = model;
      high.compatibility = {false, true};
      high.model_builds = {0x33535030};
      auto low = model;
      low.texture = payload("low-texture");
      low.model = payload("low-model");
      low.animation.reset();
      high.fallback = std::make_shared<const SceneAsset>(low);
      variants.replace_catalog({{{"variants", "statue"}, high}});
      check(variants.spawn("variants", 10, "statue", "statue", "south-platform", {}).ok, "variant spawn failed");
      auto recipient = client;
      recipient.experimental_models = true;
      auto original = variants.plan(recipient, {});
      check(original.snapshot.size() == 2 && original.snapshot[0].data == low.texture && original.snapshot[1].data == low.model,
          "original recipient did not get the complete fallback group");
      variants.sent(original);
      check(variants.plan(recipient, {}).updates.empty(), "unchanged fallback was retransmitted");
      recipient.media_profile = Episode3::LobbyMediaProfile::PC_PORT;
      recipient.build = 0x33535030;
      auto enhanced = variants.plan(recipient, {});
      check(enhanced.snapshot.size() == 3 && enhanced.snapshot[0].data == high.texture && enhanced.updates.front().type == 4,
          "profile upgrade did not replace the fallback with the primary group");
      variants.sent(enhanced);
      recipient.media_profile = Episode3::LobbyMediaProfile::ORIGINAL;
      recipient.build = client.build;
      check(variants.plan(recipient, {}).updates.front().type == 4, "fallback downgrade did not clear primary media");
      recipient.build = 0x33535030;
      check(variants.plan(recipient, {}).snapshot.empty(), "fallback bypassed its own model build allowlist");
      recipient.build = client.build;
      recipient.experimental_models = false;
      check(variants.plan(recipient, {}).snapshot.empty(), "fallback bypassed the experimental-model gate");
      auto invalid = high;
      invalid.fallback = std::make_shared<const SceneAsset>(texture());
      rejects([&] { variants.replace_catalog({{{"variants", "statue"}, invalid}}); });
      low.compatibility = {false, true};
      invalid.fallback = std::make_shared<const SceneAsset>(low);
      rejects([&] { variants.replace_catalog({{{"variants", "statue"}, invalid}}); });
      low = model;
      low.texture = payload(std::string(0x3801, 'x'));
      invalid.fallback = std::make_shared<const SceneAsset>(low);
      rejects([&] { variants.replace_catalog({{{"variants", "statue"}, invalid}}); });
      low = model;
      low.fallback = std::make_shared<const SceneAsset>(model);
      invalid.fallback = std::make_shared<const SceneAsset>(low);
      rejects([&] { variants.replace_catalog({{{"variants", "statue"}, invalid}}); });
      SceneService::Catalog expensive;
      auto large = texture();
      large.texture = payload(std::string(20000, 'x'));
      large.compatibility = {false, true};
      auto small = texture();
      small.texture = payload(std::string(12000, 'y'));
      large.fallback = std::make_shared<const SceneAsset>(small);
      for (size_t z = 0; z < 40; ++z) {
        expensive.emplace(SceneService::AssetKey{"variants", "asset-" + std::to_string(z)}, large);
      }
      rejects([&] { variants.replace_catalog(expensive); });
      check(variants.object_count() == 1, "rejected fallback catalog destroyed active objects");
      variants.remove_owner("variants");
      check(variants.plan(client, {}).snapshot.empty(), "fallback survived owner removal");
    }
    check(scenes.spawn("alpha", 10, "welcome", "banner", "teleporter-west", baseline).ok, "banner spawn failed");
    auto plan = scenes.plan(client, baseline);
    check(plan.updates.size() == 3 && plan.updates[0].type == 4 && plan.updates[1] == baseline[0], "reset did not preserve upstream banner");
    check(plan.updates[2].type == 1 && plan.updates[2].locations == 0x10, "wrong native banner location");
    check(scenes.plan(client, baseline).updates.size() == plan.updates.size(), "unsent plan suppressed retry");
    scenes.sent(plan);
    check(scenes.plan(client, baseline).updates.empty(), "unchanged scene retransmitted");
    check(scenes.spawn("alpha", 10, "welcome", "banner", "teleporter-west", baseline).ok, "owned upsert failed");
    check(scenes.plan(client, baseline).updates.empty(), "idempotent upsert retransmitted");
    check(!scenes.spawn("beta", 10, "stolen", "banner", "teleporter-west", baseline).ok, "cross-owner slot collision");
    check(!scenes.spawn("alpha", 10, "bad", "banner", "banner-south", baseline).ok, "upstream slot collision");
    check(!scenes.remove("beta", 10, "welcome").ok, "cross-owner removal succeeded");
    check(!scenes.spawn("alpha", 10, "bad", "banner", "south-platform", baseline).ok, "texture accepted a model placement");

    auto unsupported = client;
    unsupported.retail_ep3 = false; // Covers Trial Edition and every non-retail-Ep3 version at the adapter boundary.
    check(scenes.plan(unsupported, baseline).updates.empty(), "unsupported client got B9/reset");
    unsupported = client;
    unsupported.proxy = true;
    check(scenes.plan(unsupported, baseline).updates.empty(), "proxy got local media");
    unsupported = client;
    unsupported.connected = false;
    check(scenes.plan(unsupported, baseline).updates.empty(), "disconnected client got media");

    auto late = client;
    late.id = 2;
    check(scenes.plan(late, baseline).updates.size() == 3, "late join did not receive current snapshot");
    auto other = client;
    other.lobby_id = 11;
    check(scenes.plan(other, baseline).snapshot == baseline, "objects leaked to another lobby");
    other = client;
    other.card_lobby = false;
    auto game = scenes.plan(other, baseline);
    check(game.updates.size() == 2 && game.snapshot == baseline, "leaving CARD lobby did not restore baseline");

    check(scenes.spawn("alpha", 10, "statue", "statue", "south-platform", baseline).ok, "model registration failed");
    check(scenes.plan(client, baseline).snapshot.size() == 2, "experimental model leaked while disabled");
    client.experimental_models = true;
    auto models = scenes.plan(client, baseline);
    check(models.snapshot.size() == 5, "model/animation dependencies missing");
    check(models.snapshot[1].type == 1 && models.snapshot[2].type == 2 && models.snapshot[3].type == 3,
        "model dependency order is incorrect");
    auto wrong_build = client;
    wrong_build.build = 0x33535030;
    check(scenes.plan(wrong_build, baseline).snapshot.size() == 2, "model leaked to a non-allowlisted build");
    scenes.sent(models);
    scenes.remove_owner("alpha");
    auto restored = scenes.plan(client, baseline);
    check(restored.snapshot == baseline && restored.updates.size() == 2, "owner removal failed to restore upstream media");

    scenes.replace_catalog(catalog);
    check(scenes.spawn("beta", 10, "persistent", "banner", "teleporter-east", baseline).ok, "second owner spawn failed");
    scenes.sent(scenes.plan(client, baseline));
    scenes.remove_owner("alpha");
    check(scenes.object_count() == 1 && scenes.plan(client, baseline).updates.empty(), "removing one owner damaged another owner");
    scenes.replace_catalog(catalog);
    check(scenes.object_count() == 0 && scenes.plan(client, baseline).updates.size() == 2, "reload lost cleanup snapshot");

    std::vector<MediaUpdate> reserved = baseline;
    reserved.emplace_back(MediaUpdate{1, 0x800, payload("second-upstream")});
    const char* slots[] = {"counter-south", "counter-west", "counter-north", "counter-east", "teleporter-west", "teleporter-east"};
    for (size_t z = 0; z < 6; ++z) {
      check(scenes.spawn("alpha", 10, "object-" + std::to_string(z), "banner", slots[z], reserved).ok, "shared slot budget filled early");
    }
    check(!scenes.spawn("alpha", 10, "ninth", "banner", "floor-banners", reserved).ok, "ninth native group accepted");
    auto invalid = reserved;
    invalid.emplace_back(MediaUpdate{1, 0x40, payload("overlap")});
    auto rejected = scenes.plan(client, invalid);
    check(!rejected.result.ok && rejected.updates.empty(), "invalid baseline was cleared/replayed");
    auto changed = baseline;
    changed.emplace_back(MediaUpdate{1, 1, payload("new-upstream-wins")});
    auto collision = scenes.plan(client, changed);
    check(collision.snapshot[1] == changed[1] && collision.snapshot.size() == 7, "new upstream slot did not take priority");

    scenes.replace_catalog(catalog);
    for (int64_t lobby = 1; lobby <= 32; ++lobby) {
      check(scenes.spawn("alpha", lobby, "quota", "banner", "counter-west", {}).ok, "owner object quota filled early");
    }
    check(!scenes.spawn("alpha", 33, "quota", "banner", "counter-west", {}).ok, "owner object quota bypass");
    auto oversized = catalog;
    oversized.at({"alpha", "banner"}).texture = payload(std::string(0x3801, 'x'));
    rejects([&]() { scenes.replace_catalog(oversized); });
    check(scenes.object_count() == 32, "invalid replacement destroyed active registry");
    auto missing = catalog;
    missing.at({"alpha", "statue"}).texture.reset();
    rejects([&]() { scenes.replace_catalog(missing); });
    auto no_builds = catalog;
    no_builds.at({"alpha", "statue"}).model_builds.clear();
    rejects([&]() { scenes.replace_catalog(no_builds); });
    scenes.prune_lobbies({1, 2});
    check(scenes.object_count() == 2, "destroyed lobbies retained decorations");

    check(scenes.set_event("alpha", 1, 0, 1).ok, "event ownership failed");
    check(!scenes.set_event("beta", 1, 1, 3).ok, "event ownership conflict ignored");
    check(scenes.set_event("alpha", 1, 1, 4).ok, "owned event update failed");
    check(scenes.release_event("alpha", 1, 4) == 0, "event restoration lost original event");
    scenes.set_event("alpha", 1, 0, 1);
    check(!scenes.release_event("alpha", 1, 5), "cleanup overwrote an administrator's intervening event");
    scenes.set_event("alpha", 1, 0, 1);
    scenes.set_event("alpha", 1, 5, 4);
    check(scenes.release_event("alpha", 1, 4) == 5, "manual event did not become restoration baseline");
    check(!scenes.set_event("alpha", 1, 0, 2).ok && !scenes.set_event("alpha", 1, 0, 15).ok, "unknown event accepted");

    auto valid = gvm();
    validate_native_texture(valid);
    rejects([&]() { validate_native_texture(valid.substr(0, valid.size() - 1)); });
    auto corrupt = valid;
    put32(corrupt, 4, 0xFFFFFFF0);
    rejects([&]() { validate_native_texture(corrupt); });
    corrupt = valid;
    corrupt[0x5A] = 1;
    rejects([&]() { validate_native_texture(corrupt); });
    corrupt = valid;
    corrupt[0x5B] = 8;
    rejects([&]() { validate_native_texture(corrupt); });
    rejects([&]() { validate_native_texture(valid + "extra"); });
    inspect_experimental_model(std::string("NJBM\0\0\0\0", 8), 2);
    inspect_experimental_model(std::string("NJCA\0\0\0\0", 8), 3);
    rejects([&]() { inspect_experimental_model("not a model", 2); });
    rejects([&]() { inspect_experimental_model(std::string(0x37000, 'x'), 2); });

    std::string bitmap(54 + 8 * 8 * 3, '\0');
    bitmap.replace(0, 2, "BM");
    put32(bitmap, 10, 54);
    put32(bitmap, 14, 40);
    put32(bitmap, 18, 8);
    put32(bitmap, 22, 8);
    put16(bitmap, 26, 1);
    put16(bitmap, 28, 24);
    validate_banner_bitmap(bitmap);
    put32(bitmap, 18, 0x7FFFFFFF);
    rejects([&]() { validate_banner_bitmap(bitmap); });
    std::cout << "Scene registry, recipient planning, ownership, and native-format tests passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
