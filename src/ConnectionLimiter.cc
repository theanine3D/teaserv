#include "ConnectionLimiter.hh"

#include <format>
#include <stdexcept>

ConnectionLimitConfig::ConnectionLimitConfig() {
  auto exempt_json = phosg::JSON::list();
  exempt_json.emplace_back("127.0.0.0/8");
  this->exempt = IPV4RangeSet(exempt_json);
}

ConnectionLimitConfig ConnectionLimitConfig::from_json(const phosg::JSON* json) {
  ConnectionLimitConfig ret;
  if (!json) {
    return ret;
  }
  if (!json->is_dict()) {
    throw std::runtime_error("ConnectionLimits must be an object");
  }
  ret.enabled = json->get_bool("Enabled", true);

  int64_t attempts = json->get_int("MaxConnectionsPerMinute", 30);
  if (attempts < 1 || attempts > 100000) {
    throw std::runtime_error("ConnectionLimits.MaxConnectionsPerMinute must be between 1 and 100000");
  }
  ret.max_attempts_per_window = attempts;

  int64_t cooldown_minutes = json->get_int("CooldownMinutes", 10);
  if (cooldown_minutes < 1 || cooldown_minutes > 7 * 24 * 60) {
    throw std::runtime_error("ConnectionLimits.CooldownMinutes must be between 1 and 10080 (one week)");
  }
  ret.cooldown_usecs = static_cast<uint64_t>(cooldown_minutes) * 60ULL * 1000000ULL;

  int64_t concurrent = json->get_int("MaxConnectionsPerAddress", 10);
  if (concurrent < 0 || concurrent > 100000) {
    throw std::runtime_error("ConnectionLimits.MaxConnectionsPerAddress must be between 0 (no limit) and 100000");
  }
  ret.max_concurrent = concurrent;

  try {
    ret.exempt = IPV4RangeSet(json->at("ExemptAddresses"));
  } catch (const std::out_of_range&) {
  }
  return ret;
}

ConnectionLimiter::Slot::Slot(std::shared_ptr<ConnectionLimiter> limiter, uint32_t addr)
    : limiter(limiter), addr(addr) {}

ConnectionLimiter::Slot::~Slot() {
  auto l = this->limiter.lock();
  if (l) {
    l->release(this->addr);
  }
}

std::shared_ptr<ConnectionLimiter> ConnectionLimiter::create() {
  std::shared_ptr<ConnectionLimiter> ret(new ConnectionLimiter());
  ret->self = ret;
  return ret;
}

ConnectionLimiter::Result ConnectionLimiter::check(
    uint32_t addr, const ConnectionLimitConfig& config, uint64_t now_usecs) {
  Result ret;
  if (!config.enabled || config.exempt.check(addr)) {
    return ret;
  }

  std::lock_guard g(this->lock);
  this->sweep_locked(config, now_usecs);

  auto& e = this->entries[addr];
  if (e.cooldown_end_usecs > now_usecs) {
    ret.decision = Decision::IN_COOLDOWN;
    ret.cooldown_remaining_usecs = e.cooldown_end_usecs - now_usecs;
    return ret;
  }

  if (e.attempts_in_window == 0 || now_usecs - e.window_start_usecs >= config.window_usecs) {
    e.window_start_usecs = now_usecs;
    e.attempts_in_window = 0;
  }
  e.attempts_in_window++;
  if (e.attempts_in_window > config.max_attempts_per_window) {
    e.cooldown_end_usecs = now_usecs + config.cooldown_usecs;
    e.attempts_in_window = 0; // a fresh window starts when the cooldown ends
    ret.decision = Decision::STARTED_COOLDOWN;
    ret.cooldown_remaining_usecs = config.cooldown_usecs;
    return ret;
  }

  if (config.max_concurrent && e.open >= config.max_concurrent) {
    ret.decision = Decision::TOO_MANY_OPEN;
    return ret;
  }

  e.open++;
  ret.slot = std::make_shared<Slot>(this->self.lock(), addr);
  return ret;
}

void ConnectionLimiter::release(uint32_t addr) {
  std::lock_guard g(this->lock);
  auto it = this->entries.find(addr);
  if (it != this->entries.end() && it->second.open > 0) {
    it->second.open--;
  }
}

void ConnectionLimiter::sweep_locked(const ConnectionLimitConfig& config, uint64_t now_usecs) {
  // Forget addresses with nothing left to remember, at most once per window, so the table cannot grow without bound
  if (now_usecs - this->last_sweep_usecs < config.window_usecs) {
    return;
  }
  this->last_sweep_usecs = now_usecs;
  for (auto it = this->entries.begin(); it != this->entries.end();) {
    const auto& e = it->second;
    bool window_over = (e.attempts_in_window == 0) || (now_usecs - e.window_start_usecs >= config.window_usecs);
    if (e.open == 0 && e.cooldown_end_usecs <= now_usecs && window_over) {
      it = this->entries.erase(it);
    } else {
      it++;
    }
  }
}

size_t ConnectionLimiter::tracked_addresses() const {
  std::lock_guard g(this->lock);
  return this->entries.size();
}

size_t ConnectionLimiter::open_connections(uint32_t addr) const {
  std::lock_guard g(this->lock);
  auto it = this->entries.find(addr);
  return (it == this->entries.end()) ? 0 : it->second.open;
}

const char* name_for_connection_limit_decision(ConnectionLimiter::Decision decision) {
  switch (decision) {
    case ConnectionLimiter::Decision::ALLOW:
      return "allowed";
    case ConnectionLimiter::Decision::STARTED_COOLDOWN:
      return "too many connections per minute; cooldown started";
    case ConnectionLimiter::Decision::IN_COOLDOWN:
      return "on cooldown";
    case ConnectionLimiter::Decision::TOO_MANY_OPEN:
      return "too many open connections";
    default:
      return "unknown";
  }
}
