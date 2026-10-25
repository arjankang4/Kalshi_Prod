// The intraday variance calendar: cumulative variance share through the session.
//
// Port of the RUNTIME HALF of Kalshi/pricing/varcal.py -- the reader only
// (VarianceCalendar, lines 596-694). The fitting half is numpy and stays in Python: this reads
// the artifact it produces, unchanged, from var/model/variance_calendar/<UNDERLYING>.json.
//
// Bucket i covers clock minute [09:30+i, 09:31+i), so cum[i] is the share realised as of exactly
// 09:30+i. The 16:00 bucket -- the closing auction -- is therefore realised *at* the 4pm
// settlement rather than spread over the minute after it, which is why phi clamps to 1.0 at
// 16:00:00 rather than interpolating into cum[390]. That is what makes a 16:00 market's remaining
// variance equal the full remaining variance the 4pm option is priced off.
#pragma once
#include <optional>
#include <string>
#include <vector>

#include "kp/json/value.hpp"
#include "kp/time/et.hpp"

namespace kp::pricing::varcal {

using OptD = std::optional<double>;
inline constexpr std::nullopt_t MISSING = std::nullopt;

inline constexpr int SESSION_MINUTES = 391;

class VarianceCalendar {
public:
    explicit VarianceCalendar(const kp::json::Value& art);
    static VarianceCalendar load(const std::string& underlying,
                                 const std::string& profile = "default",
                                 const std::string& root = "");

    // Cumulative variance share at ET instant `t`. 0 before the open, 1 at/after the close.
    double phi(const kp::time::Instant& t) const;

    // Fraction of the variance still ahead at `now` that lands before `expiry`.
    //
    // Multiply w_4pm^2 by this to get w_expiry^2. MISSING when there is no variance left to
    // apportion -- inside the last seconds before the close, where the denominator collapses and
    // the ratio is meaningless rather than merely large.
    OptD remaining_ratio(const kp::time::Instant& now, const kp::time::Instant& expiry) const;

    const std::string& underlying() const { return underlying_; }
    double overnight_share() const { return overnight_share_; }
    long sessions() const { return sessions_; }
    const std::vector<double>& cum() const { return cum_; }

private:
    std::string underlying_, built_utc_;
    std::vector<double> shares_, cum_;
    double overnight_share_ = 0.0;
    long sessions_ = 0;
};

std::string calendar_path(const std::string& underlying, const std::string& profile,
                          const std::string& root);

}  // namespace kp::pricing::varcal
