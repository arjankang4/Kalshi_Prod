// The overnight variance calendar: where in the night the gap's variance actually lands.
//
// Port of the RUNTIME HALF of Kalshi/pricing/gapcal.py -- the reader only
// (GapCalendar, lines 640-719). The fitting half is numpy and stays in Python: this reads the
// artifact it produces, unchanged, from var/model/variance_calendar/gap_<UNDERLYING>.json.
//
// UNITS ARE DELIBERATELY NOT varcal's. varcal's `phi` is a share of one RTH session; `phi_gap` is
// a share of one overnight GAP, and the two become commensurable only after multiplication by
// varcal's `overnight_share`. `rvol/features.variance_time` does exactly that and is the only
// place the two calendars are combined.
//
// THIS IS ON THE LIVE PATH, indirectly and only for part of the session. rvol/live.py:128 calls
// features.window_sigma, which calls variance_time with gcal=None, which loads this artifact.
// The gap branch runs only when the lookback reaches before 09:30 -- and the longest lookback is
// 3h, so it fires every cycle until 12:30 ET and never after. A port that dropped it would work
// all afternoon and mis-scale every realized-vol feature all morning.
//
// `hour_shares` is presentation only and is not ported.
#pragma once
#include <optional>
#include <string>
#include <vector>

#include "kp/json/value.hpp"
#include "kp/time/et.hpp"

namespace kp::pricing::gapcal {

using OptD = std::optional<double>;
inline constexpr std::nullopt_t MISSING = std::nullopt;

// 16:00 -> 09:30 is 17.5 hours of wall clock.
inline constexpr int GAP_MINUTES = 1050;
inline constexpr int GAP_START_HOUR = 16, GAP_START_MINUTE = 0;
inline constexpr int GAP_END_HOUR = 9, GAP_END_MINUTE = 30;

class GapCalendar {
public:
    explicit GapCalendar(const kp::json::Value& art);
    static GapCalendar load(const std::string& underlying,
                            const std::string& profile = "default",
                            const std::string& root = "");

    // Minutes from the preceding 16:00 ET to `t`, or MISSING if `t` is inside RTH.
    //
    // The gap containing an instant is the one STARTING at the most recent 16:00, so 02:00
    // belongs to the gap that opened at 16:00 the previous calendar day.
    //
    // THE ANCHOR IS WALL-CLOCK AND THE ELAPSED TIME IS ABSOLUTE, which is the reference's
    // behaviour and not an accident of it: Python's `anchor -= timedelta(days=1)` on an aware
    // datetime is naive field arithmetic (same wall clock, previous calendar day), while the
    // `et - anchor` that follows is aware-aware and therefore real elapsed seconds. Across the
    // spring-forward those disagree by an hour, and the disagreement is VISIBLE: 10:00 ET on that
    // Sunday is 1020 real minutes after the previous 16:00, which is inside [0, 1050], so this
    // returns a gap position for an instant that is nominally inside RTH instead of MISSING.
    // Reproduced exactly rather than corrected -- it is the reference's answer, the artifact was
    // fitted through it, and "fixing" it here would desync the port from the Python on one day a
    // year without telling anyone.
    OptD minutes_into_gap(const kp::time::Instant& t) const;

    // Cumulative share of the gap's variance realised by `t`, in [0, 1]. Linear within a bucket,
    // exactly as varcal::phi interpolates within a minute.
    double phi_gap(const kp::time::Instant& t) const;

    // Share of the gap's variance falling in [t0, t1). Both must be in the same gap.
    double share_between(const kp::time::Instant& t0, const kp::time::Instant& t1) const;

    const std::string& underlying() const { return underlying_; }
    const std::string& profile() const { return profile_; }
    const std::vector<double>& cum() const { return cum_; }
    const std::vector<double>& shares() const { return shares_; }

private:
    std::string underlying_, profile_, built_utc_, estimator_;
    std::vector<double> shares_, cum_;
};

std::string gap_path(const std::string& underlying, const std::string& profile,
                     const std::string& root);

}  // namespace kp::pricing::gapcal
