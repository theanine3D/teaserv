#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace teaserv::extensions {

struct GameSnapshot {
  int64_t lobby_id = 0;
  std::string name;
  uint8_t episode = 1;
  uint8_t mode = 0;
  uint8_t difficulty = 0;
  std::optional<uint32_t> quest_number = {};
  bool quest_joinable = false;
};

struct GameAdmission {
  bool is_game = false;
  bool spectator = false;
  bool playback = false;
  bool authenticated = false;
  bool connected = false;
  bool proxy = false;
  bool local_session = false;
  bool current_member = false;
};

void validate_game_snapshot(const GameSnapshot& snapshot);

enum class QuestAcknowledgementKind { INITIAL_LOAD,
  JOINABLE_BB_READY,
  JOINABLE_PLAYER_VISIBLE };

struct QuestLoadAcknowledgement {
  GameSnapshot game;
  uint64_t client_id = 0;
  std::string client_version;
  QuestAcknowledgementKind kind = QuestAcknowledgementKind::INITIAL_LOAD;
};
void validate_quest_acknowledgement(const QuestLoadAcknowledgement& snapshot);

// Tracks primary games independently of packages and player occupancy. An empty
// persistent game stays admitted until the server actually removes its registry entry.
class GameLifecycle {
public:
  static bool eligible(const GameAdmission& admission);
  bool begin(int64_t lobby_id, const GameAdmission& admission);
  bool end(int64_t lobby_id);
  bool assign_quest(int64_t lobby_id, uint32_t quest_number, bool joinable = false);
  bool load_quest_map(int64_t lobby_id, uint32_t quest_number);
  bool member_joined(int64_t lobby_id, uint64_t client_id);
  void member_left(int64_t lobby_id, uint64_t client_id);
  void forget_client(uint64_t client_id);
  bool join_ready(int64_t lobby_id, uint64_t client_id, const GameAdmission& admission,
      bool was_loading, bool initial_ready);
  bool acknowledge_quest(int64_t lobby_id, uint64_t client_id, uint32_t quest_number, const GameAdmission& admission);
  bool acknowledge_joinable_quest(int64_t lobby_id, uint64_t client_id, uint32_t quest_number, const GameAdmission& admission);

private:
  struct MemberState {
    bool join_ready = false;
    bool quest_acknowledged = false;
    bool joinable_quest_acknowledged = false;
  };
  struct GameState {
    std::optional<uint32_t> number = {};
    bool map_loaded = false;
    bool joinable = false;
    std::map<uint64_t, MemberState> members;
  };
  std::map<int64_t, GameState> games;
};

} // namespace teaserv::extensions
