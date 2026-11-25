#include "CaelumScenes.hh"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "../CaelumExt.h"

namespace teaserv::extensions {
namespace {

bool valid_name(const std::string& name, size_t max) {
  return !name.empty() && name.size() <= max && std::all_of(name.begin(), name.end(), [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
  });
}

bool finite_in_range(double v, double limit) {
  return std::isfinite(v) && v > -limit && v < limit;
}

std::string number(double v) {
  char buffer[40];
  std::snprintf(buffer, sizeof(buffer), "%.3f", v);
  return buffer;
}

} // namespace

bool caelum_valid_file(const std::string& name) {
  return name.find('\0') == std::string::npos && cext_valid_file(name.c_str());
}

CaelumResult CaelumScenes::set_object(const std::string& owner, uint8_t lobby, const std::string& key, const CaelumObjectSpec& spec) {
  if (lobby > LAST_LOBBY) return {false, "invalid_lobby"};   // also NO_LOBBY
  if (!valid_name(owner, 64) || !valid_name(key, MAX_KEY_BYTES)) return {false, "invalid_key"};
  if (!caelum_valid_file(spec.model) || (!spec.part.empty() && !caelum_valid_file(spec.part)) ||
      (!spec.texture.empty() && !caelum_valid_file(spec.texture)) ||
      (!spec.motion.empty() && !caelum_valid_file(spec.motion)) ||
      (!spec.move_motion.empty() && !caelum_valid_file(spec.move_motion))) return {false, "invalid_file"};
  if (!finite_in_range(spec.x, 1.0e6) || !finite_in_range(spec.y, 1.0e6) || !finite_in_range(spec.z, 1.0e6) ||
      !finite_in_range(spec.rot_y, 36000.0) || !finite_in_range(spec.rot_x, 36000.0) || !finite_in_range(spec.rot_z, 36000.0) ||
      !std::isfinite(spec.scale) || spec.scale < 0.001 || spec.scale > 1000.0 || spec.start_ms < 0) return {false, "invalid_value"};
  // Attachment: a bone is -1..63; upright needs a bone, follow and move_motion the root; none of them without a player.
  if (spec.bone < -1 || spec.bone > CEXT_MAX_BONE || (spec.upright && spec.bone < 0) || (spec.follow && spec.bone >= 0) ||
      (!spec.move_motion.empty() && !spec.follow) ||
      (!spec.attach && (spec.bone >= 0 || spec.upright || spec.follow))) return {false, "invalid_value"};
  auto& state = this->lobbies[lobby];
  for (auto& object : state.objects) {
    if (object.owner == owner && object.key == key) {
      object.spec = spec;
      return {true, nullptr};
    }
  }
  size_t owned = 0;
  for (const auto& [number, other] : this->lobbies) {
    (void)number;
    for (const auto& object : other.objects) owned += object.owner == owner;
  }
  if (state.objects.size() >= MAX_OBJECTS_PER_LOBBY) return {false, "lobby_full"};
  if (owned >= MAX_OBJECTS_PER_OWNER) return {false, "package_quota"};
  state.objects.emplace_back(Object{owner, key, spec});
  return {true, nullptr};
}

bool CaelumScenes::remove_object(const std::string& owner, uint8_t lobby, const std::string& key) {
  auto it = this->lobbies.find(lobby);
  if (it == this->lobbies.end()) return false;
  auto& objects = it->second.objects;
  auto found = std::find_if(objects.begin(), objects.end(), [&](const Object& o) { return o.owner == owner && o.key == key; });
  if (found == objects.end()) return false;
  objects.erase(found);
  return true;
}

CaelumResult CaelumScenes::set_music(const std::string& owner, uint8_t lobby, const CaelumMusicSpec& spec) {
  if (lobby > LAST_LOBBY) return {false, "invalid_lobby"};   // also NO_LOBBY
  if (!valid_name(owner, 64)) return {false, "invalid_key"};
  if (!caelum_valid_file(spec.file)) return {false, "invalid_file"};
  if (spec.start_ms < 0) return {false, "invalid_value"};
  auto& state = this->lobbies[lobby];
  // One track per lobby. Another package's music is not replaced.
  if (state.music && state.music->owner != owner) return {false, "music_owned"};
  state.music = Music{owner, spec};
  return {true, nullptr};
}

bool CaelumScenes::stop_music(const std::string& owner, uint8_t lobby) {
  auto it = this->lobbies.find(lobby);
  if (it == this->lobbies.end() || !it->second.music || it->second.music->owner != owner) return false;
  it->second.music.reset();
  return true;
}

bool CaelumScenes::clear(const std::string& owner, uint8_t lobby) {
  auto it = this->lobbies.find(lobby);
  if (it == this->lobbies.end()) return false;
  bool changed = std::erase_if(it->second.objects, [&](const Object& o) { return o.owner == owner; }) > 0;
  if (it->second.music && it->second.music->owner == owner) {
    it->second.music.reset();
    changed = true;
  }
  return changed;
}

std::set<uint8_t> CaelumScenes::remove_owner(const std::string& owner) {
  std::set<uint8_t> changed;
  for (auto& [number, state] : this->lobbies) {
    (void)state;
    if (this->clear(owner, number)) changed.emplace(number);
  }
  return changed;
}

size_t CaelumScenes::object_count() const {
  size_t count = 0;
  for (const auto& [number, state] : this->lobbies) {
    (void)number;
    count += state.objects.size();
  }
  return count;
}

bool CaelumScenes::empty(uint8_t lobby) const {
  auto it = this->lobbies.find(lobby);
  return it == this->lobbies.end() || (it->second.objects.empty() && !it->second.music);
}

std::string CaelumScenes::snapshot(uint8_t lobby, const std::string& room, int64_t now_ms, size_t max_objects,
    const std::function<int(uint64_t)>& slot_of, bool attached) const {
  std::string text = "v=1\nroom=" + room + "\nnow=" + std::to_string(now_ms) + "\n";
  auto it = this->lobbies.find(lobby);
  if (it == this->lobbies.end()) return text;
  std::string music;
  if (it->second.music) {
    const auto& m = it->second.music->spec;
    music = "music " + m.file + " start=" + std::to_string(m.start_ms) + " loop=" + (m.loop ? "1" : "0") + "\n";
  }
  size_t written = 0;
  for (const auto& object : it->second.objects) {
    if (written >= max_objects) break;
    const auto& s = object.spec;
    int slot = -1;
    if (s.attach) {
      if (!attached || !slot_of) continue;
      slot = slot_of(s.attach);
      if (slot < 0 || slot > 11) continue; // that player is not in this room
    }
    std::string line = "obj " + object.owner + ":" + object.key + " model=" + s.model;
    if (!s.part.empty()) line += " part=" + s.part;
    if (!s.texture.empty()) line += " tex=" + s.texture;
    if (!s.motion.empty()) line += " motion=" + s.motion;
    line += " x=" + number(s.x) + " y=" + number(s.y) + " z=" + number(s.z) + " ry=" + number(s.rot_y);
    if (s.rot_x != 0) line += " rx=" + number(s.rot_x);
    if (s.rot_z != 0) line += " rz=" + number(s.rot_z);
    line += " scale=" + number(s.scale) + " start=" + std::to_string(s.start_ms) + " loop=" + (s.loop ? "1" : "0") +
        " show=" + (s.visible ? "1" : "0");
    if (s.attach) {
      line += " attach=" + std::to_string(slot) + " bone=" + std::to_string(s.bone);
      if (s.upright) line += " upright=1";
      if (s.follow) line += " follow=1";
      if (!s.move_motion.empty()) line += " move=" + s.move_motion;
    }
    line += "\n";
    if (text.size() + line.size() + music.size() > CEXT_MAX_SNAPSHOT) break; // the client refuses a longer room
    text += line;
    ++written;
  }
  return text + music;
}

std::set<uint8_t> CaelumScenes::remove_attached(uint64_t session) {
  std::set<uint8_t> changed;
  if (!session) return changed;
  for (auto& [number, state] : this->lobbies) {
    auto& objects = state.objects;
    auto end = std::remove_if(objects.begin(), objects.end(), [&](const Object& o) { return o.spec.attach == session; });
    if (end != objects.end()) {
      objects.erase(end, objects.end());
      changed.insert(number);
    }
  }
  return changed;
}

std::vector<std::string> CaelumScenes::describe(const std::string& owner) const {
  std::vector<std::string> lines;
  for (const auto& [number, state] : this->lobbies) {
    for (const auto& object : state.objects) {
      if (object.owner != owner) continue;
      lines.emplace_back("Caelum object " + object.key + ": lobby " + std::to_string(number) + ", model " + object.spec.model +
          (object.spec.motion.empty() ? "" : ", motion " + object.spec.motion) + (object.spec.visible ? "" : " (prefetch only)"));
    }
    if (state.music && state.music->owner == owner) {
      lines.emplace_back("Caelum music: lobby " + std::to_string(number) + ", " + state.music->spec.file);
    }
  }
  return lines;
}

} // namespace teaserv::extensions
