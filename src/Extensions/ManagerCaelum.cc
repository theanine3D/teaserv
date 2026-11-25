// Caelum extension channel (F2 flags 0x20-0x2F, src/CaelumExt.h) and the
// extension-owned lobby objects sent over it. Only clients that completed the
// Caelum handshake reach on_F2_Ep3, and only clients that then sent HELLO are
// ever sent a channel command: older Caelum builds end the session on an
// unknown F2 flag. See docs/CAELUM_EXT.md.

#include "Manager.hh"

#include <chrono>

#include "../CaelumExt.h"
#include "../Client.hh"
#include "../Episode3/CaelumLobbies.hh"
#include "../Lobby.hh"
#include "../Loggers.hh"
#include "../SendCommands.hh"
#include "../ServerState.hh"

namespace teaserv::extensions {
namespace {

constexpr uint32_t SERVER_CAPS = CEXT_CAP_MODEL | CEXT_CAP_MUSIC | CEXT_CAP_ATTACH;
constexpr uint32_t MAX_EVENTS_PER_SESSION = 500;

int64_t unix_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string caps_text(uint32_t caps) {
  std::string names;
  if (caps & CEXT_CAP_MODEL) names += "scene.model";
  if (caps & CEXT_CAP_MUSIC) names += std::string(names.empty() ? "" : ",") + "audio.music";
  if (caps & CEXT_CAP_ATTACH) names += std::string(names.empty() ? "" : ",") + "scene.attach";
  return "v=1\ncaps=" + names + "\n";
}

// A public lobby's number as the game's lobby list counts them (block - 1): 0-19 the stock lobbies, 20-63 the
// Caelum extra lobbies; CaelumScenes::NO_LOBBY for a game, the overflow lobby or anything else.
uint8_t lobby_number(const std::shared_ptr<Lobby>& lobby) {
  if (!lobby || lobby->is_game() || !lobby->check_flag(Lobby::Flag::PERSISTENT) || lobby->block < 1) return CaelumScenes::NO_LOBBY;
  uint32_t number = lobby->block - 1;
  if (number > CaelumScenes::LAST_LOBBY || (number >= caelum::lobbies::FIRST_NUMBER) != lobby->check_flag(Lobby::Flag::CAELUM_EXTRA)) {
    return CaelumScenes::NO_LOBBY;
  }
  return static_cast<uint8_t>(number);
}

std::string folded(const std::string& text) {
  std::string out = text;
  for (auto& ch : out) {
    if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
  }
  return out;
}

bool channel_recipient(const std::shared_ptr<Client>& c) {
  return c && c->login && c->caelum_mod_ready && c->version() == Version::GC_EP3 && !c->proxy_session &&
      c->channel && c->channel->connected();
}

} // namespace

void Manager::caelum_ext_message(std::shared_ptr<Client> client, uint8_t flag, const std::string& data) {
  try {
    if (!channel_recipient(client) || data.size() > CEXT_MAX_CLIENT_BODY) return;
    if (flag == CEXT_FLAG_HELLO) {
      uint32_t caps = cext_parse_caps(data.data(), data.size()) & SERVER_CAPS;
      auto& peer = this->caelum_peers[client->id];
      peer.caps = caps;
      peer.max_objects = cext_parse_objects(data.data(), data.size());
      send_command(client, 0xF2, CEXT_FLAG_WELCOME, caps_text(caps));
      // HELLO normally arrives before the first lobby; if not, bring the client up to date now.
      if (auto lobby = client->lobby.lock()) this->caelum_send_snapshot(client, lobby);
      return;
    }
    auto peer = this->caelum_peers.find(client->id);
    if (peer == this->caelum_peers.end()) return; // Reports only after HELLO.
    if (flag == CEXT_FLAG_EVENT) {
      if (peer->second.events >= MAX_EVENTS_PER_SESSION) return;
      ++peer->second.events;
      std::string text = data.substr(0, data.find('\0'));
      for (auto& ch : text) {
        if (static_cast<unsigned char>(ch) < 0x20 || ch == 0x7F) ch = ' ';
      }
      // Reports are diagnostics for the administrator; they never change server state.
      client->log.info_f("Caelum extension report: {}", text.substr(0, 200));
    }
  } catch (const std::exception& e) {
    client->log.warning_f("Caelum extension message ignored: {}", e.what());
  }
}

void Manager::caelum_send_snapshot(std::shared_ptr<Client> client, std::shared_ptr<Lobby> lobby) {
  auto peer = this->caelum_peers.find(client->id);
  if (peer == this->caelum_peers.end() || !peer->second.caps || !channel_recipient(client) || !lobby) return;
  try {
    std::string room = std::format("{}{}", lobby->is_game() ? 'G' : 'L', lobby->lobby_id);
    // An attached object goes by its player's slot in this room (the client draws it on that player).
    auto slot_of = [&](uint64_t session) -> int {
      for (size_t slot = 0; slot < lobby->clients.size(); slot++) {
        const auto& c = lobby->clients[slot];
        if (c && c->id == session && c->lobby_client_id == slot) return static_cast<int>(slot);
      }
      return -1;
    };
    send_command(client, 0xF2, CEXT_FLAG_SNAPSHOT,
        this->caelum.snapshot(lobby_number(lobby), room, unix_ms(), peer->second.max_objects, slot_of,
            (peer->second.caps & CEXT_CAP_ATTACH) != 0));
  } catch (const std::exception& e) {
    client->log.warning_f("Caelum extension snapshot not sent: {}", e.what());
  }
}

std::shared_ptr<Lobby> Manager::public_lobby(uint8_t number) const {
  auto s = this->server.lock();
  if (!s || number > CaelumScenes::LAST_LOBBY) return nullptr;
  if (number >= caelum::lobbies::FIRST_NUMBER) {   // the extra lobbies have fixed IDs
    auto it = s->id_to_lobby.find(caelum::lobbies::LOBBY_ID_BASE + number);
    return it != s->id_to_lobby.end() && lobby_number(it->second) == number ? it->second : nullptr;
  }
  for (const auto& [id, lobby] : s->id_to_lobby) {   // the stock lobbies: by their list position
    (void)id;
    if (lobby_number(lobby) == number) return lobby;
  }
  return nullptr;
}

uint8_t Manager::caelum_lobby_named(const std::string& name) const {
  auto s = this->server.lock();
  if (!s) return CaelumScenes::NO_LOBBY;
  const std::string wanted = folded(name);
  uint8_t found = CaelumScenes::NO_LOBBY;
  for (const auto& [id, lobby] : s->id_to_lobby) {
    (void)id;
    uint8_t number = lobby_number(lobby);
    if (number != CaelumScenes::NO_LOBBY && folded(lobby->name) == wanted && (found == CaelumScenes::NO_LOBBY || number < found)) {
      found = number;   // two lobbies of one name: the first in the list
    }
  }
  return found;
}

std::optional<std::string> Manager::caelum_lobby_name(uint8_t number) const {
  auto lobby = this->public_lobby(number);
  return lobby ? std::optional<std::string>(lobby->name) : std::nullopt;
}

std::optional<Host::Position> Manager::player_position(const Context& context) const {
  auto c = this->live_client(context);
  if (!c) return std::nullopt;
  auto lobby = c->lobby.lock();
  Position where;
  where.x = c->pos.x;
  where.y = c->pos.y;
  where.z = c->pos.z;
  where.floor = c->floor;
  where.is_game = lobby && lobby->is_game();
  where.lobby_number = lobby_number(lobby);
  if (where.lobby_number != CaelumScenes::NO_LOBBY) where.lobby_name = lobby->name;
  return where;
}

void Manager::caelum_sync_lobby(uint8_t number) {
  auto lobby = this->public_lobby(number);
  if (!lobby) return;
  for (size_t slot = 0; slot < lobby->clients.size(); slot++) {
    const auto& c = lobby->clients[slot];
    if (c && c->lobby_client_id == slot) this->caelum_send_snapshot(c, lobby);
  }
}

void Manager::caelum_cleanup_owner(const std::string& owner) {
  for (uint8_t number : this->caelum.remove_owner(owner)) this->caelum_sync_lobby(number);
}

std::string Manager::caelum_info() const {
  return std::format("Caelum extension channel: {} clients, {} lobby objects", this->caelum_peers.size(), this->caelum.object_count());
}

int Manager::caelum_listeners(uint8_t number) const {
  auto lobby = this->public_lobby(number);
  if (!lobby) return -1;
  int count = 0;
  for (size_t slot = 0; slot < lobby->clients.size(); slot++) {
    const auto& c = lobby->clients[slot];
    if (!c || c->lobby_client_id != slot || !channel_recipient(c)) continue;
    auto peer = this->caelum_peers.find(c->id);
    count += peer != this->caelum_peers.end() && peer->second.caps;
  }
  return count;
}

CaelumResult Manager::caelum_object(const std::string& owner, uint8_t lobby, const std::string& key, const CaelumObjectSpec& spec) {
  auto package = this->packages.find(owner);
  if (package == this->packages.end() || !package->second->enabled() || !package->second->grants().count("caelum.scene")) {
    return {false, "unavailable"};
  }
  if (this->caelum_listeners(lobby) < 0) return {false, "no_such_lobby"};
  if (spec.attach) { // the player must be in that lobby now; the object goes when they leave it
    auto room = this->public_lobby(lobby);
    bool here = false;
    for (size_t slot = 0; room && slot < room->clients.size(); slot++) {
      const auto& c = room->clients[slot];
      here = here || (c && c->id == spec.attach && c->lobby_client_id == slot);
    }
    if (!here) return {false, "player_not_here"};
  }
  CaelumObjectSpec copy = spec;
  if (!copy.start_ms) copy.start_ms = unix_ms();
  auto result = this->caelum.set_object(owner, lobby, key, copy);
  if (result.ok) this->caelum_sync_lobby(lobby);
  return result;
}

void Manager::caelum_detach(uint64_t session) {
  for (uint8_t lobby : this->caelum.remove_attached(session)) this->caelum_sync_lobby(lobby);
}

bool Manager::caelum_remove(const std::string& owner, uint8_t lobby, const std::string& key) {
  bool removed = this->caelum.remove_object(owner, lobby, key);
  if (removed) this->caelum_sync_lobby(lobby);
  return removed;
}

CaelumResult Manager::caelum_music(const std::string& owner, uint8_t lobby, const CaelumMusicSpec* spec) {
  auto package = this->packages.find(owner);
  if (package == this->packages.end() || !package->second->enabled() || !package->second->grants().count("caelum.scene")) {
    return {false, "unavailable"};
  }
  if (!spec) {
    bool stopped = this->caelum.stop_music(owner, lobby);
    if (stopped) this->caelum_sync_lobby(lobby);
    return {stopped, stopped ? nullptr : "no_music"};
  }
  if (this->caelum_listeners(lobby) < 0) return {false, "no_such_lobby"};
  CaelumMusicSpec copy = *spec;
  if (!copy.start_ms) copy.start_ms = unix_ms();
  auto result = this->caelum.set_music(owner, lobby, copy);
  if (result.ok) this->caelum_sync_lobby(lobby);
  return result;
}

bool Manager::caelum_clear(const std::string& owner, uint8_t lobby) {
  bool changed = this->caelum.clear(owner, lobby);
  if (changed) this->caelum_sync_lobby(lobby);
  return changed;
}

} // namespace teaserv::extensions
