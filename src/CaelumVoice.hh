#pragma once

// Caelum peer-to-peer voice chat: the server's part (docs/CAELUM_VOICE.md).
//
// The server never carries voice. It only:
//   - tells Caelum clients that voice exists here and how to reach each other (STUN / TURN addresses, F2 flag 0x10),
//   - keeps each voice room's roster (who is in the room, player or spectator, F2 flag 0x12),
//   - relays small connection messages ("signals", F2 flag 0x13) between members of the same room, rate-limited,
//   - optionally runs a TURN relay (libjuice's server) for players whose routers block direct connections. TURN only
//     forwards encrypted packets it cannot read.
// Rooms: a public lobby is one room. A battle room is the primary game plus its spectator (watcher) games; spectators
// are listed with role 'S' and every client refuses voice from them (listen only). Quiet lobbies have no voice.

#include <stdint.h>

#include <deque>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace phosg {
class JSON;
}

namespace caelum::voice {

// F2 flags used by voice (F2 flags 1-5 belong to Caelum modpacks)
constexpr uint32_t FLAG_CONFIG = 0x10; // server -> client: voice is available here; ICE servers (text, see encode_config)
constexpr uint32_t FLAG_STATE = 0x11;  // client -> server: u8 bit 0 = voice on (in the room), bit 1 = may send
constexpr uint32_t FLAG_ROSTER = 0x12; // server -> client: the client's current voice room (text, see encode_roster)
constexpr uint32_t FLAG_SIGNAL = 0x13; // both ways: Signal (binary); the server rewrites the peer to the sender

constexpr size_t MAX_SIGNAL_PAYLOAD = 2048;

struct Config {
  bool enabled = true;
  std::set<uint32_t> quiet_lobbies; // lobby numbers (1-based, as in LobbyEvents) where voice is off
  std::vector<std::string> stun_servers; // "host:port"; empty = only the built-in TURN server's STUN
  bool turn_enabled = true;
  uint16_t turn_port = 3478;
  uint16_t relay_port_begin = 0; // 0 = any port
  uint16_t relay_port_end = 0;
  uint32_t credential_lifetime_seconds = 12 * 60 * 60;
  int max_allocations = 1000;
  std::string realm = "teaserv";
  uint32_t signals_per_second = 20;
  uint32_t signal_burst = 50;
};

// The "CaelumVoice" object of config.json (missing = defaults). Throws std::runtime_error on invalid values.
Config parse_config(const phosg::JSON* json);

struct IceServer {
  std::string url; // "stun:host:port" or "turn:host:port?transport=udp"
  std::string username;
  std::string password;
};

// Lines "key=value": v=1, self=<guild card>, then one "ice=<url>[ <username> <password>]" per server.
std::string encode_config(uint32_t self_guild_card, const std::vector<IceServer>& servers);

struct RosterEntry {
  uint32_t guild_card;
  char role; // 'P' player (may speak), 'S' spectator (listen only)
  uint8_t lobby_client_id;
  bool operator==(const RosterEntry&) const = default;
};

struct Room {
  std::string id;      // "L<lobby id>" or "G<primary game id>"
  bool quiet = false;  // no voice in this room
  bool battle = false; // a game room (no distance fading)
  std::vector<RosterEntry> members; // voice-enabled members only, sorted by guild card
};

// Lines: v=1, room=<id>, quiet=0|1, battle=0|1, then "peer=<guild card> <role> <lobby client id>" per member.
std::string encode_roster(const Room& room);

struct Signal {
  uint32_t peer = 0; // client -> server: the target; server -> client: the sender
  uint8_t kind = 0;  // 1 offer, 2 answer, 3 candidate, 4 bye (opaque to the server)
  std::string payload;
};
// FLAG_STATE body: the state byte, then up to 3 zero bytes of game-packet padding. Throws std::runtime_error.
uint8_t parse_state(const std::string& data);

// u32 little-endian peer, u8 kind, 3 zero bytes, payload (at most MAX_SIGNAL_PAYLOAD). Throws std::runtime_error.
Signal parse_signal(const std::string& data);
std::string encode_signal(const Signal& signal);

// Token bucket: `rate` per second, at most `burst` at once.
class RateLimiter {
public:
  RateLimiter(uint32_t rate = 20, uint32_t burst = 50);
  bool allow(uint64_t now_usecs);

private:
  double tokens;
  uint32_t rate;
  uint32_t burst;
  uint64_t last_usecs = 0;
};

// Time-limited TURN credentials ("<expiry unix seconds>:<guild card>", random password).
struct Credentials {
  std::string username;
  std::string password;
};
Credentials make_credentials(uint32_t guild_card, uint64_t now_seconds, uint32_t lifetime_seconds);

// The built-in TURN relay (libjuice). Constructing it binds the UDP port; throws std::runtime_error on failure.
class TurnServer {
public:
  explicit TurnServer(const Config& config);
  ~TurnServer();
  TurnServer(const TurnServer&) = delete;
  TurnServer& operator=(const TurnServer&) = delete;
  uint16_t port() const;
  void add_credentials(const Credentials& credentials, uint32_t lifetime_seconds);

private:
  struct Impl;
  std::unique_ptr<Impl> impl;
};

} // namespace caelum::voice
