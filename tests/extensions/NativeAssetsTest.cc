#include "../../src/Extensions/NativeAssets.hh"
#include "../../src/Compression.hh"
#include "../../src/Extensions/NativeMediaFormat.hh"
#include "../../src/Extensions/Scene.hh"

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
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
  check(failed, "invalid native asset was accepted");
}

// Valid RGB565 GVMs; arbitrary packed pixels remain valid after GX swizzling.
static std::string texture_fixture(size_t count, size_t dimension, size_t noise_bytes) {
  size_t header_bytes = (12 + count * 0x26 + 15) & ~size_t(15);
  std::string data(header_bytes, '\0');
  auto put16 = [&](size_t offset, uint16_t value) {
    data[offset] = value >> 8;
    data[offset + 1] = value;
  };
  auto put32le = [&](size_t offset, uint32_t value) {
    for (size_t z = 0; z < 4; ++z)
      data[offset + z] = value >> (z * 8);
  };
  data.replace(0, 4, "GVMH");
  put32le(4, header_bytes - 8);
  put16(8, 15);
  put16(10, count);
  uint16_t exponent = 0;
  for (size_t z = dimension; z > 4; z >>= 1)
    ++exponent;
  uint32_t random = 0x12345678;
  for (size_t index = 0; index < count; ++index) {
    size_t entry = 12 + index * 0x26;
    put16(entry, index);
    data.replace(entry + 2, 8, "fixture" + std::to_string(index));
    data[entry + 31] = 4;
    put16(entry + 32, (exponent << 4) | exponent);
    put16(entry + 34, 0);
    put16(entry + 36, (count == 1 ? 0x180 : 0x190) + index);
    size_t offset = data.size();
    data.resize(offset + 16 + dimension * dimension * 2, '\0');
    data.replace(offset, 4, "GVRT");
    put32le(offset + 4, 8 + dimension * dimension * 2);
    data[offset + 11] = 4;
    put16(offset + 12, dimension);
    put16(offset + 14, dimension);
    for (size_t z = 0; z < dimension * dimension * 2 && noise_bytes; ++z, --noise_bytes) {
      random ^= random << 13;
      random ^= random >> 17;
      random ^= random << 5;
      data[offset + 16 + z] = random;
    }
  }
  return data;
}

int main(int argc, char** argv) {
  try {
    check(argc == 2 || argc == 3, "expected example BMP fixture path and optional output directory");
    std::ifstream file(argv[1], std::ios::binary);
    check(file.good(), "cannot open example BMP");
    std::string bitmap((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    validate_banner_bitmap(bitmap);
    auto prepared = prepare_native_media(bitmap, "example.bmp", 1, 0x100);
    check(prepared->size() <= SceneService::MAX_MEDIA_BYTES, "prepared texture exceeded native packet budget");
    auto decoded = prs_decompress_with_meta(*prepared, SceneService::MAX_DECOMPRESSED_BYTES);
    check(decoded.input_bytes_used == prepared->size(), "prepared PRS did not consume exact payload");
    validate_native_texture(decoded.data);
    auto raw = prepare_native_media(decoded.data, "example.gvm", 1, 0x100);
    auto recompressed = prepare_native_media(*prepared, "example.gvm.prs", 1, 0x100);
    check(prs_decompress(*raw) == decoded.data && prs_decompress(*recompressed) == decoded.data, "asset preparation changed texture data");
    rejects([&]() { prepare_native_media(*prepared + "x", "extra.gvm.prs", 1, 0x100); });
    rejects([&]() { prepare_native_media(prepared->substr(0, prepared->size() / 2), "truncated.gvm.prs", 1, 0x100); });
    rejects([&]() { prepare_native_media(*prepared, "wrong.bin.prs", 1, 0x100); });
    rejects([&]() { prepare_native_media(std::string(0x3801, 'x'), "large.gvm.prs", 1, 0x100); });
    auto corrupt = bitmap;
    corrupt[18] = 0;
    corrupt[19] = 0;
    corrupt[20] = 0x40; // Reject a huge image before the image decoder allocates it.
    rejects([&]() { prepare_native_media(corrupt, "huge.bmp", 1, 0x100); });
    rejects([&]() { prepare_native_media(bitmap.substr(0, 54), "short.bmp", 1, 0x100); });
    rejects([&]() { prepare_native_media("not a GVM", "broken.gvm", 1, 0x100); });

    constexpr auto pc = Episode3::LobbyMediaProfile::PC_PORT;
    auto large_texture = texture_fixture(1, 128, 18000);
    auto large_prepared = prepare_native_media(large_texture, "pc-noise.gvm", 1, 0x100, pc);
    check(large_prepared->size() > 0x3800 && large_prepared->size() <= 0x7BF0, "fixture does not exercise the larger compressed envelope");
    rejects([&]() { prepare_native_media(large_texture, "pc-noise.gvm", 1, 0x100); });
    rejects([&]() { prepare_native_media(*large_prepared, "pc-noise.gvm.prs", 1, 0x100); });
    auto multi_texture = texture_fixture(2, 256, 0);
    auto multi_prepared = prepare_native_media(multi_texture, "pc-multi.gvm", 1, 0x100, pc);
    check(multi_texture.size() > 0x37000 && multi_prepared->size() <= 0x3800, "fixture does not exercise decompressed-only incompatibility");
    rejects([&]() { prepare_native_media(*multi_prepared, "pc-multi.gvm.prs", 1, 0x100); });
    check(prs_decompress(*large_prepared) == large_texture && prs_decompress(*multi_prepared) == multi_texture, "PC preparation changed GVM bytes");
    rejects([&]() { prepare_native_media(std::string(0x7BF1, 'x'), "oversized.gvm.prs", 1, 0x100, pc); });
    rejects([&]() { prepare_native_media(std::string(16 * 1024 * 1024 + 1, '\0'), "oversized.gvm", 1, 0x100, pc); });
    if (argc == 3) {
      std::filesystem::create_directories(argv[2]);
      auto save = [&](const char* name, const std::string& data) {
        std::ofstream out(std::filesystem::path(argv[2]) / name, std::ios::binary);
        out.write(data.data(), data.size());
        check(out.good(), "cannot write media fixture");
      };
      save("pc-noise.gvm", large_texture);
      save("pc-noise.gvm.prs", *large_prepared);
      save("pc-multi.gvm", multi_texture);
      save("pc-multi.gvm.prs", *multi_prepared);
    }

    // These model bytes are synthetic parser/limit fixtures, NOT renderable assets.
    std::string model(SceneService::MAX_DECOMPRESSED_BYTES, '\0');
    model.replace(0, 4, "NJBM");
    auto model_prepared = prepare_native_media(model, "synthetic.bml", 2, 0x100);
    check(prs_decompress(*model_prepared).size() == SceneService::MAX_DECOMPRESSED_BYTES, "strict decompressed boundary rejected a valid size");
    model.push_back('\0'); // Exactly 0x37000 is forbidden, unlike the old banner loader's loose comparison.
    rejects([&]() { prepare_native_media(model, "too-large.bml", 2, 0x100); });
    auto bomb = prs_compress(model);
    rejects([&]() { prepare_native_media(bomb, "expansion.bml.prs", 2, 0x100); });
    rejects([&]() { prepare_native_media("not a model", "broken.bml", 2, 0x100); });
    auto animation = prepare_native_media(std::string("NJCA\0\0\0\0", 8), "synthetic.bml", 3, 0x100);
    check(prs_decompress(*animation).size() == 8, "animation transport preparation failed");
    std::cout << "Native BMP/GVM/PRS preparation and strict size tests passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
