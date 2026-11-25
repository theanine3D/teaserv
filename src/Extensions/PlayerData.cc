#include "PlayerData.hh"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace teaserv::extensions {
namespace {

bool plain(const std::string& text, size_t max, bool allow_empty) {
  return (allow_empty || !text.empty()) && text.size() <= max && text.find('\0') == std::string::npos;
}

bool valid_field_name(const std::string& name) {
  return !name.empty() && name.size() <= PlayerData::MAX_KEY_BYTES && std::all_of(name.begin(), name.end(), [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
  });
}

void sort_entries(std::vector<BoardEntry>& entries) {
  std::sort(entries.begin(), entries.end(), [](const BoardEntry& a, const BoardEntry& b) {
    if (a.score != b.score) return a.score > b.score;
    if (a.reached != b.reached) return a.reached < b.reached;
    return a.player_key < b.player_key;
  });
}

void require_key(const std::string& key) {
  if (!valid_player_key(key)) throw std::runtime_error("invalid player key");
}

} // namespace

bool valid_player_key(const std::string& key) {
  return key.size() == 25 && key[0] == 'p' && std::all_of(key.begin() + 1, key.end(), [](unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}

bool valid_board_name(const std::string& name) {
  return valid_field_name(name);
}

std::optional<std::string> PlayerData::get(const std::string& player_key, const std::string& name) const {
  require_key(player_key);
  if (!valid_field_name(name)) throw std::runtime_error("invalid player storage key");
  auto player = this->players.find(player_key);
  if (player == this->players.end()) return std::nullopt;
  auto value = player->second.values.find(name);
  if (value == player->second.values.end()) return std::nullopt;
  return value->second;
}

void PlayerData::set(const std::string& player_key, const std::string& name, const std::optional<std::string>& value) {
  require_key(player_key);
  if (!valid_field_name(name)) throw std::runtime_error("invalid player storage key");
  auto player = this->players.find(player_key);
  if (!value) {
    if (player != this->players.end() && player->second.values.erase(name)) {
      if (player->second.values.empty() && player->second.grant_ids.empty()) this->players.erase(player);
      this->dirty = true;
    }
    return;
  }
  if (!plain(*value, MAX_VALUE_BYTES, true)) throw std::runtime_error("player storage value is too long or contains a zero byte");
  if (player == this->players.end()) {
    if (this->players.size() >= MAX_PLAYERS) throw std::runtime_error("player storage quota exceeded");
    player = this->players.emplace(player_key, Player{}).first;
  }
  if (!player->second.values.count(name) && player->second.values.size() >= MAX_KEYS_PER_PLAYER) {
    throw std::runtime_error("player storage key quota exceeded");
  }
  player->second.values[name] = *value;
  this->dirty = true;
}

bool PlayerData::grant_id_used(const std::string& player_key, const std::string& grant_id) const {
  auto player = this->players.find(player_key);
  if (player == this->players.end()) return false;
  const auto& ids = player->second.grant_ids;
  return std::find(ids.begin(), ids.end(), grant_id) != ids.end();
}

void PlayerData::remember_grant_id(const std::string& player_key, const std::string& grant_id) {
  require_key(player_key);
  if (!plain(grant_id, MAX_GRANT_ID_BYTES, false)) throw std::runtime_error("invalid grant ID");
  auto player = this->players.find(player_key);
  if (player == this->players.end()) {
    if (this->players.size() >= MAX_PLAYERS) throw std::runtime_error("player storage quota exceeded");
    player = this->players.emplace(player_key, Player{}).first;
  }
  auto& ids = player->second.grant_ids;
  if (std::find(ids.begin(), ids.end(), grant_id) != ids.end()) return;
  ids.emplace_back(grant_id);
  while (ids.size() > MAX_GRANT_IDS_PER_PLAYER) ids.pop_front();
  this->dirty = true;
}

int64_t PlayerData::board_add(const std::string& board, const std::string& player_key, int64_t delta, const std::string& name) {
  require_key(player_key);
  if (!valid_board_name(board)) throw std::runtime_error("invalid board name");
  if (!plain(name, MAX_NAME_BYTES, true)) throw std::runtime_error("board display name is too long");
  auto it = this->boards.find(board);
  if (it == this->boards.end()) {
    if (this->boards.size() >= MAX_BOARDS) throw std::runtime_error("board quota exceeded");
    it = this->boards.emplace(board, Board{}).first;
  }
  auto& b = it->second;
  auto entry = b.entries.find(player_key);
  if (entry == b.entries.end()) {
    if (b.entries.size() >= MAX_BOARD_ENTRIES) throw std::runtime_error("board entry quota exceeded");
    entry = b.entries.emplace(player_key, BoardEntry{player_key, name, 0, 0}).first;
  }
  auto& e = entry->second;
  if ((delta > 0 && e.score > std::numeric_limits<int64_t>::max() - delta) ||
      (delta < 0 && e.score < std::numeric_limits<int64_t>::min() - delta)) {
    throw std::runtime_error("board score overflow");
  }
  e.score += delta;
  if (!name.empty()) e.name = name;
  if (delta != 0 || !e.reached) e.reached = b.next_reached++;
  this->dirty = true;
  return e.score;
}

std::optional<int64_t> PlayerData::board_get(const std::string& board, const std::string& player_key) const {
  require_key(player_key);
  if (!valid_board_name(board)) throw std::runtime_error("invalid board name");
  auto it = this->boards.find(board);
  if (it == this->boards.end()) return std::nullopt;
  auto entry = it->second.entries.find(player_key);
  if (entry == it->second.entries.end()) return std::nullopt;
  return entry->second.score;
}

std::vector<BoardEntry> PlayerData::board_top(const std::string& board, size_t count) const {
  static const std::string suffix = ".previous";
  bool previous = board.size() > suffix.size() && board.compare(board.size() - suffix.size(), suffix.size(), suffix) == 0;
  std::string name = previous ? board.substr(0, board.size() - suffix.size()) : board;
  if (!valid_board_name(name)) throw std::runtime_error("invalid board name");
  if (count < 1 || count > MAX_TOP) throw std::runtime_error("board_top count must be 1..50");
  auto it = this->boards.find(name);
  if (it == this->boards.end()) return {};
  std::vector<BoardEntry> result;
  if (previous) {
    result = it->second.previous;
  } else {
    result.reserve(it->second.entries.size());
    for (const auto& [key, entry] : it->second.entries) {
      (void)key;
      result.emplace_back(entry);
    }
    sort_entries(result);
  }
  if (result.size() > count) result.resize(count);
  return result;
}

bool PlayerData::board_reset(const std::string& board) {
  if (!valid_board_name(board)) throw std::runtime_error("invalid board name");
  auto it = this->boards.find(board);
  if (it == this->boards.end()) return false;
  std::vector<BoardEntry> all;
  all.reserve(it->second.entries.size());
  for (const auto& [key, entry] : it->second.entries) {
    (void)key;
    all.emplace_back(entry);
  }
  sort_entries(all);
  if (all.size() > MAX_TOP) all.resize(MAX_TOP);
  it->second.previous = std::move(all);
  it->second.entries.clear();
  it->second.next_reached = 1;
  this->dirty = true;
  return true;
}

void PlayerData::validate() const {
  if (this->players.size() > MAX_PLAYERS || this->boards.size() > MAX_BOARDS) throw std::runtime_error("player data quota exceeded");
  for (const auto& [key, player] : this->players) {
    require_key(key);
    if (player.values.size() > MAX_KEYS_PER_PLAYER || player.grant_ids.size() > MAX_GRANT_IDS_PER_PLAYER) {
      throw std::runtime_error("player data quota exceeded");
    }
    for (const auto& [name, value] : player.values) {
      if (!valid_field_name(name) || !plain(value, MAX_VALUE_BYTES, true)) throw std::runtime_error("invalid player data entry");
    }
    for (const auto& id : player.grant_ids) {
      if (!plain(id, MAX_GRANT_ID_BYTES, false)) throw std::runtime_error("invalid stored grant ID");
    }
  }
  for (const auto& [name, board] : this->boards) {
    if (!valid_board_name(name) || board.entries.size() > MAX_BOARD_ENTRIES || board.previous.size() > MAX_TOP) {
      throw std::runtime_error("invalid board data");
    }
    for (const auto& [key, entry] : board.entries) {
      require_key(key);
      if (entry.player_key != key || !plain(entry.name, MAX_NAME_BYTES, true)) throw std::runtime_error("invalid board entry");
    }
    for (const auto& entry : board.previous) {
      require_key(entry.player_key);
      if (!plain(entry.name, MAX_NAME_BYTES, true)) throw std::runtime_error("invalid board entry");
    }
  }
}

} // namespace teaserv::extensions
