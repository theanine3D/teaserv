#include <phosg/UnitTest.hh>

#include "QuestScript.hh"
#include "RareItemSet.hh"
#include "Episode3/LobbyMedia.hh"
#include "Episode3/LobbyMediaCapabilities.hh"
#include "Episode3/Modpacks.hh"
#include "Episode3/CaelumLobbies.hh"
#include "CommandFormats.hh"
#include "PSOProtocol.hh"
#include "Text.hh"

void run_static_tests() {
  phosg::log_info_f("-- Quest opcode definitions");
  check_default_quest_opcode_definitions();

  phosg::log_info_f("-- RareItemSet rate calculations");
  for (size_t z = 0; z < 0x100; z++) {
    uint32_t expanded = RareItemSet::expand_rate(z);
    uint8_t reencoded = RareItemSet::compress_rate(expanded);
    if (z == 0) {
      expect_eq(reencoded, 0);
    } else if ((reencoded < 0x28) && (z < 0x28)) {
      if ((z & 7) == 0) {
        expect_eq(1, reencoded & 7);
      } else {
        expect_eq(z & 7, reencoded & 7);
      }
    } else if (z == 0x28) {
      expect_eq(7, reencoded);
    } else if ((z & 7) == 0) {
      expect_eq(z - 1, reencoded);
    } else {
      expect_eq(z, reencoded);
    }
  }

  phosg::log_info_f("-- Sega Shift-JIS custom characters");
  TextTranscoder& sega_decoder = tt_sega_sjis_to_utf8;
  for (uint8_t low = 0x40; low <= 0x64; low++) {
    std::string encoded{static_cast<char>(0xF0), static_cast<char>(low)};
    uint32_t codepoint = low == 0x40 ? 0x2665 : low == 0x41 ? 0x24EA :
        low <= 0x4A ? 0x2460 + low - 0x42 : 0x1D4D0 + low - 0x4B;
    auto expected = encode_utf8_char(codepoint);
    expect_eq(sega_decoder(encoded), expected);
    expect_eq(sega_decoder("before" + encoded + "after"), "before" + expected + "after");
    expect_eq(tt_utf8_to_sega_sjis(expected), encoded);
    char buffer[4];
    auto result = sega_decoder(buffer, expected.size(), encoded.data(), encoded.size(), false);
    expect_eq(result.bytes_read, encoded.size());
    expect_eq(std::string(buffer, result.bytes_written), expected);
    expect_raises(std::runtime_error, [&] {
      sega_decoder(buffer, expected.size() - 1, encoded.data(), encoded.size(), false);
    });
    result = sega_decoder(buffer, expected.size() - 1, encoded.data(), encoded.size(), true);
    expect_eq(result.bytes_written, 0);
  }
  // F0 can also be a trail byte in an ordinary character (the angstrom sign).
  std::string ordinary = "\x81\xF0@";
  expect_eq(sega_decoder(ordinary), tt_standard_sjis_to_utf8(ordinary));
  expect_eq(sega_decoder("plain ASCII"), "plain ASCII");
  expect_raises(std::runtime_error, [&] { sega_decoder("\xF0"); });

  phosg::log_info_f("-- Caelum extra lobbies");
  {
    auto extras = caelum::lobbies::parse("# extra lobbies\r\n20 LOBBY-CARD06\n\n21\t  Second lobby  \n");
    expect_eq(2, extras.size());
    expect_eq(20, extras[0].number);
    expect_eq("LOBBY-CARD06", extras[0].name);
    expect_eq(21, extras[1].number);
    expect_eq("Second lobby", extras[1].name);
    expect_eq("LOBBY23", caelum::lobbies::parse("20 a\n21 b\n22\n")[2].name);
    expect(caelum::lobbies::parse("").empty());
    expect_raises(std::runtime_error, [&] { caelum::lobbies::parse("21 x\n"); });            // must start at 20
    expect_raises(std::runtime_error, [&] { caelum::lobbies::parse("20 x\n22 y\n"); });     // no gaps
    expect_raises(std::runtime_error, [&] { caelum::lobbies::parse("20 x\n20 y\n"); });     // no repeats
    expect_raises(std::runtime_error, [&] { caelum::lobbies::parse("19 x\n"); });            // stock numbers
    expect_raises(std::runtime_error, [&] { caelum::lobbies::parse("x20 a\n"); });
    expect_raises(std::runtime_error, [&] { caelum::lobbies::parse("20 " + std::string(33, 'n') + "\n"); });
    expect_raises(std::runtime_error, [&] { caelum::lobbies::parse("20 a\x01\n"); });
    std::string all;
    for (int n = 20; n <= 63; n++) {
      all += std::to_string(n) + " L\n";
    }
    expect_eq(44, caelum::lobbies::parse(all).size());
    expect_raises(std::runtime_error, [&] { caelum::lobbies::parse(all + "64 L\n"); });     // Caelum keeps 63
    expect(caelum::lobbies::visible_to(Version::GC_EP3, true));
    expect(!caelum::lobbies::visible_to(Version::GC_EP3, false));
    expect(!caelum::lobbies::visible_to(Version::GC_EP3_NTE, true));
    expect(!caelum::lobbies::visible_to(Version::GC_V3, true));
  }
  phosg::log_info_f("-- Caelum server mod manifests");
  const std::string mod_hash(64,'a');
  auto mods=caelum::mods::parse_manifest("CMOD1\n"+mod_hash+"\tExample\thttps://example.test/mod.zip\n");
  expect_eq(mods.size(),1);
  expect_eq(mods[0].sha256,mod_hash);
  expect(caelum::mods::parse_manifest("CMOD1\r\n").empty());
  expect_raises(std::runtime_error,[&]{caelum::mods::parse_manifest("CMOD1\n"+mod_hash+"\tx\tfile:///x\n");});
  expect_raises(std::runtime_error,[&]{caelum::mods::parse_manifest("CMOD1\ninvalid\tx\thttps://x\n");});
  expect_raises(std::runtime_error,[&]{caelum::mods::parse_manifest("CMOD1\n"+mod_hash+"\tx\thttps://x\n"+mod_hash+"\ty\thttps://y\n");});

  phosg::log_info_f("-- Episode 3 login media capability");
  for (size_t pair = 0; pair < 0x10000; ++pair) {
    expect_eq(Episode3::lobby_media_login_marker(pair >> 8, pair & 0xFF), (pair == 0xE301 || pair == 0xE302));
  }
  // Validate the agreed raw offsets against every supported GC login layout.
  auto check_login_marker_layout = []<typename T>(uint16_t command) {
    std::string payload(sizeof(T), '\0');
    payload[0x16] = static_cast<char>(0xE3);
    payload[0x17] = 1;
    const auto& cmd = check_size_t<T>(payload);
    expect(Episode3::lobby_media_login_marker(cmd.unused3[0], cmd.unused3[1]));
    auto packet = prepend_command_header(Version::GC_EP3, true, command, 0, payload);
    expect_eq(static_cast<uint8_t>(packet[0x1A]), 0xE3);
    expect_eq(static_cast<uint8_t>(packet[0x1B]), 1);
    expect_eq(packet.size(), sizeof(T) + 4);
    payload.resize(sizeof(T) - 1);
    expect_raises(std::runtime_error, [&] { check_size_t<T>(payload); });
  };
  check_login_marker_layout.template operator()<C_Login_DC_PC_GC_9D>(0x9D);
  check_login_marker_layout.template operator()<C_LoginExtended_DC_GC_9D>(0x9D);
  check_login_marker_layout.template operator()<C_Login_PC_GC_9E>(0x9E);
  check_login_marker_layout.template operator()<C_LoginExtended_GC_9E>(0x9E);
  // Exhaust all gates independently, including unsupported sessions.
  for (auto version : ALL_VERSIONS) {
    for (size_t flags = 0; flags < 16; ++flags) {
      bool authenticated = flags & 1;
      bool proxy = flags & 2;
      bool replay = flags & 4;
      bool marker = flags & 8;
      auto profile = Episode3::lobby_media_profile_for_session(
          version, authenticated, proxy, replay, marker);
      bool expected_pc = version == Version::GC_EP3 && authenticated && !proxy && !replay && marker;
      expect_eq(profile == Episode3::LobbyMediaProfile::PC_PORT, expected_pc);
      // Both oversized fixture classes must follow the same recipient decision.
      expect_eq(Episode3::lobby_media_compatibility(20427, 32848).accepts(profile), expected_pc);
      expect_eq(Episode3::lobby_media_compatibility(3403, 262272).accepts(profile), expected_pc);
    }
  }

  phosg::log_info_f("-- Episode 3 complete-group media fallbacks");
  {
    auto data = [](const char* s) { return std::make_shared<const std::string>(s); };
    auto high_texture = data("high-texture"), low_texture = data("low-texture");
    auto high_model = data("high-model"), low_model = data("low-model");
    std::vector<Episode3::LobbyMediaUpdate> variants{
        {1, 1, high_texture, {true, true}, low_texture},
        {2, 1, high_model, {false, true}, low_model}};
    auto original = Episode3::select_lobby_media(Episode3::LobbyMediaProfile::ORIGINAL, variants);
    auto pc = Episode3::select_lobby_media(Episode3::LobbyMediaProfile::PC_PORT, variants);
    expect_eq(original.size(), 2);
    expect(original[0].data == low_texture && original[1].data == low_model);
    expect(pc[0].data == high_texture && pc[1].data == high_model);
    expect(!original[0].fallback_data && !pc[0].fallback_data);
    variants[1].fallback_data.reset();
    expect(Episode3::select_lobby_media(Episode3::LobbyMediaProfile::ORIGINAL, variants).empty());
    expect_eq(Episode3::select_lobby_media(Episode3::LobbyMediaProfile::PC_PORT, variants).size(), 2);
    variants[1].fallback_data = std::make_shared<const std::string>(0x3801, 'x');
    expect(Episode3::select_lobby_media(Episode3::LobbyMediaProfile::ORIGINAL, variants).empty());
  }

  phosg::log_info_f("-- Episode 3 lobby media wire limits");
  using Episode3::lobby_media_compatibility;
  expect(lobby_media_compatibility(0x3800, 0x36FFF).gc_ok);
  expect(!lobby_media_compatibility(0x3801, 0x36FFF).gc_ok);
  expect(!lobby_media_compatibility(1, 0x37000).gc_ok);
  expect(lobby_media_compatibility(0x7BF0, 16 * 1024 * 1024).pc_ok);
  expect(!lobby_media_compatibility(0x7BF1, 1).pc_ok);
  expect(!lobby_media_compatibility(1, 16 * 1024 * 1024 + 1).pc_ok);
  for (size_t size : {0x3800, 0x3801, 0x7BED, 0x7BEE, 0x7BEF, 0x7BF0}) {
    std::string data(size, 'x');
    auto body = Episode3::encode_lobby_media_body(1, 0x78563412, data);
    expect_eq(body.size() + 4, Episode3::lobby_media_packet_size(size));
    expect_le(body.size() + 4, 0x7C00);
    expect_eq(static_cast<size_t>(static_cast<uint8_t>(body[8]) | (static_cast<uint8_t>(body[9]) << 8)), size);
    expect_eq(body.substr(12, size), data);
    expect_eq(body.substr(0, 8), std::string("\x01\0\0\0\x12\x34\x56\x78", 8));
    expect_eq(body[10], 0);
    expect_eq(body[11], 0);
    for (size_t z = 12 + size; z < body.size(); ++z) {
      expect_eq(body[z], 0);
    }
  }
  expect_eq(Episode3::encode_lobby_media_body(4, 0, "").size(), 12);
  expect_raises(std::runtime_error, [] { Episode3::encode_lobby_media_body(1, 1, std::string(0x7BF1, 'x')); });
  expect_raises(std::runtime_error, [] { Episode3::encode_lobby_media_body(4, 0, "x"); });
  phosg::log_info_f("-- All static tests passed");
}
