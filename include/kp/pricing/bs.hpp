// Black-Scholes in TOTAL-VOL space, for pricing markets that have no option of their own.
//
// Port of Kalshi/pricing/bs.py. The reasoning below is that module's, kept
// because it is the specification -- the arithmetic alone does not say why any of it is shaped
// this way.
//
// `pricing/theo` reads probabilities model-free off vertical spreads, which is exact but only
// answers the question the option board is asked: what happens at 16:00 ET. The hourly Kalshi
// boards settle at 10:00..15:00, and no listed option expires then. So those need a
// distribution, and this is the smallest one that will do.
//
// Everything here is parameterised by **w = sigma * sqrt(T)**, the standard deviation of the log
// return over the window -- not by an annualised sigma and a separate T. Black-Scholes depends
// on the pair only through their product, so carrying the product:
//   * removes the annualisation round trip (pick a day-count, divide, later multiply back),
//     which is where a factor of sqrt(252/365) hides;
//   * lets the variance calendar scale a 4pm number to a 14:00 number by a pure ratio, with no
//     day-count convention entering at all (see pricing/varcal);
//   * makes the degenerate cases legible: w == 0 is intrinsic, and w is what a bisection solves.
//
// Rates and dividends are zero. These are 0DTE index options: the financing component over a few
// hours is smaller than a tick, and Kalshi settles on the index, not on a total-return series.
// So `F` is both the forward and the spot, and prices are undiscounted.
#pragma once
#include <cmath>
#include <optional>

namespace kp::pricing::bs {

// MISSING is `None` in the Python and is a CONTROL-FLOW token, not a numeric sentinel. It must
// never become NaN: `theo.clamp` is min(1, max(0, x)), which turns NaN into 0.0 and would
// silently price a market the reference declined to price.
using OptD = std::optional<double>;
inline constexpr std::nullopt_t MISSING = std::nullopt;

// bs.py:31-32. sqrt is IEEE-mandated correctly rounded, so a compile-time fold necessarily
// agrees with glibc -- which is NOT true of any other transcendental here (GCC folds with MPFR;
// measured, folded cbrt(27) differs from glibc's by an ulp). Pinned in tests/unit/test_fpenv.
inline const double SQRT2    = std::sqrt(2.0);
inline const double SQRT_2PI = std::sqrt(2.0 * 3.141592653589793);

// Bisection bounds for the implied-vol solve. The upper bound is deliberately absurd (500% total
// stdev): a real 0DTE ATM straddle is w ~ 0.002-0.02, and a bound that can be hit silently
// returns a wrong answer instead of MISSING.
inline constexpr double W_MIN    = 1e-9;
inline constexpr double W_MAX    = 5.0;
inline constexpr double W_TOL    = 1e-12;
inline constexpr int    MAX_ITER = 200;

// Minimum time value, in dollars, for a price to identify a vol at all.
//
// Vega vanishes far from the money, so past a few sigma the price stops responding to w and the
// bisection converges on whatever the last representable price happened to be. Measured: a
// 50-point-ITM call at w = 5e-4 (a 3.9 point stdev, i.e. 13 sigma) round-trips with |dw| = 3e-06,
// 0.6% of w -- not a solver bug, just a question the price cannot answer.
//
// One cent is the natural floor: it is the resolution of the price grid these quotes live on, so
// an option whose entire time value is under a tick carries less than one tick of information
// about vol. Returning MISSING keeps such a quote out of the ATM aggregate instead of letting it
// vote with a fabricated number.
inline constexpr double MIN_TIME_VALUE = 0.01;

double norm_cdf(double x);
double norm_pdf(double x);
double intrinsic(double F, double K, char cp);           // cp is 'C' or 'P'

struct D1D2 { double d1, d2; };
D1D2 d1_d2(double F, double K, double w);

OptD bs_price(double F, double K, double w, char cp);

// d(price)/dw -- sensitivity to TOTAL stdev, so no sqrt(T) factor. Same for calls and puts.
// NOTE: this is the VANILLA vega. pricing/greeks has a different quantity under a similar name
// (the binary's), and the two are deliberately not interchangeable.
double vega_w(double F, double K, double w);

// P(S_T > K) under the risk-neutral lognormal -- this is N(d2), NOT the delta N(d1).
//
// The delta is the hedge ratio; N(d2) is the probability. They differ by roughly phi(0)*w, which
// on a one-sided Kalshi rung is ~0.08c at a quiet hour's vol and ~0.32c in a stressed one. Under
// a 1c tick, but a real fraction of a quote offset -- and N(d2) is no more expensive, so
// there is no reason to carry the bias. On a two-sided bracket the bias very largely cancels
// (<0.06c on a 25-wide range), which is why the daily board could be priced off spreads without
// ever confronting this.
OptD p_above(double F, double K, double w);

// Total stdev implied by an option price, or MISSING if the price implies none.
//
// Bisection rather than Newton: the price is monotone in w with no local structure, 200 halvings
// of [1e-9, 5] is exact to ~1e-12, and it cannot diverge on the near-zero vega a deep-OTM 0DTE
// quote presents.
//
// MISSING (not an exception, and not a clamp to the bound) when the price is at or through a
// no-arbitrage boundary, or when its time value is under `min_time_value`. Those quotes are
// common on a 0DTE board -- a deep-ITM call whose bid is gone prices to intrinsic -- and
// clamping them to W_MIN would feed a zero vol into the aggregate as though it were real.
//
// PORTING NOTE: the loop shape is load-bearing. A 1-ulp difference in bs_price near the root
// flips the `< price` branch at a late iteration and moves the answer. Do not rewrite as Newton,
// do not change the tolerance test, do not hoist the break.
OptD implied_w(double F, double K, OptD price, char cp,
               double lo = W_MIN, double hi = W_MAX, double min_time_value = MIN_TIME_VALUE);

}  // namespace kp::pricing::bs
