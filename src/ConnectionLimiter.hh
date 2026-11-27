#pragma once

// Teaserv: per-IP connection limits against reconnect floods.
//
// Every new TCP connection from an address counts as one attempt. If an address makes more than
// max_attempts_per_window attempts within window_usecs, it is put on cooldown: all its new connections are closed at
// once until cooldown_usecs have passed. Rejected attempts do not count, so the cooldown always ends on time. An
// address may also hold at most max_concurrent open connections. Addresses in `exempt` (by default 127.0.0.0/8) are
// never limited. Clients behind one NAT share an address, so the defaults are generous.
//
// Config key "ConnectionLimits" (see system/config.example.json). The server runs one event thread, but the class
// locks anyway so it stays correct if that changes.

#include <stdint.h>

#include <memory>
#include <mutex>
#include <phosg/JSON.hh>
#include <unordered_map>

#include "IPV4RangeSet.hh"

struct ConnectionLimitConfig {
  bool enabled = true;
  size_t max_attempts_per_window = 30;
  uint64_t window_usecs = 60ULL * 1000000ULL; // 1 minute
  uint64_t cooldown_usecs = 600ULL * 1000000ULL; // 10 minutes
  size_t max_concurrent = 10; // 0 = no limit
  IPV4RangeSet exempt;

  ConnectionLimitConfig(); // exempt = 127.0.0.0/8
  // json may be null (key missing): defaults. Throws std::runtime_error on invalid values.
  static ConnectionLimitConfig from_json(const phosg::JSON* json);
};

class ConnectionLimiter {
public:
  enum class Decision {
    ALLOW = 0,
    STARTED_COOLDOWN, // this attempt went over the limit; the address is now on cooldown
    IN_COOLDOWN,
    TOO_MANY_OPEN,
  };

  // Holds one open-connection slot; the count drops when the last copy is destroyed.
  class Slot {
  public:
    Slot(std::shared_ptr<ConnectionLimiter> limiter, uint32_t addr);
    ~Slot();
    Slot(const Slot&) = delete;
    Slot& operator=(const Slot&) = delete;

  private:
    std::weak_ptr<ConnectionLimiter> limiter;
    uint32_t addr;
  };

  struct Result {
    Decision decision = Decision::ALLOW;
    std::shared_ptr<Slot> slot; // set when decision is ALLOW (null for exempt addresses or a disabled limiter)
    uint64_t cooldown_remaining_usecs = 0;
  };

  static std::shared_ptr<ConnectionLimiter> create();

  // Call once per accepted connection. now_usecs is any monotonic-enough clock in microseconds (tests pass their own).
  Result check(uint32_t addr, const ConnectionLimitConfig& config, uint64_t now_usecs);

  size_t tracked_addresses() const;
  size_t open_connections(uint32_t addr) const;

private:
  ConnectionLimiter() = default;
  void release(uint32_t addr);
  void sweep_locked(const ConnectionLimitConfig& config, uint64_t now_usecs);

  struct Entry {
    uint64_t window_start_usecs = 0;
    size_t attempts_in_window = 0;
    uint64_t cooldown_end_usecs = 0;
    size_t open = 0;
  };

  mutable std::mutex lock;
  std::unordered_map<uint32_t, Entry> entries;
  uint64_t last_sweep_usecs = 0;
  std::weak_ptr<ConnectionLimiter> self;
};

const char* name_for_connection_limit_decision(ConnectionLimiter::Decision decision);
