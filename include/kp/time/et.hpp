// America/New_York time, without <chrono>'s tzdb.
//
// libstdc++ only shipped the C++20 time-zone database in GCC 13, and this box has GCC 11, so
// std::chrono::zoned_time is unavailable. The whole stack is denominated in ET -- varcal.phi
// converts with t.astimezone(config.ET), and the session gates, theta and MIN_MINUTES_TO_CLOSE
// all key off it -- so this cannot be skipped.
//
// The approach is TZ=America/New_York plus localtime_r, which reads the same system IANA tzdata
// that Python's ZoneInfo reads, so the two agree by construction rather than by a hand-rolled DST
// rule that would silently rot. init() sets TZ once at startup.
//
// Note 09:30 and 16:00 ET are never inside a DST transition (transitions are at 02:00), so the
// datetime.combine(date, time, tzinfo=ET) in varcal and greeks is never ambiguous.
#pragma once
#include <cstdint>
#include <string>

namespace kp::time {

// An absolute instant. Nanoseconds since the Unix epoch, UTC -- the same thing Python's
// datetime.timestamp() * 1e9 denotes, with no timezone baked in.
struct Instant {
    std::int64_t ns = 0;
    bool operator<(const Instant& o) const { return ns < o.ns; }
    bool operator<=(const Instant& o) const { return ns <= o.ns; }
    bool operator>(const Instant& o) const { return ns > o.ns; }
    bool operator>=(const Instant& o) const { return ns >= o.ns; }
    bool operator==(const Instant& o) const { return ns == o.ns; }
};

// Call once from main() before any pricing. Sets TZ and tzset().
void init();

// The ET calendar fields of an instant.
struct EtParts { int year, month, day, hour, minute, second; long nanos; };
EtParts to_et(const Instant& t);

// The instant at which the given ET wall-clock reading occurs. Ambiguous readings (the repeated
// hour in November) resolve to the FIRST occurrence, matching zoneinfo's fold=0 default.
Instant from_et(int year, int month, int day, int hour, int minute, int second = 0, long nanos = 0);

// datetime.combine(t.date(), time(h, m), tzinfo=ET) -- the same-day ET wall clock.
Instant et_same_day(const Instant& t, int hour, int minute, int second = 0);

// Python's timedelta.total_seconds(): an exact integer count of microseconds, divided ONCE at
// the end. Never accumulate a double of seconds.
double total_seconds(const Instant& a, const Instant& b);   // b - a

}  // namespace kp::time
