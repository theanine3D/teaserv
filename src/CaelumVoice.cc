#include "CaelumVoice.hh"

#include <string.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <mutex>
#include <phosg/Encoding.hh>
#include <phosg/JSON.hh>
#include <phosg/Random.hh>
#include <stdexcept>

#include <juice/juice.h>

namespace caelum::voice {

Config parse_config(const phosg::JSON* json) {
  Config ret;
  if (!json) {
    return ret;
  }
  if (!json->is_dict()) {
    throw std::runtime_error("CaelumVoice must be an object");
  }
  ret.enabled = json->get_bool("Enabled", true);
  try {
    for (const auto& it : json->get_list("QuietLobbies")) {
      int64_t n = it->as_int();
      if (n < 1 || n > 0xFFFF) {
        throw std::runtime_error("CaelumVoice.QuietLobbies entries are lobby numbers (1 and up)");
      }
      ret.quiet_lobbies.emplace(n);
    }
  } catch (const std::out_of_range&) {
  }
  try {
    for (const auto& it : json->get_list("StunServers")) {
      const auto& s = it->as_string();
      if (s.empty() || s.size() > 200 || s.find_first_of(" \r\n\t") != std::string::npos) {
        throw std::runtime_error("CaelumVoice.StunServers entries must be \"host:port\"");
      }
      ret.stun_servers.emplace_back(s);
    }
  } catch (const std::out_of_range&) {
  }
  ret.turn_enabled = json->get_bool("TurnEnabled", true);
  auto port = [&](const char* key, int64_t def) -> uint16_t {
    int64_t v = json->get_int(key, def);
    if (v < 0 || v > 0xFFFF) {
      throw std::runtime_error(std::format("CaelumVoice.{} must be a port number", key));
    }
    return v;
  };
  ret.turn_port = port("TurnPort", 3478);
  ret.relay_port_begin = port("TurnRelayPortBegin", 0);
  ret.relay_port_end = port("TurnRelayPortEnd", 0);
  if ((ret.relay_port_begin == 0) != (ret.relay_port_end == 0) || ret.relay_port_begin > ret.relay_port_end) {
    throw std::runtime_error("CaelumVoice.TurnRelayPortBegin/End must both be set (begin <= end) or both be 0");
  }
  int64_t lifetime = json->get_int("TurnCredentialHours", 12);
  if (lifetime < 1 || lifetime > 24 * 7) {
    throw std::runtime_error("CaelumVoice.TurnCredentialHours must be between 1 and 168");
  }
  ret.credential_lifetime_seconds = lifetime * 3600;
  int64_t allocations = json->get_int("TurnMaxAllocations", 1000);
  if (allocations < 1 || allocations > 100000) {
    throw std::runtime_error("CaelumVoice.TurnMaxAllocations must be between 1 and 100000");
  }
  ret.max_allocations = allocations;
  return ret;
}

std::string encode_config(uint32_t self_guild_card, const std::vector<IceServer>& servers) {
  std::string ret = std::format("v=1\nself={}\n", self_guild_card);
  for (const auto& s : servers) {
    if (s.username.empty()) {
      ret += std::format("ice={}\n", s.url);
    } else {
      ret += std::format("ice={} {} {}\n", s.url, s.username, s.password);
    }
  }
  return ret;
}

std::string encode_roster(const Room& room) {
  std::string ret = std::format("v=1\nroom={}\nquiet={}\nbattle={}\n", room.id, room.quiet ? 1 : 0, room.battle ? 1 : 0);
  for (const auto& m : room.members) {
    ret += std::format("peer={} {} {}\n", m.guild_card, m.role, m.lobby_client_id);
  }
  return ret;
}

uint8_t parse_state(const std::string& data) {
  if (data.empty() || data.size() > 4) {
    throw std::runtime_error("invalid voice state size");
  }
  for (size_t z = 1; z < data.size(); z++) {
    if (data[z]) {
      throw std::runtime_error("invalid voice state padding");
    }
  }
  uint8_t state = static_cast<uint8_t>(data[0]);
  if (state & ~3) {
    throw std::runtime_error("invalid voice state bits");
  }
  return state;
}

Signal parse_signal(const std::string& data) {
  if (data.size() < 8) {
    throw std::runtime_error("voice signal is too short");
  }
  if (data.size() > 8 + MAX_SIGNAL_PAYLOAD) {
    throw std::runtime_error("voice signal is too long");
  }
  Signal ret;
  ret.peer = phosg::le_uint32_t(*reinterpret_cast<const phosg::le_uint32_t*>(data.data()));
  ret.kind = static_cast<uint8_t>(data[4]);
  if (ret.kind < 1 || ret.kind > 4 || data[5] || data[6] || data[7]) {
    throw std::runtime_error("invalid voice signal header");
  }
  ret.payload = data.substr(8);
  return ret;
}

std::string encode_signal(const Signal& signal) {
  if (signal.payload.size() > MAX_SIGNAL_PAYLOAD) {
    throw std::runtime_error("voice signal is too long");
  }
  std::string ret(8, '\0');
  *reinterpret_cast<phosg::le_uint32_t*>(ret.data()) = signal.peer;
  ret[4] = static_cast<char>(signal.kind);
  return ret + signal.payload;
}

RateLimiter::RateLimiter(uint32_t rate, uint32_t burst) : tokens(burst), rate(rate), burst(burst) {}

bool RateLimiter::allow(uint64_t now_usecs) {
  if (this->last_usecs && now_usecs > this->last_usecs) {
    this->tokens = std::min<double>(this->burst, this->tokens + (now_usecs - this->last_usecs) * 1e-6 * this->rate);
  }
  this->last_usecs = now_usecs;
  if (this->tokens < 1.0) {
    return false;
  }
  this->tokens -= 1.0;
  return true;
}

Credentials make_credentials(uint32_t guild_card, uint64_t now_seconds, uint32_t lifetime_seconds) {
  Credentials ret;
  ret.username = std::format("{}:{}", now_seconds + lifetime_seconds, guild_card);
  std::string raw = phosg::random_data(16);
  for (uint8_t ch : raw) {
    ret.password += std::format("{:02x}", ch);
  }
  return ret;
}

struct TurnServer::Impl {
  juice_server_t* server = nullptr;
  std::mutex lock;
};

TurnServer::TurnServer(const Config& config) : impl(std::make_unique<Impl>()) {
  juice_server_config_t c;
  memset(&c, 0, sizeof(c));
  c.credentials = nullptr; // per-session credentials are added with add_credentials
  c.credentials_count = 0;
  c.max_allocations = config.max_allocations;
  c.max_peers = 16;
  c.bind_address = nullptr;
  c.external_address = nullptr;
  c.port = config.turn_port;
  c.relay_port_range_begin = config.relay_port_begin;
  c.relay_port_range_end = config.relay_port_end;
  c.realm = config.realm.c_str();
  this->impl->server = juice_server_create(&c);
  if (!this->impl->server) {
    throw std::runtime_error(std::format("cannot start the voice TURN relay on UDP port {}", config.turn_port));
  }
}

TurnServer::~TurnServer() {
  if (this->impl && this->impl->server) {
    juice_server_destroy(this->impl->server);
  }
}

uint16_t TurnServer::port() const {
  return juice_server_get_port(this->impl->server);
}

void TurnServer::add_credentials(const Credentials& credentials, uint32_t lifetime_seconds) {
  juice_server_credentials_t c;
  c.username = credentials.username.c_str();
  c.password = credentials.password.c_str();
  c.allocations_quota = 16; // one client: one relayed connection per other room member (rooms hold up to 12)
  std::lock_guard g(this->impl->lock);
  if (juice_server_add_credentials(this->impl->server, &c, static_cast<unsigned long>(lifetime_seconds) * 1000) < 0) {
    throw std::runtime_error("cannot add voice TURN credentials");
  }
}

} // namespace caelum::voice
