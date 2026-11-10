#include "NativeMediaFormat.hh"

#include <cstring>
#include <stdexcept>

#include "Scene.hh"

namespace teaserv::extensions {
namespace {

uint16_t u16(std::string_view data, size_t offset, bool big_endian) {
  if (offset > data.size() || data.size() - offset < 2) {
    throw std::runtime_error("truncated native media field");
  }
  auto a = static_cast<uint8_t>(data[offset]);
  auto b = static_cast<uint8_t>(data[offset + 1]);
  return big_endian ? (a << 8) | b : (b << 8) | a;
}

uint32_t u32(std::string_view data, size_t offset) {
  return u16(data, offset, false) | (static_cast<uint32_t>(u16(data, offset + 2, false)) << 16);
}

bool dimension(uint32_t value) { return value >= 8 && value <= 256 && !(value & (value - 1)); }

} // namespace

void validate_banner_bitmap(std::string_view data) {
  if (data.size() < 54 || data.substr(0, 2) != "BM") {
    throw std::runtime_error("banner must be an uncompressed Windows BMP");
  }
  auto header_size = u32(data, 14);
  auto offset = u32(data, 10);
  auto width = u32(data, 18);
  auto height = u32(data, 22);
  auto bits = u16(data, 28, false);
  if (header_size < 40 || header_size > data.size() - 14 || offset < 14 + header_size ||
      !dimension(width) || height != width || u16(data, 26, false) != 1 || (bits != 24 && bits != 32) || u32(data, 30)) {
    throw std::runtime_error("BMP must be square 8..256 pixels, positive-height, uncompressed RGB24/RGBA32");
  }
  size_t stride = ((width * bits + 31) / 32) * 4;
  if (offset > data.size() || stride * height > data.size() - offset) {
    throw std::runtime_error("BMP pixel data extends beyond file");
  }
}

void validate_native_texture(std::string_view data, Episode3::LobbyMediaProfile profile) {
  if (data.size() < 12 || data.size() > Episode3::lobby_media_decompressed_limit(profile) || data.substr(0, 4) != "GVMH") {
    throw std::runtime_error("invalid or oversized native GVM texture set");
  }
  uint32_t header_size = u32(data, 4);
  uint16_t flags = u16(data, 8, true);
  uint16_t count = u16(data, 10, true);
  if (flags != 0x000F || count == 0 || count > 16 || header_size < 4U + 0x26U * count ||
      header_size > data.size() - 8) {
    throw std::runtime_error("GVM requires a complete, bounded texture metadata table (flags 0xF)");
  }
  size_t offset = 8 + header_size;
  for (size_t index = 0; index < count; ++index) {
    size_t entry = 12 + index * 0x26;
    if (u16(data, entry, true) != index || !std::memchr(data.data() + entry + 2, 0, 28) ||
        offset > data.size() || data.size() - offset < 16 || data.substr(offset, 4) != "GVRT") {
      throw std::runtime_error("invalid GVM entry index/name or missing GVRT payload");
    }
    uint32_t size = u32(data, offset + 4);
    uint32_t width = u16(data, offset + 12, true);
    uint32_t height = u16(data, offset + 14, true);
    uint8_t format = data[offset + 11];
    uint16_t exponent = 0;
    for (auto dimension = width; dimension > 4; dimension >>= 1) {
      ++exponent;
    }
    if (!dimension(width) || height != width || data[offset + 10] || (format != 4 && format != 5) ||
        data[entry + 30] || static_cast<uint8_t>(data[entry + 31]) != format ||
        u16(data, entry + 32, true) != ((exponent << 4) | exponent) ||
        size != 8 + width * height * 2 || size > data.size() - offset - 8) {
      throw std::runtime_error("unsupported or inconsistent GVR format/dimensions/length");
    }
    offset += size + 8;
  }
  if (offset != data.size()) {
    throw std::runtime_error("unexpected trailing GVM data");
  }
}

void inspect_experimental_model(std::string_view data, uint32_t type, Episode3::LobbyMediaProfile profile) {
  if ((type != 2 && type != 3) || data.size() < 8 || data.size() > Episode3::lobby_media_decompressed_limit(profile)) {
    throw std::runtime_error("invalid model/animation media size or type");
  }
  for (size_t offset = 0; offset + 8 <= data.size(); offset += 4) {
    auto token = data.substr(offset, 4);
    if (type == 2 && (token == "GJBM" || token == "GMDM" || token == "GSSM" || token == "NJBM" || token == "NMDM" || token == "NSSM")) {
      return;
    }
    if (type == 3 && (token == "GCAM" || token == "GLIM" || token == "NCAM" || token == "NJCA" || token == "NLIM")) {
      return;
    }
  }
  throw std::runtime_error("no documented model/animation media token found");
}

} // namespace teaserv::extensions
