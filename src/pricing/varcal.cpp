#include "kp/pricing/varcal.hpp"

#include <cstdint>
#include <cstdlib>
#include <stdexcept>

namespace kp::pricing::varcal {

namespace {
// varcal.py: OPEN_T is 09:30, CLOSE_T is 16:00.
constexpr int OPEN_H = 9, OPEN_M = 30, CLOSE_H = 16, CLOSE_M = 0;
}

VarianceCalendar::VarianceCalendar(const kp::json::Value& art) {
    underlying_ = art.opt_str("underlying").value_or("");
    shares_ = art.num_array("shares");
    cum_    = art.num_array("cum");
    overnight_share_ = art.opt_num("overnight_share").value_or(0.0);
    sessions_ = (long)art.opt_num("sessions").value_or(0.0);
    built_utc_ = art.opt_str("built_utc").value_or("");
    if ((int)cum_.size() != SESSION_MINUTES + 1)
        throw std::runtime_error("calendar has " + std::to_string(cum_.size()) +
                                 " cum points, want " + std::to_string(SESSION_MINUTES + 1));
}

std::string calendar_path(const std::string& underlying, const std::string& profile,
                          const std::string& root) {
    std::string base = root;
    if (base.empty()) {
        const char* env = std::getenv("KALSHI_ROOT");
        base = env ? env : KP_KALSHI_ROOT_DEFAULT;
    }
    const std::string suffix = (profile == "default") ? "" : ("." + profile);
    return base + "/var/model/variance_calendar/" + underlying + suffix + ".json";
}

VarianceCalendar VarianceCalendar::load(const std::string& underlying,
                                        const std::string& profile, const std::string& root) {
    const std::string p = calendar_path(underlying, profile, root);
    return VarianceCalendar(kp::json::parse_file(p));
}

double VarianceCalendar::phi(const kp::time::Instant& t) const {
    const kp::time::EtParts et = kp::time::to_et(t);
    // Python: `if et.time() >= CLOSE_T`, a wall-clock time-of-day comparison at MICROSECOND
    // resolution (datetime has no nanoseconds). 16:00:00.000000 itself clamps: the closing
    // auction's variance is realised AT the settlement, not spread over the minute after it.
    const std::int64_t tod_us =
        ((std::int64_t)et.hour * 3600 + et.minute * 60 + et.second) * 1000000LL + et.nanos / 1000;
    constexpr std::int64_t close_us = ((std::int64_t)CLOSE_H * 3600 + CLOSE_M * 60) * 1000000LL;
    if (tod_us >= close_us) return 1.0;

    const kp::time::Instant open_dt = kp::time::et_same_day(t, OPEN_H, OPEN_M, 0);
    const double elapsed = kp::time::total_seconds(open_dt, t) / 60.0;
    if (elapsed <= 0) return 0.0;
    const int i = (int)elapsed;                     // Python int() truncates; elapsed > 0 here
    const double frac = elapsed - i;
    if (i >= SESSION_MINUTES - 1) return cum_[SESSION_MINUTES - 1];
    return cum_[(std::size_t)i] + frac * (cum_[(std::size_t)i + 1] - cum_[(std::size_t)i]);
}

OptD VarianceCalendar::remaining_ratio(const kp::time::Instant& now,
                                       const kp::time::Instant& expiry) const {
    const double p_now = phi(now);
    const double denom = 1.0 - p_now;
    if (denom <= 1e-9) return MISSING;
    if (expiry <= now) return MISSING;
    const double num = phi(expiry) - p_now;
    const double r = num / denom;
    // Python max(0.0, min(1.0, x))
    const double lo = (r > 0.0) ? r : 0.0;
    return (1.0 < lo) ? 1.0 : lo;
}

}  // namespace kp::pricing::varcal
