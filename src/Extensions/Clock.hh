#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <string>

namespace teaserv::extensions {

// Wall-clock snapshot for scripts (extensions.now) and schedules. The Lua
// sandbox has no os library; this is the only source of dates. All fields are
// in the server's configured time zone except utc.
struct ClockTime {
  int64_t utc = 0; // Unix seconds
  int32_t offset_seconds = 0; // Zone offset applied to the fields below
  int year = 1970;
  int month = 1; // 1..12
  int day = 1; // 1..31
  int hour = 0;
  int minute = 0;
  int second = 0;
  int weekday = 4; // ISO: 1 = Monday .. 7 = Sunday
  int yday = 1; // 1..366
  int iso_year = 1970;
  int iso_week = 1; // 1..53
  int64_t day_number = 0; // Days since 1970-01-01 in the zone
  int64_t minute_number = 0; // Minutes since 1970-01-01 00:00 in the zone
};

// Pure calendar arithmetic (proleptic Gregorian); no OS time-zone access.
ClockTime clock_time(int64_t utc_seconds, int32_t offset_seconds);
// Seconds since 1970-01-01 00:00:00 for calendar fields read as if they were UTC.
int64_t civil_to_seconds(int year, int month, int day, int hour, int minute, int second);

// A cron-like rule. An empty set means "any". every_minutes, when nonzero, is
// the only rule: it fires when minute_number is a multiple of it.
struct ScheduleSpec {
  std::set<int> minutes; // 0..59
  std::set<int> hours; // 0..23
  std::set<int> weekdays; // 1..7
  std::set<int> days; // 1..31
  std::set<int> months; // 1..12
  int every_minutes = 0; // 1..1440

  void validate() const; // Throws std::runtime_error
  bool matches(const ClockTime& time) const;
};

} // namespace teaserv::extensions
