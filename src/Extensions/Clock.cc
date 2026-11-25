#include "Clock.hh"

#include <stdexcept>

namespace teaserv::extensions {
namespace {

int64_t floor_div(int64_t a, int64_t b) {
  int64_t q = a / b;
  return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

// Howard Hinnant's civil-from-days algorithm.
void civil_from_days(int64_t z, int& y, int& m, int& d) {
  z += 719468;
  const int64_t era = floor_div(z, 146097);
  const int64_t doe = z - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  y = static_cast<int>(yoe + era * 400 + (m <= 2 ? 1 : 0));
}

int64_t days_from_civil(int y, int m, int d) {
  y -= m <= 2;
  const int64_t era = floor_div(y, 400);
  const int64_t yoe = y - era * 400;
  const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

void check_set(const std::set<int>& values, int low, int high, const char* name) {
  for (int value : values) {
    if (value < low || value > high) throw std::runtime_error(std::string("schedule ") + name + " is out of range");
  }
}

} // namespace

ClockTime clock_time(int64_t utc_seconds, int32_t offset_seconds) {
  ClockTime t;
  t.utc = utc_seconds;
  t.offset_seconds = offset_seconds;
  const int64_t local = utc_seconds + offset_seconds;
  t.day_number = floor_div(local, 86400);
  const int64_t in_day = local - t.day_number * 86400;
  t.minute_number = floor_div(local, 60);
  t.hour = static_cast<int>(in_day / 3600);
  t.minute = static_cast<int>((in_day / 60) % 60);
  t.second = static_cast<int>(in_day % 60);
  civil_from_days(t.day_number, t.year, t.month, t.day);
  // 1970-01-01 was a Thursday (ISO weekday 4).
  t.weekday = static_cast<int>(((t.day_number % 7) + 7 + 3) % 7) + 1;
  t.yday = static_cast<int>(t.day_number - days_from_civil(t.year, 1, 1)) + 1;
  // ISO week: the week containing this date's Thursday decides the ISO year.
  const int64_t thursday = t.day_number - (t.weekday - 1) + 3;
  int ty, tm, td;
  civil_from_days(thursday, ty, tm, td);
  t.iso_year = ty;
  t.iso_week = static_cast<int>((thursday - days_from_civil(ty, 1, 1)) / 7) + 1;
  return t;
}

int64_t civil_to_seconds(int year, int month, int day, int hour, int minute, int second) {
  return days_from_civil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second;
}

void ScheduleSpec::validate() const {
  if (this->every_minutes) {
    if (this->every_minutes < 1 || this->every_minutes > 1440) throw std::runtime_error("schedule every_minutes must be 1..1440");
    if (!this->minutes.empty() || !this->hours.empty() || !this->weekdays.empty() || !this->days.empty() || !this->months.empty()) {
      throw std::runtime_error("schedule every_minutes cannot be combined with other fields");
    }
    return;
  }
  if (this->minutes.empty()) throw std::runtime_error("schedule needs minute or every_minutes");
  check_set(this->minutes, 0, 59, "minute");
  check_set(this->hours, 0, 23, "hour");
  check_set(this->weekdays, 1, 7, "weekday");
  check_set(this->days, 1, 31, "day");
  check_set(this->months, 1, 12, "month");
}

bool ScheduleSpec::matches(const ClockTime& t) const {
  if (this->every_minutes) return t.minute_number % this->every_minutes == 0;
  auto ok = [](const std::set<int>& values, int value) { return values.empty() || values.count(value); };
  return ok(this->minutes, t.minute) && ok(this->hours, t.hour) && ok(this->weekdays, t.weekday) &&
      ok(this->days, t.day) && ok(this->months, t.month);
}

} // namespace teaserv::extensions
