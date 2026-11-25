#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace teaserv::extensions {

// A player key is the host's opaque, stable name for an account: "p" plus 24
// lowercase hexadecimal digits. Scripts never see account or Guild Card numbers.
bool valid_player_key(const std::string& key);

struct BoardEntry {
  std::string player_key;
  std::string name;
  int64_t score = 0;
  uint64_t reached = 0; // Order in which the current score was reached (lower = earlier); breaks ties
};

// One package's per-player data: a small string store per player, the grant
// IDs already used for that player, and score boards. No Newserv, JSON, or Lua
// dependency; the Manager persists it.
class PlayerData {
public:
  static constexpr size_t MAX_PLAYERS = 50000;
  static constexpr size_t MAX_KEYS_PER_PLAYER = 16;
  static constexpr size_t MAX_KEY_BYTES = 32;
  static constexpr size_t MAX_VALUE_BYTES = 256;
  static constexpr size_t MAX_GRANT_IDS_PER_PLAYER = 32;
  static constexpr size_t MAX_GRANT_ID_BYTES = 64;
  static constexpr size_t MAX_BOARDS = 8;
  static constexpr size_t MAX_BOARD_ENTRIES = 50000;
  static constexpr size_t MAX_NAME_BYTES = 64;
  static constexpr size_t MAX_TOP = 50;

  struct Player {
    std::map<std::string, std::string> values;
    std::deque<std::string> grant_ids; // Oldest first
  };
  struct Board {
    std::map<std::string, BoardEntry> entries; // By player key
    std::vector<BoardEntry> previous; // Sorted result of the period closed by the last reset
    uint64_t next_reached = 1;
  };

  std::map<std::string, Player> players;
  std::map<std::string, Board> boards;
  bool dirty = false;

  // All mutators throw std::runtime_error for invalid input or exceeded quotas.
  std::optional<std::string> get(const std::string& player_key, const std::string& name) const;
  void set(const std::string& player_key, const std::string& name, const std::optional<std::string>& value);

  bool grant_id_used(const std::string& player_key, const std::string& grant_id) const;
  void remember_grant_id(const std::string& player_key, const std::string& grant_id);

  int64_t board_add(const std::string& board, const std::string& player_key, int64_t delta, const std::string& name);
  std::optional<int64_t> board_get(const std::string& board, const std::string& player_key) const;
  // board may end in ".previous" to read the period closed by the last reset.
  std::vector<BoardEntry> board_top(const std::string& board, size_t count) const;
  // Returns false if the board does not exist. The sorted old entries (at most
  // MAX_TOP) become "<board>.previous".
  bool board_reset(const std::string& board);

  void validate() const; // For data loaded from disk
};

bool valid_board_name(const std::string& name);

} // namespace teaserv::extensions
