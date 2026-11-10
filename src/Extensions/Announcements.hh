#pragma once

#include <chrono>
#include <iterator>
#include <map>
#include <optional>
#include <string>

namespace teaserv::extensions {

struct AnnouncementResult {
  bool ok = false;
  const char* reason = "unavailable";
};

struct AnnouncementRecipient {
  bool authenticated = false;
  bool connected = false;
  bool proxy = false;
  bool local_game_session = false;
  bool supported_version = false;
  bool phase_ready = false;
};

inline bool eligible_announcement_recipient(const AnnouncementRecipient& recipient) {
  return recipient.authenticated && recipient.connected && !recipient.proxy &&
      recipient.local_game_session && recipient.supported_version && recipient.phase_ready;
}

// Owned by the manager, independently of package runtimes/reloads. Only accepted
// sends consume cooldowns. Expiration plus the global interval bounds the map
// even if administrators repeatedly replace packages with different IDs.
class AnnouncementGate {
public:
  using Clock = std::chrono::steady_clock;
  static constexpr auto PACKAGE_INTERVAL = std::chrono::seconds(30);
  static constexpr auto SERVER_INTERVAL = std::chrono::seconds(5);

  AnnouncementResult admit(const std::string& owner, Clock::time_point now = Clock::now()) {
    for (auto it = this->packages.begin(); it != this->packages.end();) {
      it = it->second <= now ? this->packages.erase(it) : std::next(it);
    }
    if (this->packages.count(owner)) return {false, "package_rate_limited"};
    if (this->next_server && now < *this->next_server) return {false, "server_rate_limited"};
    this->packages.emplace(owner, now + PACKAGE_INTERVAL);
    this->next_server = now + SERVER_INTERVAL;
    return {true, nullptr};
  }

private:
  std::map<std::string, Clock::time_point> packages;
  std::optional<Clock::time_point> next_server;
};

} // namespace teaserv::extensions
