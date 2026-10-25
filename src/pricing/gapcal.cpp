#include "kp/pricing/gapcal.hpp"

#include <cstdlib>
#include <stdexcept>

namespace kp::pricing::gapcal {

std::string gap_path(const std::string& underlying, const std::string& profile,
                     const std::string& root) {
    std::string base = root;
    if (base.empty()) {
        const char* env = std::getenv("KALSHI_ROOT");
        base = env ? env : KP_KALSHI_ROOT_DEFAULT;
    }
    const std::string stem = (profile == "default") ? underlying : (underlying + "." + profile);
    return base + "/var/model/variance_calendar/gap_" + stem + ".json";
}

GapCalendar::GapCalendar(const kp::json::Value& art) {
    underlying_ = art.opt_str("underlying").value_or("");
    profile_    = art.opt_str("profile").value_or("default");
    built_utc_  = art.opt_str("built_utc").value_or("");
    estimator_  = art.opt_str("estimator").value_or("");
    shares_ = art.num_array("shares");
    cum_    = art.num_array("cum");
    if ((int)cum_.size() != GAP_MINUTES + 1) {
        throw std::runtime_error("gap calendar has " + std::to_string(cum_.size())
                                 + " cum points, want " + std::to_string(GAP_MINUTES + 1));
    }
}

GapCalendar GapCalendar::load(const std::string& underlying, const std::string& profile,
                              const std::string& root) {
    return GapCalendar(kp::json::parse_file(gap_path(underlying, profile, root)));
}

OptD GapCalendar::minutes_into_gap(const kp::time::Instant& t) const {
    const kp::time::EtParts et = kp::time::to_et(t);

    // PYTHON'S SUBTRACTION HERE IS NAIVE, AND THAT IS THE WHOLE SUBTLETY. `et` and `anchor` are
    // both aware and carry the SAME tzinfo object (config.ET), and the language rule is that when
    // two aware datetimes share a tzinfo the common tzinfo is IGNORED and the result is the naive
    // field difference. So `(et - anchor).total_seconds()` measures WALL CLOCK, not elapsed time,
    // and the DST days come out the opposite way round from the intuitive reading:
    //
    //   spring forward, 2026-03-08 10:00 ET -> 480 + 600 = 1080 wall minutes, past the 1050
    //                                          window, so REFUSED (returns MISSING);
    //   fall back,      2026-11-01 09:00 ET -> 480 + 540 = 1020, INSIDE the window,
    //                                          phi_gap 0.9387.
    //
    // Both verified against the reference. An implementation using real elapsed seconds gets
    // 1020 and 1080 respectively -- i.e. exactly swapped -- and mis-scales every pre-open
    // realized-vol feature on those two days.
    //
    // Wall-clock arithmetic needs no calendar walk at all: the anchor is 16:00 either today or
    // yesterday, so the difference collapses to a time-of-day offset.
    const std::int64_t NS_PER_MIN = 60000000000LL;
    const std::int64_t tod_ns = ((std::int64_t)et.hour * 3600 + (std::int64_t)et.minute * 60
                                 + et.second) * 1000000000LL + et.nanos;
    const std::int64_t close_ns = (std::int64_t)GAP_START_HOUR * 3600000000000LL
                                + (std::int64_t)GAP_START_MINUTE * NS_PER_MIN;
    // The gap containing an instant is the one STARTING at the most recent 16:00, so 02:00
    // belongs to the gap that opened at 16:00 the previous calendar day: add a full day.
    const std::int64_t delta_ns = tod_ns - close_ns
                                + (tod_ns < close_ns ? 86400000000000LL : 0LL);
    // Python's timedelta holds MICROseconds and total_seconds() divides that integer count once.
    // Reproduce that quantisation rather than dividing nanoseconds, so a sub-microsecond instant
    // cannot land the port one bucket away from the reference.
    const double mins = ((double)(delta_ns / 1000LL) / 1000000.0) / 60.0;
    if (mins < 0.0 || mins > (double)GAP_MINUTES) return MISSING;   // inside RTH: varcal owns it
    return mins;
}

double GapCalendar::phi_gap(const kp::time::Instant& t) const {
    const OptD mins = minutes_into_gap(t);
    if (!mins) {
        // Inside RTH: the whole gap has already happened (1.0) or has not started (0.0).
        const kp::time::EtParts et = kp::time::to_et(t);
        const long tod = (long)et.hour * 3600 + (long)et.minute * 60 + et.second;
        const long end = (long)GAP_END_HOUR * 3600 + (long)GAP_END_MINUTE * 60;
        const long start = (long)GAP_START_HOUR * 3600 + (long)GAP_START_MINUTE * 60;
        return (tod >= end && tod < start) ? 1.0 : 0.0;
    }
    if (*mins <= 0.0) return 0.0;
    if (*mins >= (double)GAP_MINUTES) return 1.0;
    const int i = (int)*mins;                   // Python int(): truncation, and mins > 0 here
    const double frac = *mins - (double)i;
    return cum_[(std::size_t)i] + frac * (cum_[(std::size_t)i + 1] - cum_[(std::size_t)i]);
}

double GapCalendar::share_between(const kp::time::Instant& t0,
                                  const kp::time::Instant& t1) const {
    const double d = phi_gap(t1) - phi_gap(t0);
    return d > 0.0 ? d : 0.0;
}

}  // namespace kp::pricing::gapcal
