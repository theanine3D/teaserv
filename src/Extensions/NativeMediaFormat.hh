#pragma once

#include <cstdint>
#include <string_view>
#include "../Episode3/LobbyMedia.hh"

namespace teaserv::extensions {

// Intentionally strict subset: complete GVM headers with unmipmapped RGB565 or
// RGB5A3 textures, matching the existing Newserv encoder. Throws on malformed or
// unsupported input rather than passing opaque bytes to original clients.
void validate_native_texture(std::string_view data, Episode3::LobbyMediaProfile profile = Episode3::LobbyMediaProfile::ORIGINAL);
void validate_banner_bitmap(std::string_view data);

// Only identifies documented media tokens. This does NOT validate the nested
// model/animation pointers, materials, or rendering behavior; use only behind
// the explicit experimental switch and an exact client-build allowlist.
void inspect_experimental_model(std::string_view data, uint32_t type,
    Episode3::LobbyMediaProfile profile = Episode3::LobbyMediaProfile::ORIGINAL);

} // namespace teaserv::extensions
