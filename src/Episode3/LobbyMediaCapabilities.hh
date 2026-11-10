#pragma once

#include "../Version.hh"
#include "LobbyMedia.hh"

namespace Episode3 {

// Two wire bytes in 9D/9E unused3, not an integer, bitmask, or version range.
constexpr bool lobby_media_login_marker(uint8_t family, uint8_t profile) {
  return family == 0xE3 && (profile == 0x01 || profile == 0x02);
}

// Shared by every native-media recipient path, including extensions-OFF builds.
constexpr LobbyMediaProfile lobby_media_profile_for_session(
    Version version, bool authenticated, bool proxy, bool replay,
    bool advertised_pc_media) {
  return version == Version::GC_EP3 && authenticated && !proxy && !replay &&
          advertised_pc_media
      ? LobbyMediaProfile::PC_PORT
      : LobbyMediaProfile::ORIGINAL;
}

} // namespace Episode3
