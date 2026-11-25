#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace teaserv::extensions {

// Extension-owned objects and music in Caelum-only lobbies, sent to Caelum
// clients over the F2 extension channel (src/CaelumExt.h, docs/CAELUM_EXT.md).
// The registry is the single source of truth: every change and every lobby
// join sends the room's whole snapshot, so late joiners see the same state.
// No Newserv, JSON or Lua dependency.

struct CaelumObjectSpec {
  std::string model; // Required: a BML archive (file base name)
  std::string part; // Optional: the model inside the BML (default: its first model)
  std::string texture; // Reserved (a BML carries its textures)
  std::string motion; // Optional: the motion inside the BML
  double x = 0, y = 0, z = 0;
  double rot_y = 0; // Degrees
  double scale = 1;
  int64_t start_ms = 0; // Unix ms the motion started; 0 = "now" (filled by the host)
  bool loop = true;
  bool visible = true; // false: clients only load the files (prefetch)
  double rot_x = 0, rot_z = 0; // Degrees, turned after rot_y
  // Attached to a player (a hat, a pet): the player's session ID, 0 = placed in the lobby. The object is shown only
  // while that player is in the lobby, and removed when the player leaves it (remove_attached).
  uint64_t attach = 0;
  int bone = -1; // Attached: skeleton node 0-63 (58 = the head), -1 = the player's root (position and facing)
  bool upright = false; // Bone: the node's place, the player's facing (a hat that stays level)
  bool follow = false; // Root: trails the player, turning the way it moves (a pet)
  std::string move_motion; // Follow: the motion while moving (default: motion)
};

struct CaelumMusicSpec {
  std::string file;
  int64_t start_ms = 0;
  bool loop = true;
};

struct CaelumResult {
  bool ok = false;
  const char* reason = nullptr;
};

class CaelumScenes {
public:
  static constexpr size_t MAX_OBJECTS_PER_LOBBY = 64; // CEXT_MAX_OBJECTS (all packages together)
  static constexpr size_t MAX_OBJECTS_PER_OWNER = 256; // one package, all lobbies together
  static constexpr size_t LEGACY_OBJECTS = 16; // CEXT_LEGACY_OBJECTS: what a client without "objects=" can hold
  static constexpr size_t MAX_KEY_BYTES = 32;
  // Lobby numbers as the game's lobby list counts them (block - 1): 0-19 the 20 stock lobbies, 20-63 the Caelum
  // extra lobbies (caelum-lobbies.txt). Only Caelum clients that sent HELLO are sent anything, in any of them.
  static constexpr uint8_t FIRST_LOBBY = 0;
  static constexpr uint8_t LAST_LOBBY = 63;
  static constexpr uint8_t NO_LOBBY = 0xFF;   // "not a public lobby": a game, the overflow lobby, an unknown name

  // Mutators validate everything and return a stable reason code on refusal.
  CaelumResult set_object(const std::string& owner, uint8_t lobby, const std::string& key, const CaelumObjectSpec& spec);
  bool remove_object(const std::string& owner, uint8_t lobby, const std::string& key);
  CaelumResult set_music(const std::string& owner, uint8_t lobby, const CaelumMusicSpec& spec);
  bool stop_music(const std::string& owner, uint8_t lobby);
  // Removes everything the owner has in one lobby; returns true if anything changed.
  bool clear(const std::string& owner, uint8_t lobby);
  // Removes everything the owner has anywhere; returns the lobbies that changed.
  std::set<uint8_t> remove_owner(const std::string& owner);
  // Removes every package's objects attached to that player (who left the lobby); returns the lobbies that changed.
  std::set<uint8_t> remove_attached(uint64_t session);

  size_t object_count() const;
  bool empty(uint8_t lobby) const;
  // The SNAPSHOT body for a room. NO_LOBBY (or a lobby without state) gives an empty room. At most max_objects
  // objects are written, the earliest placed first (a client holds as many as its HELLO said), and never more than
  // CEXT_MAX_SNAPSHOT bytes. Attached objects are written only when attached is true (the client named scene.attach)
  // and slot_of gives their player's lobby slot (-1: not in this room).
  std::string snapshot(uint8_t lobby, const std::string& room, int64_t now_ms, size_t max_objects = MAX_OBJECTS_PER_LOBBY,
      const std::function<int(uint64_t)>& slot_of = nullptr, bool attached = false) const;
  std::vector<std::string> describe(const std::string& owner) const;

private:
  struct Object {
    std::string owner, key;
    CaelumObjectSpec spec;
  };
  struct Music {
    std::string owner;
    CaelumMusicSpec spec;
  };
  struct Lobby {
    std::vector<Object> objects; // Creation order, which is also the wire order
    std::optional<Music> music;
  };
  std::map<uint8_t, Lobby> lobbies;
};

bool caelum_valid_file(const std::string& name);

} // namespace teaserv::extensions
