// Caelum voice chat: rooms, rosters, signal relay and TURN credentials (see CaelumVoice.hh, docs/CAELUM_VOICE.md).

#include "CaelumVoice.hh"

#include <algorithm>
#include <chrono>
#include <phosg/Time.hh>

#include "Client.hh"
#include "Lobby.hh"
#include "Loggers.hh"
#include "NetworkAddresses.hh"
#include "SendCommands.hh"
#include "ServerState.hh"

using namespace caelum;

static std::shared_ptr<Lobby> voice_primary(std::shared_ptr<Lobby> l) {
  if (l && l->is_game() && l->check_flag(Lobby::Flag::IS_SPECTATOR_TEAM)) {
    auto watched = l->watched_lobby.lock();
    if (watched) {
      return watched;
    }
  }
  return l;
}

voice::Room ServerState::caelum_voice_room(std::shared_ptr<Lobby> l) const {
  voice::Room room;
  auto primary = voice_primary(l);
  if (!primary) {
    return room;
  }
  room.battle = primary->is_game();
  room.id = std::format("{}{}", room.battle ? 'G' : 'L', primary->lobby_id);
  room.quiet = !this->data->caelum_voice.enabled ||
      (!room.battle && this->data->caelum_voice.quiet_lobbies.count(primary->lobby_id));
  auto add = [&](std::shared_ptr<Lobby> from, char role) {
    for (const auto& c : from->clients) {
      if (c && c->login && c->caelum_voice_on) {
        room.members.emplace_back(voice::RosterEntry{c->login->account->account_id, role, c->lobby_client_id});
      }
    }
  };
  add(primary, 'P');
  if (room.battle) {
    // Spectators listen only (user decision 2026-11-16): every client refuses voice from role 'S'.
    for (const auto& watcher : primary->watcher_lobbies) {
      add(watcher, 'S');
    }
  }
  std::sort(room.members.begin(), room.members.end(),
      [](const auto& a, const auto& b) { return a.guild_card < b.guild_card; });
  return room;
}

static std::vector<std::shared_ptr<Client>> voice_room_clients(std::shared_ptr<Lobby> l) {
  std::vector<std::shared_ptr<Client>> ret;
  auto primary = voice_primary(l);
  if (!primary) {
    return ret;
  }
  auto add = [&](std::shared_ptr<Lobby> from) {
    for (const auto& c : from->clients) {
      if (c && c->login && c->caelum_mod_ready) {
        ret.emplace_back(c);
      }
    }
  };
  add(primary);
  if (primary->is_game()) {
    for (const auto& watcher : primary->watcher_lobbies) {
      add(watcher);
    }
  }
  return ret;
}

void ServerState::caelum_voice_room_changed(std::shared_ptr<Lobby> l) {
  if (!l || !this->data->caelum_voice.enabled) {
    return;
  }
  auto room = this->caelum_voice_room(l);
  std::string text = voice::encode_roster(room);
  // Everyone in the room who can do voice gets the roster, voice on or not (it also says whether the room is quiet).
  for (const auto& c : voice_room_clients(l)) {
    send_command(c, 0xF2, voice::FLAG_ROSTER, text);
  }
}

void ServerState::caelum_voice_send_config(std::shared_ptr<Client> c) {
  const auto& config = this->data->caelum_voice;
  if (!config.enabled || !c->caelum_mod_ready || !c->login) {
    return;
  }
  std::vector<voice::IceServer> servers;
  for (const auto& stun : config.stun_servers) {
    servers.emplace_back(voice::IceServer{"stun:" + stun, "", ""});
  }
  if (this->caelum_voice_turn) {
    try {
      uint32_t lifetime = config.credential_lifetime_seconds;
      auto creds = voice::make_credentials(c->login->account->account_id, phosg::now() / 1000000, lifetime);
      this->caelum_voice_turn->add_credentials(creds, lifetime);
      std::string host = string_for_address(this->data->connect_address_for_client(c));
      uint16_t port = this->caelum_voice_turn->port();
      // The relay also answers STUN, so it doubles as the address-discovery server.
      servers.emplace_back(voice::IceServer{std::format("stun:{}:{}", host, port), "", ""});
      servers.emplace_back(voice::IceServer{std::format("turn:{}:{}?transport=udp", host, port), creds.username, creds.password});
    } catch (const std::exception& e) {
      c->log.warning_f("Voice relay credentials unavailable: {}", e.what());
    }
  }
  send_command(c, 0xF2, voice::FLAG_CONFIG, voice::encode_config(c->login->account->account_id, servers));
}

void ServerState::caelum_voice_set_state(std::shared_ptr<Client> c, uint8_t state) {
  bool on = state & 1;
  if (c->caelum_voice_on == on) {
    return;
  }
  c->caelum_voice_on = on;
  this->caelum_voice_room_changed(c->lobby.lock());
}

void ServerState::caelum_voice_relay(std::shared_ptr<Client> from, const std::string& data) {
  if (!this->data->caelum_voice.enabled || !from->caelum_voice_on || !from->login) {
    return;
  }
  if (!from->caelum_voice_rate.allow(phosg::now())) {
    from->log.warning_f("Voice signal dropped (rate limit)");
    return;
  }
  auto signal = voice::parse_signal(data);
  auto lobby = from->lobby.lock();
  auto room = this->caelum_voice_room(lobby);
  if (room.quiet) {
    return;
  }
  uint32_t sender = from->login->account->account_id;
  for (const auto& c : voice_room_clients(lobby)) {
    if (c->caelum_voice_on && c != from && c->login->account->account_id == signal.peer) {
      signal.peer = sender; // receivers always learn the real sender
      send_command(c, 0xF2, voice::FLAG_SIGNAL, voice::encode_signal(signal));
      return;
    }
  }
}

void ServerState::caelum_voice_start_relay() {
  const auto& config = this->data->caelum_voice;
  if (this->is_replay || !config.enabled || !config.turn_enabled || this->caelum_voice_turn) {
    return;
  }
  try {
    this->caelum_voice_turn = std::make_unique<voice::TurnServer>(config);
    config_log.info_f("Caelum voice: TURN relay listening on UDP port {}", this->caelum_voice_turn->port());
  } catch (const std::exception& e) {
    config_log.warning_f("Caelum voice: {}; players behind strict routers may not hear each other", e.what());
  }
}
