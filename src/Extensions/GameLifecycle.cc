#include "GameLifecycle.hh"

#include <algorithm>
#include <stdexcept>

namespace teaserv::extensions {

void validate_game_snapshot(const GameSnapshot& snapshot) {
  if (snapshot.lobby_id <= 0 || snapshot.name.size() > 256 || snapshot.name.find('\0') != std::string::npos ||
      snapshot.episode < 1 || snapshot.episode > 4 || snapshot.mode > 3 || snapshot.difficulty > 3) {
    throw std::runtime_error("invalid game observation snapshot");
  }
}

void validate_quest_acknowledgement(const QuestLoadAcknowledgement& snapshot) {
  validate_game_snapshot(snapshot.game);
  if (!snapshot.game.quest_number || !snapshot.client_id || snapshot.client_version.empty() ||
      snapshot.client_version.size() > 32 || !std::all_of(snapshot.client_version.begin(), snapshot.client_version.end(), [](unsigned char ch) {
        return (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_';
      })) {
    throw std::runtime_error("quest acknowledgement requires a quest, client identity, and bounded version name");
  }
  switch (snapshot.kind) {
    case QuestAcknowledgementKind::INITIAL_LOAD:
      break;
    case QuestAcknowledgementKind::JOINABLE_BB_READY:
      if (!snapshot.game.quest_joinable || snapshot.client_version != "BB_V4") {
        throw std::runtime_error("BB joinable acknowledgement requires a joinable quest and BB client");
      }
      break;
    case QuestAcknowledgementKind::JOINABLE_PLAYER_VISIBLE:
      if (!snapshot.game.quest_joinable || snapshot.client_version == "BB_V4") {
        throw std::runtime_error("visibility acknowledgement requires a joinable quest and non-BB client");
      }
      break;
    default:
      throw std::runtime_error("unknown quest acknowledgement kind");
  }
}

bool GameLifecycle::eligible(const GameAdmission& admission) {
  return admission.is_game && !admission.spectator && !admission.playback && admission.authenticated &&
      admission.connected && !admission.proxy && admission.local_session && admission.current_member;
}

bool GameLifecycle::begin(int64_t lobby_id, const GameAdmission& admission) {
  if (lobby_id <= 0 || !eligible(admission))
    return false;
  return this->games.emplace(lobby_id, GameState{}).second;
}

bool GameLifecycle::end(int64_t lobby_id) {
  return this->games.erase(lobby_id) != 0; // Consume before callbacks.
}

bool GameLifecycle::assign_quest(int64_t lobby_id, uint32_t quest_number, bool joinable) {
  auto it = this->games.find(lobby_id);
  if (it == this->games.end() || it->second.number.has_value())
    return false;
  it->second.number = quest_number; // Record before callbacks; zero is a valid ID.
  it->second.joinable = joinable;
  return true;
}

bool GameLifecycle::load_quest_map(int64_t lobby_id, uint32_t quest_number) {
  auto it = this->games.find(lobby_id);
  if (it == this->games.end() || !it->second.number || *it->second.number != quest_number || it->second.map_loaded)
    return false;
  it->second.map_loaded = true;
  return true;
}

bool GameLifecycle::member_joined(int64_t lobby_id, uint64_t client_id) {
  auto it = this->games.find(lobby_id);
  return client_id && it != this->games.end() && it->second.members.emplace(client_id, MemberState{}).second;
}

void GameLifecycle::member_left(int64_t lobby_id, uint64_t client_id) {
  auto it = this->games.find(lobby_id);
  if (it != this->games.end())
    it->second.members.erase(client_id);
}

void GameLifecycle::forget_client(uint64_t client_id) {
  for (auto& [id, state] : this->games) {
    (void)id;
    state.members.erase(client_id);
  }
}

bool GameLifecycle::join_ready(int64_t lobby_id, uint64_t client_id, const GameAdmission& admission,
    bool was_loading, bool initial_ready) {
  if (!eligible(admission) || !was_loading || !initial_ready)
    return false;
  auto game = this->games.find(lobby_id);
  if (game == this->games.end())
    return false;
  auto member = game->second.members.find(client_id);
  if (member == game->second.members.end() || member->second.join_ready)
    return false;
  member->second.join_ready = true; // Consume before callbacks, even when an observer fails.
  return true;
}

bool GameLifecycle::acknowledge_quest(int64_t lobby_id, uint64_t client_id, uint32_t quest_number, const GameAdmission& admission) {
  if (!eligible(admission))
    return false;
  auto game = this->games.find(lobby_id);
  if (game == this->games.end() || !game->second.number || *game->second.number != quest_number)
    return false;
  auto member = game->second.members.find(client_id);
  if (member == game->second.members.end() || member->second.quest_acknowledged)
    return false;
  member->second.quest_acknowledged = true; // Independent of join readiness/map preparation; consume before callbacks.
  return true;
}

bool GameLifecycle::acknowledge_joinable_quest(int64_t lobby_id, uint64_t client_id, uint32_t quest_number, const GameAdmission& admission) {
  if (!eligible(admission))
    return false;
  auto game = this->games.find(lobby_id);
  if (game == this->games.end() || !game->second.number || *game->second.number != quest_number || !game->second.joinable)
    return false;
  auto member = game->second.members.find(client_id);
  if (member == game->second.members.end() || member->second.joinable_quest_acknowledged)
    return false;
  member->second.joinable_quest_acknowledged = true; // One late-join signal per membership; consume before callbacks.
  return true;
}

} // namespace teaserv::extensions
