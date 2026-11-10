#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace teaserv::extensions {

struct LobbyPlayerView {
  uint64_t client_id = 0;
  size_t slot = 0;
  std::string player_name;
  std::string client_version;
  bool is_leader = false;
};

// Plain snapshots, with no account information, upstream objects, or handles.
struct LobbyView {
  int64_t lobby_id = 0;
  std::string name;
  bool is_game = false;
  uint8_t event = 0;
  size_t capacity = 12;
  std::vector<LobbyPlayerView> players;
};

// Bound native memory and verify the adapter returned the caller's own lobby.
void validate_lobby_view(const LobbyView& view, uint64_t caller_id, int64_t lobby_id);

} // namespace teaserv::extensions
