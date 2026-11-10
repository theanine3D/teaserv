#pragma once

#include <map>
#include <optional>

#include "Runtime.hh"

namespace teaserv::extensions {

// Local game-server sessions, not accounts or persistent player identities.
// Admission eligibility is checked by Manager before calling begin().
class SessionLifecycle {
public:
  bool begin(Context context) {
    if (!context.client_id)
      return false;
    context.allow_player_actions = false;
    return this->sessions.emplace(context.client_id, std::move(context)).second;
  }

  void observe(Context context) {
    auto it = this->sessions.find(context.client_id);
    if (it != this->sessions.end()) {
      context.allow_player_actions = false;
      it->second = std::move(context);
    }
  }

  std::optional<Context> end(uint64_t client_id) {
    auto it = this->sessions.find(client_id);
    if (it == this->sessions.end())
      return std::nullopt;
    Context context = std::move(it->second);
    this->sessions.erase(it); // Consume before callbacks; duplicate cleanup is harmless.
    return context;
  }

  void forget(uint64_t client_id) { this->sessions.erase(client_id); }

private:
  std::map<uint64_t, Context> sessions;
};

} // namespace teaserv::extensions
