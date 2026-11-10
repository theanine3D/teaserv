#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace Episode3 {

// The 16-byte wire prefix includes the four-byte game command header.
constexpr size_t LOBBY_MEDIA_MAX_PACKET = 0x7C00;
constexpr size_t LOBBY_MEDIA_ORIGINAL_COMPRESSED = 0x3800;
constexpr size_t LOBBY_MEDIA_ORIGINAL_DECOMPRESSED = 0x37000 - 1;
constexpr size_t LOBBY_MEDIA_PC_COMPRESSED = LOBBY_MEDIA_MAX_PACKET - 0x10;
constexpr size_t LOBBY_MEDIA_PC_DECOMPRESSED = 16 * 1024 * 1024;

enum class LobbyMediaProfile { ORIGINAL,
  PC_PORT };

struct LobbyMediaCompatibility {
  bool gc_ok = false;
  bool pc_ok = false;
  bool operator==(const LobbyMediaCompatibility&) const = default;
  bool accepts(LobbyMediaProfile profile) const {
    return profile == LobbyMediaProfile::PC_PORT ? this->pc_ok : this->gc_ok;
  }
};

constexpr LobbyMediaCompatibility lobby_media_compatibility(size_t compressed, size_t decompressed) {
  return {
      compressed > 0 && compressed <= LOBBY_MEDIA_ORIGINAL_COMPRESSED &&
          decompressed > 0 && decompressed <= LOBBY_MEDIA_ORIGINAL_DECOMPRESSED,
      compressed > 0 && compressed <= LOBBY_MEDIA_PC_COMPRESSED &&
          decompressed > 0 && decompressed <= LOBBY_MEDIA_PC_DECOMPRESSED};
}

constexpr size_t lobby_media_compressed_limit(LobbyMediaProfile profile) {
  return profile == LobbyMediaProfile::PC_PORT ? LOBBY_MEDIA_PC_COMPRESSED : LOBBY_MEDIA_ORIGINAL_COMPRESSED;
}
constexpr size_t lobby_media_decompressed_limit(LobbyMediaProfile profile) {
  return profile == LobbyMediaProfile::PC_PORT ? LOBBY_MEDIA_PC_DECOMPRESSED : LOBBY_MEDIA_ORIGINAL_DECOMPRESSED;
}

// A fallback has already passed ORIGINAL compressed/decompressed validation.
// Keep variants together until recipient selection, then strip fallback metadata.
struct LobbyMediaUpdate {
  uint32_t type = 0;
  uint32_t locations = 0;
  std::shared_ptr<const std::string> data;
  LobbyMediaCompatibility compatibility{true, true};
  std::shared_ptr<const std::string> fallback_data = nullptr;
  bool operator==(const LobbyMediaUpdate& other) const {
    auto equal_data = [](const auto& a, const auto& b) {
      return a == b || (a && b && *a == *b);
    };
    return this->type == other.type && this->locations == other.locations &&
        this->compatibility == other.compatibility && equal_data(this->data, other.data) &&
        equal_data(this->fallback_data, other.fallback_data);
  }
};

inline std::vector<LobbyMediaUpdate> select_lobby_media(
    LobbyMediaProfile profile, const std::vector<LobbyMediaUpdate>& media) {
  struct Group {
    bool compatible = true;
    bool any_fallback = false;
    bool complete_fallback = true;
  };
  std::map<uint32_t, Group> groups;
  for (const auto& entry : media) {
    auto& group = groups[entry.locations];
    group.compatible &= entry.compatibility.accepts(profile);
    group.any_fallback |= static_cast<bool>(entry.fallback_data);
    group.complete_fallback &= entry.fallback_data && !entry.fallback_data->empty() &&
        entry.fallback_data->size() <= LOBBY_MEDIA_ORIGINAL_COMPRESSED;
  }
  std::vector<LobbyMediaUpdate> result;
  for (const auto& entry : media) {
    const auto& group = groups.at(entry.locations);
    bool fallback = profile == LobbyMediaProfile::ORIGINAL && group.any_fallback;
    if (fallback ? !group.complete_fallback : !group.compatible) {
      continue;
    }
    auto selected = entry;
    if (fallback) {
      selected.data = entry.fallback_data;
      selected.compatibility = {true, true};
    }
    selected.fallback_data.reset();
    result.emplace_back(std::move(selected));
  }
  return result;
}

inline size_t lobby_media_packet_size(size_t compressed) {
  if (compressed > LOBBY_MEDIA_PC_COMPRESSED) {
    throw std::runtime_error("B9 exceeds the 0x7C00-byte packet limit");
  }
  return 0x10 + ((compressed + 3) & ~static_cast<size_t>(3));
}

// Body only: send_command supplies the existing four-byte B9 command header.
inline std::string encode_lobby_media_body(uint32_t type, uint32_t locations, std::string_view compressed) {
  if (type < 1 || type > 4 || (type == 4 ? !compressed.empty() : compressed.empty())) {
    throw std::runtime_error("invalid B9 media type or payload");
  }
  size_t packet_size = lobby_media_packet_size(compressed.size());
  std::string body(12, '\0');
  for (size_t z = 0; z < 4; ++z) {
    body[z] = type >> (z * 8);
    body[4 + z] = locations >> (z * 8);
  }
  body[8] = compressed.size();
  body[9] = compressed.size() >> 8;
  body.append(compressed);
  body.resize(packet_size - 4, '\0');
  return body;
}

} // namespace Episode3
