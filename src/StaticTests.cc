#include <phosg/UnitTest.hh>

#include "QuestScript.hh"
#include "RareItemSet.hh"
#include "Episode3/LobbyMedia.hh"
#include "Episode3/LobbyMediaCapabilities.hh"
#include "Episode3/Modpacks.hh"
#include "Episode3/CaelumLobbies.hh"
#include "Episode3/CaelumCardMods.hh"
#include "Compression.hh"
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
  { // Google Drive share links map to the direct download endpoint (Modpacks.hh direct_download_url)
    using caelum::mods::direct_download_url;
    const std::string direct="https://drive.usercontent.google.com/download?id=1AbC-d_9&export=download&confirm=t";
    const std::string plain="https://example.test/mod.zip";
    expect_eq(direct_download_url(plain),plain);
    expect_eq(direct_download_url("https://drive.google.com/file/d/1AbC-d_9/view?usp=sharing"),direct);
    expect_eq(direct_download_url("https://drive.google.com/file/u/0/d/1AbC-d_9/view"),direct);
    expect_eq(direct_download_url("https://DRIVE.google.com/open?id=1AbC-d_9"),direct);
    expect_eq(direct_download_url("https://drive.google.com/uc?export=download&id=1AbC-d_9"),direct);
    expect_eq(direct_download_url("https://docs.google.com/uc?id=1AbC-d_9#x"),direct);
    expect_eq(direct_download_url("https://drive.usercontent.google.com/download?id=1AbC-d_9&export=download"),direct);
    expect_eq(direct_download_url("https://drive.google.com/file/d/1AbC-d_9/view?resourcekey=0-Xy_z"),direct+"&resourcekey=0-Xy_z");
    expect_raises(std::runtime_error,[&]{direct_download_url("https://drive.google.com/drive/folders/1AbC");});
    expect_raises(std::runtime_error,[&]{direct_download_url("https://docs.google.com/document/d/1AbC/edit");});
    expect_raises(std::runtime_error,[&]{direct_download_url("https://drive.google.com/open?id=a%2Fb");});
    expect_raises(std::runtime_error,[&]{caelum::mods::parse_manifest("CMOD1\n"+mod_hash+"\tx\thttps://drive.google.com/drive/folders/1AbC\n");});
    // Dropbox dl=1, OneDrive shares API (Microsoft's documented share id), SharePoint download=1.
    expect_eq(direct_download_url("https://www.dropbox.com/scl/fi/abc123/pack.zip?rlkey=k9z&st=s1&dl=0"),
              std::string("https://www.dropbox.com/scl/fi/abc123/pack.zip?rlkey=k9z&st=s1&dl=1"));
    expect_eq(direct_download_url("https://dropbox.com/s/abc123/pack.zip"),std::string("https://www.dropbox.com/s/abc123/pack.zip?dl=1"));
    expect_eq(direct_download_url("https://onedrive.live.com/redir?resid=1231244193912!12&authKey=1201919!12921!1"),
              std::string("https://api.onedrive.com/v1.0/shares/u!aHR0cHM6Ly9vbmVkcml2ZS5saXZlLmNvbS9yZWRpcj9yZXNpZD0xMjMxMjQ0MTkzOTEyITEyJmF1dGhLZXk9MTIwMTkxOSExMjkyMSEx/root/content"));
    expect_eq(direct_download_url("https://1drv.ms/u/s!AbCdEf-12345?e=xyz"),
              std::string("https://api.onedrive.com/v1.0/shares/u!aHR0cHM6Ly8xZHJ2Lm1zL3UvcyFBYkNkRWYtMTIzNDU_ZT14eXo/root/content"));
    expect_eq(direct_download_url("https://contoso-my.sharepoint.com/:u:/g/personal/a_contoso_com/EaBc?e=Xy1"),
              std::string("https://contoso-my.sharepoint.com/:u:/g/personal/a_contoso_com/EaBc?download=1"));
    expect_raises(std::runtime_error,[&]{direct_download_url("https://www.dropbox.com/scl/fo/abc/xyz?rlkey=1");});
    expect_raises(std::runtime_error,[&]{direct_download_url("https://1drv.ms/f/s!AbC");});
    expect_raises(std::runtime_error,[&]{direct_download_url("https://contoso.sharepoint.com/:f:/s/site/EaBc");});
    const std::string share="https://drive.google.com/file/d/1AbC/view";
    expect_eq(caelum::mods::parse_manifest("CMOD1\n"+mod_hash+"\tx\t"+share+"\n")[0].url,share); // wire keeps the link
  }

  phosg::log_info_f("-- Caelum card mods (cross-play)");
  {
    using namespace caelum::cardmods;
    expect(!separated(false, true, false));
    expect(!separated(true, true, true));
    expect(!separated(true, false, false));
    expect(separated(true, true, false));
    expect(separated(true, false, true));

    // Raw deflate: stored, fixed and dynamic blocks (zlib output, temp/card_mod_xplay_1112/deflate_vectors.py)
    std::string content;
    for (size_t z = 0; z < 60; z++) {
      content += std::format("card {} ", z);
    }
    expect_eq(inflate_raw(phosg::parse_data_string("01d60129fe6361726420302063617264203120636172642032206361726420332063617264203420636172642035206361726420362063617264203720636172642038206361726420392063617264203130206361726420313120636172642031322063617264203133206361726420313420636172642031352063617264203136206361726420313720636172642031382063617264203139206361726420323020636172642032312063617264203232206361726420323320636172642032342063617264203235206361726420323620636172642032372063617264203238206361726420323920636172642033302063617264203331206361726420333220636172642033332063617264203334206361726420333520636172642033362063617264203337206361726420333820636172642033392063617264203430206361726420343120636172642034322063617264203433206361726420343420636172642034352063617264203436206361726420343720636172642034382063617264203439206361726420353020636172642035312063617264203532206361726420353320636172642035342063617264203535206361726420353620636172642035372063617264203538206361726420353920"), 0x1000), content);
    expect_eq(inflate_raw(phosg::parse_data_string("4b4e2c4a5130504806518610ca08421943281308650aa1cc20943984b280509650ed3063a0e618420d32849a640835ca106a9621d43043a8698650e30ca1e61941cd3382b90b6a9e11d43c23a8794650f38ca0e61941cd33829a670435cf186a9e31d43c639847a1e61943cd33869a670c35cf186a9e31d43c63a8792650f34ca0e69940cd3381851cd43c13a8792650f34ca0e69940cd33819a670a35cf146a9e29d43c53a879a6b0a8809a670a35cf146a9e29d43c534b0500"), 0x1000), content);
    expect_eq(inflate_raw(phosg::parse_data_string("35d1cb6d03310c40c156b684f0177bcb09ec0adcffc108383abd1307a4f4fafbbcaf9febf59fd8e4a636bd99cdefe6b1796e6ee387e1042848810a56c08216b8e0252fcf5ebce4252f79c94b5ef29257bce2d5399457bce215af78c52b5ef39ad7bc3e2fc76b5ef39ad7bce60d6f78c31bde9cafe00d6f78c39bfbfa02"), 0x1000), content);
    expect_raises(std::runtime_error, [&] { inflate_raw(phosg::parse_data_string("35d1cb6d03310c40c156b684f0177bcb09ec0adcffc108383abd1307a4f4fafbbcaf9febf59fd8e4a636bd99cdefe6b1796e6ee387e1042848810a56c08216b8e0252fcf5ebce4252f79c94b5ef29257bce2d5399457bce215af78c52b5ef39ad7bc3e2fc76b5ef39ad7bce60d6f78c31bde9cafe00d6f78c39bfbfa02"), 100); });
    expect_raises(std::runtime_error, [&] { inflate_raw(phosg::parse_data_string("35d1cb6d03310c40c156b684f0177bcb09ec0adcffc108383abd1307a4f4fafbbcaf9febf59fd8e4a636bd99cdefe6b1796e6ee387e1042848810a56c08216b8e0252fcf5ebce4252f79c94b5ef29257bce2d5399457bce215af78c52b5ef39ad7bc3e2fc76b5ef39ad7bce60d6f78c31bde9cafe00d6f78c39bfbfa02").substr(0, 60), 0x1000); });

    // A card table REL: records {id at +0, type at +0x44}, the end record has a negative type
    auto make_table = [](const std::vector<uint32_t>& ids) {
      std::string rel;
      for (uint32_t id : ids) {
        std::string rec(0x128, '\0');
        rec[0] = id >> 24, rec[1] = id >> 16, rec[2] = id >> 8, rec[3] = id;
        rel += rec;
      }
      std::string end(0x128, '\0');
      end[0] = 0x04, end[0x44] = static_cast<char>(0xFF);
      rel += end;
      const uint32_t root = rel.size(), count = ids.size() + 1;
      for (uint32_t v : {0u, count}) {
        rel.push_back(v >> 24), rel.push_back(v >> 16), rel.push_back(v >> 8), rel.push_back(v);
      }
      RELFileFooterBE footer;
      footer.root_offset = root;
      rel.append(reinterpret_cast<const char*>(&footer), sizeof(footer));
      return rel;
    };
    expect_eq(card_table_ids(make_table({1, 2, 999})), (std::vector<uint32_t>{1, 2, 999}));
    // A stored ZIP holding name -> data
    auto make_zip = [](const std::vector<std::pair<std::string, std::string>>& files) {
      std::string zip, central;
      auto u16 = [](std::string& s, uint32_t v) { s.push_back(v), s.push_back(v >> 8); };
      auto u32 = [&](std::string& s, uint32_t v) { u16(s, v), u16(s, v >> 16); };
      for (const auto& [name, data] : files) {
        const uint32_t local = zip.size();
        u32(zip, 0x04034B50), u16(zip, 20), u16(zip, 0), u16(zip, 0), u32(zip, 0), u32(zip, 0);
        u32(zip, data.size()), u32(zip, data.size()), u16(zip, name.size()), u16(zip, 0);
        zip += name + data;
        u32(central, 0x02014B50), u16(central, 20), u16(central, 20), u16(central, 0), u16(central, 0);
        u32(central, 0), u32(central, 0), u32(central, data.size()), u32(central, data.size());
        u16(central, name.size()), u16(central, 0), u16(central, 0), u16(central, 0), u16(central, 0);
        u32(central, 0), u32(central, local);
        central += name;
      }
      const uint32_t central_offset = zip.size();
      zip += central;
      u32(zip, 0x06054B50), u16(zip, 0), u16(zip, 0), u16(zip, files.size()), u16(zip, files.size());
      u32(zip, central.size()), u32(zip, central_offset), u16(zip, 0);
      return zip;
    };
    const std::set<uint32_t> stock{1, 2};
    const auto table = prs_compress(make_table({1, 2, 999}));
    auto pack = check_pack_bytes(make_zip({{"mod.txt", "name=x"}, {"cards/PSOCardDataTbl_Base.prs", table}}), stock);
    expect(pack.result == PackCheck::Result::NEW_CARDS);
    expect_eq(pack.new_card_ids, (std::vector<uint32_t>{999}));
    expect(pack.blocks_cross_play());
    pack = check_pack_bytes(make_zip({{"PsoCardDataTbl_Base.prs", table}}), {1, 2, 999});
    expect(pack.result == PackCheck::Result::NO_NEW_CARDS); // a changed stock table (stats, text) is not new cards
    expect(!pack.blocks_cross_play());
    pack = check_pack_bytes(make_zip({{"mod.txt", "name=x"}, {"textures/card.png", "png"}}), stock);
    expect(pack.result == PackCheck::Result::NO_NEW_CARDS); // cosmetic pack
    pack = check_pack_bytes("not a zip", stock);
    expect(pack.result == PackCheck::Result::UNREADABLE);
    expect(pack.blocks_cross_play());
    // A listed pack without a local copy counts as adding cards
    const auto missing = check_manifest({{std::string(64, 'e'), "Missing", "https://example.test/m.zip"}},
        "system/caelum-modpacks-static-test-missing", stock);
    expect_eq(missing.size(), 1);
    expect(missing[0].result == PackCheck::Result::NOT_FOUND);
    expect(missing[0].blocks_cross_play());
  }

  phosg::log_info_f("-- Caelum server modpacks (key exchange, sealed card check)");
  {
    namespace sp = caelum::serverpack;
    using namespace caelum::cardmods;
    const auto server = sp::Identity::from_seed(sp::random_array<sp::Key>());
    const auto other = sp::Identity::from_seed(sp::random_array<sp::Key>());
    sp::Header header;
    header.id = sp::random_array<sp::PackId>();
    header.servers = {server.public_key};
    const sp::Key key = sp::random_array<sp::Key>();
    // F2 flag 4 -> 5
    const sp::Key client_secret = sp::random_array<sp::Key>();
    sp::KeyRequest request;
    crypto_x25519_public_key(request.client_public.data(), client_secret.data());
    request.packs = {header.id, sp::random_array<sp::PackId>()};
    const auto parsed = sp::parse_key_request(sp::write_key_request(request) + std::string(3, '\0'));
    const auto reply = sp::open_key_reply(sp::make_key_reply(server, parsed, {{header.id, key}}), client_secret, request.client_public);
    expect(reply.server_identity == server.public_key);
    expect_eq(reply.keys.size(), 1);
    expect(reply.keys.at(header.id) == key);
    expect(sp::parse_header(sp::write_header(header)).allows(server.public_key));
    expect(!sp::parse_header(sp::write_header(header)).allows(other.public_key));
    expect_raises(std::runtime_error, [&] {
      sp::open_key_reply(sp::make_key_reply(server, parsed, {{header.id, key}}), sp::random_array<sp::Key>(), request.client_public);
    });

    // A sealed pack whose card table adds card 999 (stored ZIP; the table sealed with method 0)
    std::string rel;
    for (uint32_t id : {1u, 999u}) {
      std::string rec(0x128, '\0');
      rec[0] = id >> 24, rec[1] = id >> 16, rec[2] = id >> 8, rec[3] = id;
      rel += rec;
    }
    std::string end_rec(0x128, '\0');
    end_rec[0x44] = static_cast<char>(0xFF);
    rel += end_rec;
    const uint32_t root = rel.size();
    for (uint32_t v : {0u, 3u}) {
      rel.push_back(v >> 24), rel.push_back(v >> 16), rel.push_back(v >> 8), rel.push_back(v);
    }
    RELFileFooterBE footer;
    footer.root_offset = root;
    rel.append(reinterpret_cast<const char*>(&footer), sizeof(footer));
    const std::string table = prs_compress(rel);
    std::vector<std::pair<std::string, std::string>> files{
        {sp::HEADER_FILE, sp::write_header(header)},
        {"mod.txt", "name=x\nauthor=y\nversion=1\n"},
        {sp::entry_name(0), sp::seal(key, header.id, "0", table)},
        {sp::INDEX_FILE, sp::seal(key, header.id, "index", sp::write_index({{0, 0, static_cast<uint32_t>(table.size()), "x/PsoCardDataTbl_Base.prs"}}))}};
    std::string zip, central;
    auto u16 = [](std::string& s, uint32_t v) { s.push_back(v), s.push_back(v >> 8); };
    auto u32 = [&](std::string& s, uint32_t v) { u16(s, v), u16(s, v >> 16); };
    for (const auto& [name, data] : files) {
      const uint32_t local = zip.size();
      u32(zip, 0x04034B50), u16(zip, 20), u16(zip, 0), u16(zip, 0), u32(zip, 0), u32(zip, 0);
      u32(zip, data.size()), u32(zip, data.size()), u16(zip, name.size()), u16(zip, 0);
      zip += name + data;
      u32(central, 0x02014B50), u16(central, 20), u16(central, 20), u16(central, 0), u16(central, 0);
      u32(central, 0), u32(central, 0), u32(central, data.size()), u32(central, data.size());
      u16(central, name.size()), u16(central, 0), u16(central, 0), u16(central, 0), u16(central, 0);
      u32(central, 0), u32(central, local);
      central += name;
    }
    const uint32_t central_offset = zip.size();
    zip += central;
    u32(zip, 0x06054B50), u16(zip, 0), u16(zip, 0), u16(zip, files.size()), u16(zip, files.size());
    u32(zip, central.size()), u32(zip, central_offset), u16(zip, 0);
    auto sealed = check_pack_bytes(zip, {1}, {{header.id, key}});
    expect(sealed.result == PackCheck::Result::NEW_CARDS);
    expect_eq(sealed.new_card_ids, (std::vector<uint32_t>{999}));
    sealed = check_pack_bytes(zip, {1}); // no key: counted as adding cards
    expect(sealed.result == PackCheck::Result::UNREADABLE);
    expect(sealed.blocks_cross_play());
    sealed = check_pack_bytes(zip, {1}, {{header.id, sp::random_array<sp::Key>()}}); // wrong key
    expect(sealed.result == PackCheck::Result::UNREADABLE);
  }

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
