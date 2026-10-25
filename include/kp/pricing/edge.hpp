// The calibrated R -> required-edge curve.
//
// Port of Kalshi/pricing/edge.py, reading the artifact edgecal/ produces at
// var/model/edge/<UNDERLYING>.json unchanged.
//
// A floor (EDGE_FLOOR) plus a risk bonus that is exponential 0 -> BONUS_KNEE through the bulk of
// the distribution and linear BONUS_KNEE -> BONUS_CAP across the top 1% -- and ABOVE R_max WE
// REFUSE TO QUOTE
// rather than clamp. An R beyond anything a year produced is a market we do not understand, not
// one to charge more for.
#pragma once
#include <optional>
#include <string>
#include <vector>

#include "kp/json/value.hpp"

namespace kp::pricing::edge {

using OptD = std::optional<double>;
inline constexpr std::nullopt_t MISSING = std::nullopt;

// The charge is TWO parts (edge.py): the FLOOR is what we charge to quote at all -- the
// bar a resting order must keep clearing, and the only part the realized-vol lean scales -- and the
// BONUS is what the market's own risk earns on top, required to POST and never to STAY.
inline constexpr double EDGE_FLOOR  = 0.01;   // the base charge, and the exit bar
inline constexpr double BONUS_KNEE  = 0.02;   // the bonus at R_p99
inline constexpr double BONUS_CAP   = 0.08;   // the bonus at R_max
inline constexpr double BONUS_BASE  = 4.0;    // curvature of the exponential leg
// Python computes these as SUMS, so they are sums here too: a sum of two doubles is not, in
// general, the literal of their decimal sum.
inline constexpr double EDGE_KNEE   = EDGE_FLOOR + BONUS_KNEE;
inline constexpr double EDGE_CAP    = EDGE_FLOOR + BONUS_CAP;
inline constexpr long   MICROS      = 1000000;

class Curve {
public:
    explicit Curve(const kp::json::Value& art);
    static Curve load(const std::string& underlying, const std::string& root = "");

    // The RISK part of the charge: 0 at R_min, BONUS_KNEE at R_p99, BONUS_CAP at R_max.
    // MISSING means DO NOT QUOTE.
    OptD bonus_dollars(OptD R) const;

    // Total required edge to POST: EDGE_FLOOR + bonus, or MISSING meaning DO NOT QUOTE. The lean
    // is not in here -- it is applied per SIDE by lean::edge_adjust, and a curve has no side.
    OptD edge_dollars(OptD R) const;

    // The same, in integer micro-dollars -- the unit every threshold comparison uses.
    //
    // Float threshold comparisons are wrong on exact boundaries: theo 0.115 against a 0.11 quote
    // is 0.0050000000000000044 in float, so a bar of exactly 0.005 compared in float would be
    // cleared by noise alone.
    std::optional<long> edge_micros(OptD R) const;

    // Where R sits on the EDGE curve, in [0,1] -- exponential then linear, same as the edge.
    //
    // (edge(R) - floor) / (cap - floor). This is what the tiered rungs step back on, so the back
    // rungs move on exactly the scale the edge does rather than on a separate percentile rank.
    // Because the bulk of the curve is exponential, most markets sit near 0 here: at the median R
    // the position is ~0.04, at R_p99 ~0.26, and only the extreme tail approaches 1. A refused R
    // returns 1.0, the most conservative answer.
    double scale_position(OptD R) const;

    // R's percentile rank in the calibration distribution, in [0,1]. Interpolated across the
    // stored percentile grid rather than recomputed, so the live path never touches the sweep.
    double risk_u(OptD R) const;

    const std::string& underlying() const { return underlying_; }
    double R_min() const { return R_min_; }
    double R_p99() const { return R_p99_; }
    double R_max() const { return R_max_; }
    std::optional<long> n_sessions() const { return n_sessions_; }
    // <Curve SPX R_min=%.5f R_p99=%.5f R_max=%.5f from N sessions>
    std::string repr() const;

private:
    std::string underlying_, built_utc_;
    double R_min_ = 0, R_p99_ = 0, R_max_ = 0;
    std::vector<double> pct_r_, pct_p_;
    std::optional<long> n_sessions_;
};

// The `--edge` off operator override. Since a market without a usable R is no longer quoted at
// all, turning the edge manager off would otherwise quote nothing -- so this installs the flat
// QUOTE_OFFSET everywhere instead.
class FlatCurve {
public:
    OptD edge_dollars(OptD R) const;
    std::optional<long> edge_micros(OptD R) const;
    double scale_position(OptD R) const;
    double risk_u(OptD R) const;
};

std::string curve_path(const std::string& underlying, const std::string& root);

}  // namespace kp::pricing::edge
