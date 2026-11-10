#include "NativeAssets.hh"

#include <stdexcept>

#include "../Compression.hh"
#include "../ImageEncoder.hh"
#include "NativeMediaFormat.hh"
#include "Scene.hh"

namespace teaserv::extensions {

std::shared_ptr<const std::string> prepare_native_media(
    const std::string& source, const std::string& name, uint32_t type, uint32_t texture_index,
    Episode3::LobbyMediaProfile profile) {
  if (type < 1 || type > 3) {
    throw std::runtime_error("unsupported native media asset type");
  }
  std::string raw;
  std::string compressed;
  if (name.ends_with(".prs")) {
    if ((type == 1 && !name.ends_with(".gvm.prs")) || (type != 1 && !name.ends_with(".bml.prs"))) {
      throw std::runtime_error("PRS asset filename does not match its native media type");
    }
    if (source.empty() || source.size() > Episode3::lobby_media_compressed_limit(profile)) {
      throw std::runtime_error("compressed native media exceeds the selected client profile");
    }
    auto decoded = prs_decompress_with_meta(source, Episode3::lobby_media_decompressed_limit(profile));
    if (decoded.input_bytes_used != source.size()) {
      throw std::runtime_error("trailing bytes after PRS terminator");
    }
    raw = std::move(decoded.data);
  } else if (type == 1 && name.ends_with(".bmp")) {
    validate_banner_bitmap(source); // Check dimensions before allocating an image.
    auto image = phosg::ImageRGBA8888N::from_file_data(source);
    raw = encode_gvm(image, has_any_transparent_pixels(image) ? GVRDataFormat::RGB5A3 : GVRDataFormat::RGB565,
        "ext" + std::to_string(texture_index), texture_index);
  } else if ((type == 1 && name.ends_with(".gvm")) || (type != 1 && name.ends_with(".bml"))) {
    raw = source;
  } else {
    throw std::runtime_error("native assets require .bmp/.gvm/.gvm.prs textures or .bml/.bml.prs models/animations");
  }
  if (raw.empty() || raw.size() > Episode3::lobby_media_decompressed_limit(profile)) {
    throw std::runtime_error("native media exceeds the selected decompressed-size limit");
  }
  if (type == 1) {
    validate_native_texture(raw, profile);
  } else {
    inspect_experimental_model(raw, type, profile);
  }
  // Normalize every input through upstream's compressor, which emits an explicit
  // stop opcode even if the supplied stream ended cleanly at an opcode boundary.
  compressed = prs_compress(raw);
  if (compressed.empty() || compressed.size() > Episode3::lobby_media_compressed_limit(profile)) {
    throw std::runtime_error("native media cannot fit the selected compressed-size limit");
  }
  return std::make_shared<const std::string>(std::move(compressed));
}

} // namespace teaserv::extensions
