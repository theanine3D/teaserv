#include <phosg/UnitTest.hh>

#include "QuestScript.hh"
#include "RareItemSet.hh"
#include "Episode3/LobbyMedia.hh"
#include "Episode3/LobbyMediaCapabilities.hh"
#include "Episode3/Modpacks.hh"
#include "Episode3/CaelumLobbies.hh"
#include "Episode3/CaelumCardMods.hh"
#include "CaelumVoice.hh"
#include "ConnectionLimiter.hh"
#include "TeaservModpackHttp.hh"
#include "Episode3/Server.hh"
#include "Episode3/RulerServer.hh"
#include "Episode3/DataIndexes.hh"
#include <phosg/Filesystem.hh>
#include <phosg/Hash.hh>
#include <phosg/Time.hh>
#include "Client.hh"
#include "GameServer.hh"
#include "ServerState.hh"
#include "SendCommands.hh"
#include "Compression.hh"
#include "CommandFormats.hh"
#include "PSOProtocol.hh"
#include "Text.hh"

namespace {
// No sockets, accounts on disk, login secrets or running server are used by this protocol fixture.
struct CardCatalogTestState : ServerState { CardCatalogTestState() : ServerState() {} };
class CardCatalogTestChannel : public Channel {
public:
  std::vector<std::string> packets;
  explicit CardCatalogTestChannel(Version v) : Channel(v, Language::ENGLISH, "card-catalog-test",
      phosg::TerminalFormat::NORMAL, phosg::TerminalFormat::NORMAL, true, true) {}
  std::string default_name() const override { return "card-catalog-test"; }
  bool connected() const override { return true; }
  void disconnect() override {}
  void send_raw(std::string&& data) override { this->packets.emplace_back(std::move(data)); }
  asio::awaitable<void> recv_raw(void*, size_t) override { throw std::logic_error("fixture does not receive"); co_return; }
};
}

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

  phosg::log_info_f("-- Filename components from clients (path traversal)");
  for (const char* good : {"gba_demo.gba", "bob", "Player 1", "a.b.c", "x-y_z", "\xC3\xA9t\xC3\xA9", "CAELUM~1.ZIP"}) {
    expect(is_safe_filename_component(good));
  }
  for (const char* bad : {"", ".", "..", ".hidden", "a.", "a ", "..\\licenses\\1.json", "x\\..\\player_bob",
           "a/b", "a\\b", "C:x", "file:stream", "a..b", "a*", "a?", "a\"b", "a<b", "a>b", "a|b", "a\tb", "a\x7F"}) {
    expect(!is_safe_filename_component(bad));
  }
  expect(is_unsafe_filename_char('\\') && is_unsafe_filename_char(':') && is_unsafe_filename_char('/'));
  expect(!is_unsafe_filename_char('.') && !is_unsafe_filename_char(' ') && !is_unsafe_filename_char('\xC3'));

  phosg::log_info_f("-- Per-address connection limits");
  {
    using D = ConnectionLimiter::Decision;
    constexpr uint64_t SEC = 1000000;
    auto defaults = ConnectionLimitConfig::from_json(nullptr);
    expect(defaults.enabled);
    expect_eq(30, defaults.max_attempts_per_window);
    expect_eq(600 * SEC, defaults.cooldown_usecs);
    expect_eq(10, defaults.max_concurrent);
    expect(defaults.exempt.check(0x7F000001) && !defaults.exempt.check(0xC0A80102));

    auto j = phosg::JSON::parse(
        R"({"MaxConnectionsPerMinute": 3, "CooldownMinutes": 10, "MaxConnectionsPerAddress": 2, "ExemptAddresses": ["10.0.0.0/8"]})");
    auto config = ConnectionLimitConfig::from_json(&j);
    expect(config.exempt.check(0x0A000005) && !config.exempt.check(0x7F000001));
    for (const char* bad : {R"({"MaxConnectionsPerMinute": 0})", R"({"CooldownMinutes": 0})",
             R"({"MaxConnectionsPerAddress": -1})", R"([])", R"({"ExemptAddresses": ["1.2.3"]})"}) {
      auto bad_json = phosg::JSON::parse(bad);
      expect_raises(std::runtime_error, [&] { ConnectionLimitConfig::from_json(&bad_json); });
    }

    const uint32_t a = 0xC0A80102, b = 0xC0A80103;
    auto limiter = ConnectionLimiter::create();
    uint64_t t = 1000 * SEC;
    {
      // Concurrent cap: two slots held, the third is refused, releasing one frees a place
      auto r1 = limiter->check(a, config, t);
      auto r2 = limiter->check(a, config, t + 1);
      expect(r1.decision == D::ALLOW && r1.slot && r2.decision == D::ALLOW);
      expect_eq(2, limiter->open_connections(a));
      expect(limiter->check(a, config, t + 2).decision == D::TOO_MANY_OPEN); // 3rd attempt this minute
      r1.slot.reset();
      expect_eq(1, limiter->open_connections(a));
      // 4th attempt within the minute goes over 3/minute: cooldown, even though a place is free
      auto r4 = limiter->check(a, config, t + 3);
      expect(r4.decision == D::STARTED_COOLDOWN && !r4.slot);
      expect_eq(600 * SEC, r4.cooldown_remaining_usecs);
    }
    expect_eq(0, limiter->open_connections(a)); // slots released when the results went out of scope
    // Other addresses are unaffected; exempt addresses are never counted
    expect(limiter->check(b, config, t + 4).decision == D::ALLOW);
    for (size_t z = 0; z < 100; z++) {
      expect(limiter->check(0x0A000005, config, t + z).decision == D::ALLOW);
    }
    // Attempts during the cooldown are refused and do not extend it
    auto in_cooldown = limiter->check(a, config, t + 300 * SEC);
    expect(in_cooldown.decision == D::IN_COOLDOWN);
    expect_eq(300 * SEC + 3, in_cooldown.cooldown_remaining_usecs); // still ends 600 s after it started
    expect(limiter->check(a, config, t + 3 + 600 * SEC - 1).decision == D::IN_COOLDOWN);
    // When it ends, a fresh window starts
    expect(limiter->check(a, config, t + 3 + 600 * SEC).decision == D::ALLOW);
    // The window resets after a minute: 3 per minute, spread over minutes, never triggers a cooldown
    auto spread = ConnectionLimiter::create();
    for (size_t minute = 0; minute < 5; minute++) {
      for (size_t z = 0; z < 3; z++) {
        expect(spread->check(b, config, t + minute * 61 * SEC + z).decision == D::ALLOW);
      }
    }
    // Idle addresses are forgotten (sweep runs at most once per window)
    expect(spread->tracked_addresses() >= 1);
    spread->check(a, config, t + 3600 * SEC); // a: new entry; b: window long over, no open slots
    expect_eq(1, spread->tracked_addresses());
    // Disabled: everything allowed, nothing tracked
    ConnectionLimitConfig off;
    off.enabled = false;
    auto unused = ConnectionLimiter::create();
    for (size_t z = 0; z < 100; z++) {
      expect(unused->check(a, off, t).decision == D::ALLOW);
    }
    expect_eq(0, unused->tracked_addresses());
  }

  phosg::log_info_f("-- Caelum voice chat");
  {
    using namespace caelum::voice;
    auto defaults = parse_config(nullptr);
    expect(defaults.enabled && defaults.turn_enabled && defaults.quiet_lobbies.empty());
    expect_eq(3478, defaults.turn_port);
    auto json = phosg::JSON::parse(
        "{\"Enabled\": true, \"QuietLobbies\": [3, 7], \"StunServers\": [\"stun.example.net:3478\"], "
        "\"TurnPort\": 0, \"TurnCredentialHours\": 2}");
    auto config = parse_config(&json);
    expect(config.quiet_lobbies.count(3) && config.quiet_lobbies.count(7) && !config.quiet_lobbies.count(1));
    expect_eq(1, config.stun_servers.size());
    expect_eq(7200, config.credential_lifetime_seconds);
    for (const char* bad : {"[]", "{\"QuietLobbies\": [0]}", "{\"TurnPort\": 70000}", "{\"TurnCredentialHours\": 0}",
             "{\"StunServers\": [\"a b\"]}", "{\"TurnRelayPortBegin\": 5000}"}) {
      auto j = phosg::JSON::parse(bad);
      expect_raises(std::runtime_error, [&] { parse_config(&j); });
    }
    expect_eq("v=1\nself=42\nice=stun:h:1\nice=turn:h:1?transport=udp u p\n",
        encode_config(42, {{"stun:h:1", "", ""}, {"turn:h:1?transport=udp", "u", "p"}}));
    Room room{"G7", false, true, {{10, 'P', 0}, {20, 'S', 4}}};
    expect_eq("v=1\nroom=G7\nquiet=0\nbattle=1\npeer=10 P 0\npeer=20 S 4\n", encode_roster(room));
    Signal sig{0x01020304, 3, std::string("candidate:1 1 UDP 1 10.0.0.1 5000 typ host")};
    auto wire = encode_signal(sig);
    expect_eq(8 + sig.payload.size(), wire.size());
    expect_eq(0x04, static_cast<uint8_t>(wire[0])); // little-endian peer
    auto back = parse_signal(wire);
    expect_eq(sig.peer, back.peer);
    expect_eq(sig.kind, back.kind);
    expect_eq(sig.payload, back.payload);
    expect_raises(std::runtime_error, [&] { parse_signal("short"); });
    std::string header(8, '\0');
    header[0] = 1;
    header[4] = 9; // unknown kind
    expect_raises(std::runtime_error, [&] { parse_signal(header); });
    header[4] = 1;
    header[5] = 1; // reserved bytes must be zero
    expect_raises(std::runtime_error, [&] { parse_signal(header); });
    header[5] = 0;
    expect_eq(1, parse_signal(header).peer);
    expect_raises(std::runtime_error, [&] { parse_signal(header + std::string(MAX_SIGNAL_PAYLOAD + 1, 'x')); });
    expect_raises(std::runtime_error, [&] { encode_signal({1, 1, std::string(MAX_SIGNAL_PAYLOAD + 1, 'x')}); });
    // voice state: what a real Caelum client sends (padded to 4 bytes by the game's packet layer)
    expect_eq(1, parse_state(std::string("\x01\0\0\0", 4)));
    expect_eq(3, parse_state(std::string("\x03", 1)));
    expect_raises(std::runtime_error, [&] { parse_state(""); });
    expect_raises(std::runtime_error, [&] { parse_state(std::string("\x01\0\0\0\0", 5)); });
    expect_raises(std::runtime_error, [&] { parse_state(std::string("\x01\x01\0\0", 4)); });
    expect_raises(std::runtime_error, [&] { parse_state(std::string("\x04\0\0\0", 4)); });
    RateLimiter limiter(20, 50);
    size_t allowed = 0;
    for (size_t z = 0; z < 100; z++) {
      allowed += limiter.allow(1000000); // a burst within one instant
    }
    expect_eq(50, allowed);
    expect(!limiter.allow(1000000 + 10000)); // 0.01 s later: 0.2 tokens
    expect(limiter.allow(1000000 + 100000)); // 0.1 s later: 2 tokens
    auto creds = make_credentials(1234567, 1000, 3600);
    expect_eq("4600:1234567", creds.username);
    expect_eq(32, creds.password.size());
    expect(make_credentials(1, 0, 1).password != make_credentials(1, 0, 1).password);
    // The relay really starts (TurnPort 0: any free port) and takes credentials.
    TurnServer relay(config);
    expect(relay.port() != 0);
    relay.add_credentials(creds, 60);
  }

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
  phosg::log_info_f("-- Teaserv modpack download server: request targets");
  {
    using teaserv_modpack_http::filename_for_target;
    expect_eq("Beach Lobby.zip", filename_for_target("/modpacks/Beach%20Lobby.zip").value());
    expect_eq("pack.ZIP", filename_for_target("/modpacks/pack.ZIP?x=1").value());
    expect_eq("a+b.zip", filename_for_target("/modpacks/a+b.zip").value());
    expect_eq("\xC3\xA9t\xC3\xA9.zip", filename_for_target("/modpacks/%C3%A9t%C3%A9.zip").value());
    for (const char* bad : {"/pack.zip", "/modpacks/", "/modpacks/pack.txt", "/modpacks/.zip", "/modpacks/.hidden.zip",
             "/modpacks/../config.zip", "/modpacks/%2e%2e%2fconfig.zip", "/modpacks/sub/pack.zip",
             "/modpacks/sub%2Fpack.zip", "/modpacks/sub%5Cpack.zip", "/modpacks/C:pack.zip", "/modpacks/a%00.zip",
             "/modpacks/a%0A.zip", "/modpacks/a%zz.zip", "/modpacks/a%2.zip", "/modpacks/pack.zip%",
             "/modpacks/x.zip.exe", "/Modpacks/pack.zip"}) {
      expect(!filename_for_target(bad).has_value());
    }
    expect(filename_for_target("/modpacks/" + std::string(251, 'n') + ".zip").has_value());
    expect(!filename_for_target("/modpacks/" + std::string(252, 'n') + ".zip").has_value());
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

    phosg::log_info_f("-- Caelum effective card catalog and battle rules");
    // The actual server definitions can contain every custom ID without altering the stock baseline.
    auto table_with_types = [&](bool extra) {
      std::vector<uint32_t> ids{1,2,3,4,5,6,7,8,9,10,11};
      if (extra) ids.emplace_back(740);
      auto rel = make_table(ids);
      for (size_t z = 1; z < ids.size(); z++) rel[z * 0x128 + 0x44] = 4; // action cards
      return rel;
    };
    auto stock_rel = table_with_types(false), custom_rel = table_with_types(true);
    auto stock_index = Episode3::CardIndex::from_rel_data(stock_rel);
    auto custom_zip = make_zip({{"new/PsoCardDataTbl_Base.prs", prs_compress(custom_rel)}});
    auto custom_index = catalog_for_pack_bytes(custom_zip, stock_index);
    expect_eq(custom_index->all_ids().size(), 12);
    expect_eq(stock_index->all_ids().size(), 11);
    expect_raises(std::out_of_range, [&] { stock_index->definition_for_id(740); });
    expect_eq(custom_index->definition_for_id(740)->def.card_id, 740);
    expect(incompatible(stock_index, custom_index));
    expect(!incompatible(custom_index, custom_index));
    expect(!incompatible(stock_index, catalog_for_pack_bytes(make_zip({{"art.png", "png"}}), stock_index)));
    // A later matching entry replaces the whole file; it does not merge the earlier table.
    auto winner = catalog_for_pack_bytes(make_zip({
        {"a/PsoCardDataTbl_Base.prs", prs_compress(custom_rel)},
        {"b/PSOCARDDATATBL_BASE.PRS", prs_compress(stock_rel)}}), stock_index);
    expect(!incompatible(stock_index, winner));
    expect_eq(winner->all_ids().size(), 11);
    expect_eq(prs_decompress(custom_index->get_compressed_definitions()), custom_rel); // B8 has exactly these rules
    // Names do not change gameplay compatibility, but an existing card's cost/abilities do.
    auto renamed = stock_rel; renamed[0xA0] = 'X';
    expect(!incompatible(stock_index, Episode3::CardIndex::from_rel_data(renamed)));
    auto changed = stock_rel; changed[0x128 + 0x45] = 5;
    auto changed_index = Episode3::CardIndex::from_rel_data(changed);
    expect(incompatible(stock_index, changed_index));
    expect_eq(changed_index->definition_for_id(2)->def.self_cost, 5);
    auto malformed = custom_rel; malformed[0x128 + 3] = 1; // duplicate ID
    expect_raises(std::runtime_error, [&] { Episode3::CardIndex::from_rel_data(malformed); });
    expect_raises(std::runtime_error, [&] { Episode3::CardIndex::from_rel_data(make_table({752})); });
    expect_raises(std::runtime_error, [&] { Episode3::CardIndex::from_rel_data(std::string(0x36EC1, '\0')); });
    expect_raises(std::runtime_error, [&] { catalog_for_pack_bytes(make_zip({{"PsoCardDataTbl_Base.prs", "broken PRS"}}), stock_index); });
    // Pinning does not change old sessions when the latest catalog changes or reverts to stock.
    auto old_session = custom_index; auto latest = changed_index; latest = stock_index;
    expect_eq(old_session->all_ids().size(), 12);
    expect(incompatible(old_session, latest));
    // Exercise the actual native battle verifier, with and without the new ID in its index.
    parray<le_uint16_t, 0x1F> deck;
    deck[0] = 1;
    for (size_t z = 1; z < deck.size(); z++) deck[z] = 2 + (z - 1) / 3;
    deck[1] = 740;
    auto engine = [&](std::shared_ptr<const Episode3::CardIndex> index) {
      return std::make_shared<Episode3::Server>(nullptr,
          Episode3::Server::Options{.card_index = index, .map_index = nullptr, .behavior_flags = 0,
              .opt_rand_stream = nullptr, .rand_crypt = std::make_shared<PSOV2Encryption>(1),
              .tournament = nullptr, .trap_card_ids = {}, .output_queue = nullptr});
    };
    auto stock_engine = engine(stock_index), custom_engine = engine(custom_index);
    Episode3::RulerServer stock_ruler(stock_engine), custom_ruler(custom_engine);
    expect_eq(stock_ruler.verify_deck(deck), -0x7C);
    expect_eq(custom_ruler.verify_deck(deck), 0);
    expect_eq(custom_engine->definition_for_card_id(740)->def.type, Episode3::CardType::ACTION);
    // Exercise the actual B8 sender and lobby entry guard, rather than only testing a hash helper.
    auto state = std::make_shared<CardCatalogTestState>();
    state->io_context = std::make_shared<asio::io_context>(1);
    state->data = std::make_shared<DataIndex>();
    state->data->ep3_card_index = stock_index;
    state->data->ep3_card_index_trial = stock_index;
    auto game_server = std::make_shared<GameServer>(state);
    auto make_client = [&](Version version) {
      auto channel = std::make_shared<CardCatalogTestChannel>(version);
      auto client = std::make_shared<Client>(game_server, channel, static_cast<ServerBehavior>(0));
      client->login = std::make_shared<Login>();
      client->login->account = std::make_shared<Account>();
      client->login->account->set_flag(Account::Flag::FREE_JOIN_GAMES);
      return std::make_pair(client, channel);
    };
    auto [custom_client, custom_channel] = make_client(Version::GC_EP3);
    custom_client->caelum_mod_ready = true;
    custom_client->ep3_cards = custom_index;
    send_ep3_card_list_update(custom_client);
    expect_eq(custom_channel->packets.size(), 1);
    phosg::StringReader b8(custom_channel->packets.back());
    expect_eq(b8.pget_u8(0), 0xB8);
    expect_eq(prs_decompress(b8.preadx(8, b8.pget_u32l(4))), custom_rel);
    custom_client->set_flag(Client::Flag::HAS_EP3_CARD_DEFS);
    send_ep3_card_list_update(custom_client);
    expect_eq(custom_channel->packets.size(), 1);
    auto [original_client, original_channel] = make_client(Version::GC_EP3);
    send_ep3_card_list_update(original_client);
    phosg::StringReader stock_b8(original_channel->packets.back());
    expect_eq(prs_decompress(stock_b8.preadx(8, stock_b8.pget_u32l(4))), stock_rel);
    state->caelum_card_index = changed_index;
    expect(custom_client->ep3_card_catalog() == custom_index);
    expect(original_client->ep3_card_catalog() == stock_index);
    auto game = std::make_shared<Lobby>(state, 1, true);
    game->episode = Episode::EP3; game->max_clients = 4; game->allow_version(Version::GC_EP3);
    game->ep3_cards = custom_index;
    expect_eq(game->join_error_for_client(original_client, nullptr), Lobby::JoinError::CAELUM_CARD_MODS);
    expect_eq(game->join_error_for_client(custom_client, nullptr), Lobby::JoinError::ALLOWED);
    custom_client->ep3_cards = changed_index;
    expect_eq(game->join_error_for_client(custom_client, nullptr), Lobby::JoinError::CAELUM_CARD_MODS);
    auto [ordinary_client, ordinary_channel] = make_client(Version::GC_V3);
    game->episode = Episode::EP1; game->allow_version(Version::GC_V3);
    expect_eq(game->join_error_for_client(ordinary_client, nullptr), Lobby::JoinError::ALLOWED);

    // Manifest loading verifies bytes by SHA-256, uses manifest order, and fails closed on missing packs.
    auto dir = std::filesystem::temp_directory_path() / std::format("teaserv-card-catalog-{}", phosg::now());
    expect(std::filesystem::create_directories(dir));
    expect(std::filesystem::canonical(dir).parent_path() == std::filesystem::canonical(std::filesystem::temp_directory_path()));
    try {
      phosg::save_file((dir / "custom.zip").string(), custom_zip);
      auto stock_zip = make_zip({{"PsoCardDataTbl_Base.prs", prs_compress(stock_rel)}});
      phosg::save_file((dir / "stock.zip").string(), stock_zip);
      caelum::mods::Requirement a{phosg::tolower(phosg::SHA256(custom_zip).hex()), "custom", "https://example.invalid/a.zip"};
      caelum::mods::Requirement b{phosg::tolower(phosg::SHA256(stock_zip).hex()), "stock", "https://example.invalid/b.zip"};
      expect(!incompatible(stock_index, load_catalog({a, b}, dir, stock_index)));
      expect(!incompatible(custom_index, load_catalog({b, a}, dir, stock_index)));
      auto missing = a; missing.sha256 = std::string(64, '0');
      expect_raises(std::runtime_error, [&] { load_catalog({missing}, dir, stock_index); });
      expect(!incompatible(stock_index, load_catalog({}, dir, stock_index)));
    } catch (...) {
      std::filesystem::remove_all(dir);
      throw;
    }
    std::filesystem::remove_all(dir);

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

    // A sealed pack whose card table adds card 740 (stored ZIP; the table sealed with method 0)
    std::string rel;
    for (uint32_t id : {1u, 740u}) {
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
    expect_eq(sealed.new_card_ids, (std::vector<uint32_t>{740}));
    auto sealed_index = catalog_for_pack_bytes(zip, nullptr, {{header.id, key}});
    expect_eq(sealed_index->definition_for_id(740)->def.card_id, 740);
    expect_eq(prs_decompress(sealed_index->get_compressed_definitions()), rel);
    expect_raises(std::runtime_error, [&] { catalog_for_pack_bytes(zip, nullptr); });
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
