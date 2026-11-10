#pragma once

#include <cstdint>

namespace teaserv::extensions {

// Synchronous adapter evidence, not stored authority or a player snapshot.
// Chat/account permissions are intentionally separate: a muted/less-privileged
// recipient may still receive a reply/whisper from an authorized sender.
struct LiveMembership {
  uint64_t client_id = 0;
  int64_t lobby_id = 0;
  bool authenticated = false;
  bool connected = false;
  bool proxy = false;
  bool local_game_session = false;
  bool supported_version = false;
  bool phase_ready = false;
  bool client_registered = false;
  bool lobby_registered = false;
  bool current_member = false;
};

inline bool eligible_live_member(const LiveMembership& state) {
  return state.client_id && state.lobby_id > 0 && state.authenticated && state.connected && !state.proxy &&
      state.local_game_session && state.supported_version && state.phase_ready && state.client_registered &&
      state.lobby_registered && state.current_member;
}

} // namespace teaserv::extensions
