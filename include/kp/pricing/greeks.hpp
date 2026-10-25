// The three DOLLAR Greeks for a Kalshi binary, and the risk norm R the edge curve consumes.
//
// Port of Kalshi/pricing/greeks.py. Because a Kalshi binary settles $0/$1,
// probability units ARE dollars -- there is no contract multiplier to apply.
//
// There is deliberately NO GAMMA AXIS. For a binary, gamma == vega/(F^2 w) exactly (verified to
// 3.7e-16 over 54 F/w/K combinations), and F and w are constant across a board at any instant --
// so gamma is vega rescaled, and adding it would only double-weight that axis.
#pragma once
#include <functional>
#include <optional>
#include <string>

#include "kp/pricing/theo_core.hpp"
#include "kp/pricing/varcal.hpp"
#include "kp/time/et.hpp"

namespace kp::pricing::greeks {

namespace theo = kp::pricing::theo;

using OptD = std::optional<double>;
inline constexpr std::nullopt_t MISSING = std::nullopt;

inline constexpr double TRADING_DAYS = 252.0;

// DELTA and VEGA divide by w (and w^2), so both diverge as the remaining variance-time goes to
// zero -- an at-the-money binary really does have unbounded delta at expiry. Applied as a FLOOR
// on the remaining variance-time, so a market inside the last MIN_VAR_LEFT_FRACTION of its life is
// measured as if it had that much left.
inline constexpr double MIN_VAR_LEFT_FRACTION = 0.01;

// THETA gets a floor on the remaining CLOCK time instead. Flooring theta's variance-time the way
// delta's is floored does not work: once both endpoints of the one-minute difference hit the same
// floor they return the same w and the difference collapses to exactly 0.000000, which reported
// NO time decay in the highest-theta minutes of the day.
//
// THE FLOOR DOES NOT, AND CANNOT, TAME THE FINAL MINUTE. Once the forward minute reaches
// settlement the contract resolves, so theta becomes (payoff - price), which for an ATM binary is
// +-0.50. That is not an artefact
// to be floored away -- it is what a coin flip one minute from resolution is actually worth. The
// answer is not to quote it: config::MIN_MINUTES_TO_CLOSE refuses those markets explicitly.
inline constexpr double THETA_MIN_SECONDS = 30.0;

// dP/dS: dollars per $1 move in the index. Always positive; peaks at the money.
OptD delta_per_point(OptD F, OptD K, OptD w);

// dP/dsigma_annualised, per ONE vol point (1%). Signed; zero at the money.
//
// The chain rule matters and is easy to get wrong: w = sigma * sqrt(var_left / 252), so
// dP/dsigma = dP/dw * sqrt(var_left / 252), and a vol POINT is a further factor of 0.01. Skipping
// either rescales the whole axis and silently reweights R against delta and theta.
OptD vega_per_volpoint(OptD F, OptD K, OptD w, OptD var_left);

// Remaining variance-time from `now` to `settlement`, in session-variance units.
OptD var_left(const varcal::VarianceCalendar& cal, const kp::time::Instant& now,
              const kp::time::Instant& settlement, bool floor = true);

// sigma on the intraday variance clock -> total vol w over the remaining window.
OptD total_vol(OptD sigma_annualised, OptD var_left_units);

// Dollars of value lost per ONE minute of time passing. Signed.
//
// A FINITE one-minute difference through the variance calendar, not an analytic dP/dT, and the
// difference is not cosmetic: varcal.phi is piecewise-linear and deliberately not smooth -- the
// 15:59 bucket carries ~3x a flat minute (the MOC ramp) and midday carries ~0.7x. The next
// minute's ACTUAL variance share is the honest number; an analytic derivative would smooth away
// exactly the feature the calendar exists to capture.
OptD theta_per_minute(OptD F, OptD K, OptD sigma_annualised, const kp::time::Instant& now,
                      const kp::time::Instant& settlement, const varcal::VarianceCalendar& cal);

// The four one-sided spellings of a Greek at one boundary, from d/dx of P(S > K).
//
// Mirrors theo::one_sided_probs so the rule table is a lookup here too -- but THE MAPPING IS NOT
// THE SAME, and that difference is the whole reason this exists. A probability maps `less` to
// 1 - p; a DERIVATIVE maps it to -g, because the constant differentiates away. Reusing
// one_sided_probs on a Greek returns 1 - delta, which on a real board read +0.985 where the true
// delta was -0.015.
theo::OneSided one_sided_greeks(OptD g_above);

// A Kalshi market's Greek, from a per-boundary Greek function.
//
// Same signed-term arithmetic as theo::bracket_fair over the same rule table, and deliberately
// NOT a call into it: bracket_fair ends in clamp(total), which is right for a probability and
// destroys a Greek. On a real board that clamp turned a `between` delta of -0.004156 into
// +0.000000 -- a market reading as risk-free at the moment it was most exposed. NO clamp here at
// all: not [0,1], not [-1,1]. What keeps it finite is the variance-time floor.
OptD bracket_greek(const std::string& strike_type, OptD floor_strike, OptD cap_strike,
                   const std::function<OptD(double)>& g_at);

struct MarketGreeks {
    OptD delta, vega, theta, R, w, var_left, sigma, forward;
    std::string reason;
};

// The three dollar Greeks for one Kalshi market, plus the inputs they were built from.
//
// R uses ABSOLUTE values because the sign of a Greek is direction, not risk.
MarketGreeks market_greeks(const std::string& strike_type, OptD floor_strike, OptD cap_strike,
                           OptD F, OptD sigma_annualised, const kp::time::Instant& now,
                           const kp::time::Instant& settlement,
                           const varcal::VarianceCalendar& cal);

}  // namespace kp::pricing::greeks
