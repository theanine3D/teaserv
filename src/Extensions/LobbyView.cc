#include "LobbyView.hh"

#include <set>
#include <stdexcept>

namespace teaserv::extensions {

void validate_lobby_view(const LobbyView& view, uint64_t caller_id, int64_t lobby_id) {
  if (!caller_id || lobby_id <= 0 || view.lobby_id != lobby_id || view.capacity < 1 || view.capacity > 12 || view.players.size() > 12 ||
      view.name.size() > 256 || view.name.find('\0') != std::string::npos) {
    throw std::runtime_error("invalid or out-of-scope lobby snapshot");
  }
  std::set<uint64_t> ids;
  std::set<size_t> slots;
  for (const auto& player : view.players) {
    if (!player.client_id || player.slot >= 12 || !ids.emplace(player.client_id).second || !slots.emplace(player.slot).second ||
        player.player_name.size() > 256 || player.player_name.find('\0') != std::string::npos ||
        player.client_version.size() > 32 || player.client_version.find('\0') != std::string::npos) {
      throw std::runtime_error("invalid lobby player snapshot");
    }
  }
  if (!ids.count(caller_id))
    throw std::runtime_error("caller is absent from lobby snapshot");
}

} // namespace teaserv::extensions
