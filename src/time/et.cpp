#include "kp/time/et.hpp"

#include <cstdlib>
#include <ctime>

namespace kp::time {

namespace {
constexpr std::int64_t NS = 1000000000LL;
}

void init() {
    ::setenv("TZ", "America/New_York", 1);
    ::tzset();
}

EtParts to_et(const Instant& t) {
    // Floor division, so instants before the epoch still land on the right second.
    std::int64_t secs = t.ns / NS;
    std::int64_t rem  = t.ns % NS;
    if (rem < 0) { rem += NS; --secs; }
    std::time_t tt = (std::time_t)secs;
    std::tm tm{};
    ::localtime_r(&tt, &tm);
    return {tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
            tm.tm_hour, tm.tm_min, tm.tm_sec, (long)rem};
}

namespace {
long long civil_days(long long y, unsigned m, unsigned d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}
}  // namespace

Instant from_et(int year, int month, int day, int hour, int minute, int second, long nanos) {
    // zoneinfo, fold=0, exactly -- NOT mktime(tm_isdst=-1), whose choice for an ambiguous or
    // non-existent reading is glibc's and was measured to pick the SECOND occurrence.
    //   * a reading that exists once: that instant;
    //   * the repeated November hour: the FIRST occurrence (EDT, the earlier instant);
    //   * the skipped March hour: the offset in force BEFORE the transition (EST), which lands
    //     the instant an hour past the reading, exactly as datetime(..., fold=0).timestamp().
    // America/New_York is only ever UTC-4 or UTC-5, so the two candidates are the whole search.
    const long long wall = (civil_days(year, (unsigned)month, (unsigned)day) * 86400LL
                            + hour * 3600LL + minute * 60LL + second);
    const auto same = [&](long long utc_s) {
        const EtParts p = to_et(Instant{utc_s * NS});
        return p.year == year && p.month == month && p.day == day && p.hour == hour
               && p.minute == minute && p.second == second;
    };
    const long long edt = wall + 4 * 3600LL, est = wall + 5 * 3600LL;
    const bool e4 = same(edt), e5 = same(est);
    long long utc;
    if (e4 && e5) utc = edt;                // ambiguous: the first occurrence
    else if (e4) utc = edt;
    else if (e5) utc = est;
    else utc = est;                         // in the gap: the pre-transition (standard) offset
    return {(std::int64_t)utc * NS + nanos};
}

Instant et_same_day(const Instant& t, int hour, int minute, int second) {
    const EtParts p = to_et(t);
    return from_et(p.year, p.month, p.day, hour, minute, second, 0);
}

double total_seconds(const Instant& a, const Instant& b) {
    // Python's timedelta.total_seconds() is days*86400 + seconds + microseconds/1e6: an exact
    // integer microsecond count divided once. Mirror that, truncating ns to us as datetime does.
    const std::int64_t us_a = a.ns / 1000, us_b = b.ns / 1000;
    return (double)(us_b - us_a) / 1e6;
}

}  // namespace kp::time
