#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include "../Episode3/LobbyMedia.hh"

namespace teaserv::extensions {

// Uses upstream PRS and BMP/GVM encoding. All work happens during staging, not
// during a player callback. source_name determines .bmp/.gvm/.bml[.prs] handling.
std::shared_ptr<const std::string> prepare_native_media(
    const std::string& source, const std::string& source_name, uint32_t type, uint32_t texture_index,
    Episode3::LobbyMediaProfile profile = Episode3::LobbyMediaProfile::ORIGINAL);

} // namespace teaserv::extensions
