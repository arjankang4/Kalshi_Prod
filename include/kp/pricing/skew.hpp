// The volatility skew curve: per-strike implied vols -> a natural cubic spline in sigma-space.
//
// Port of Kalshi/pricing/skew.py.
//
// A refusal is a SkewFit with flat=true and a reason -- never an exception, and never a
// half-fitted curve. ratio() then returns 1.0 everywhere, so the caller gets the flat-vol model
// it would have had anyway.
#pragma once
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "kp/pricing/theo_core.hpp"

namespace kp::pricing::skew {

using OptD = std::optional<double>;
inline constexpr std::nullopt_t MISSING = std::nullopt;

// Knots at +-BUCKET_MAX sigma in BUCKET_STEP-sigma steps. Far from the money an index option's
// quote is mostly tick noise on a price that carries almost no vega, so a knot out there would fit
// the spread rather than the smile.
inline constexpr double BUCKET_STEP = 0.25;
inline constexpr double BUCKET_MAX  = 2.5;

// Below this many populated buckets the spline is fitting noise; refuse and use flat vol.
inline constexpr int MIN_BUCKETS = 4;

// A fitted ratio outside this band is not skew, it is a bad quote that survived inversion.
inline constexpr double RATIO_MIN = 0.2;
inline constexpr double RATIO_MAX = 5.0;

// The differencing half-width, in sigmas, used to read dw/dK off the fitted ratio curve. It is
// well inside the spacing of the spline's own knots, so it reads the curve's slope rather than the
// noise between knots.
inline constexpr double SLOPE_STEP_X = 0.04;

// The projection only has to cover the strikes where the digital is not already saturated: beyond
// 5 sigma N(d2) is inside 3e-7 of 0 or 1 and the slope term is numerically zero, so the ladder is
// monotone there whatever the curve does. Every violation measured on the 1s board sits inside
// |x| = 3.1. Bounding the window is worth 2.2x on the whole fit-plus-board path.
inline constexpr double PROJECTION_XMAX = 5.0;

// L2 projection onto the non-increasing cone (pool-adjacent-violators). O(n), no scipy.
std::vector<double> pava_nonincreasing(const std::vector<double>& y);

// Linear interpolation on a sorted grid, flat outside it.
double interp(double x, const std::vector<double>& xs, const std::vector<double>& ys);

// Smallest width, in total-vol units, that a leg is credited with. Only bites on a zero-wide
// (synthetic or touching) quote, where it keeps the weight finite instead of infinite.
inline constexpr double MIN_WIDTH_VOL = 1e-9;

// A reference total-vol used only to turn a price width into a vol width when no ATM vol is
// known yet. It cancels out of the leg WEIGHTS (both legs share it), so its level only matters
// for the vega>0 guard -- any plausible 0DTE number does.
inline constexpr double W_PROBE = 0.005;

// The nominal knot positions, in sigmas: -3.0, -2.7, ... 0.0 ... +2.7, +3.0.
std::vector<double> bucket_offsets(double step = BUCKET_STEP, double xmax = BUCKET_MAX);

// Stable name for a bucket. The label, not the strike, is the identity a live queue keys on:
// spot moves during a session, so the strike sitting in the "+0.3 sigma" bucket changes -- and
// when it does we still want the new observation appended to the +0.3 series.
std::string bucket_label(double x);

// The canonical board: ('C'|'P', strike) -> Q frame.
struct LegKey {
    char right;
    double strike;
    bool operator<(const LegKey& o) const {
        if (right != o.right) return right < o.right;
        return strike < o.strike;
    }
};
using Board = std::map<LegKey, theo::Quote>;

struct LegVol { OptD w; double weight = 0.0; };

// (implied w, weight) for one leg. Requires BOTH sides quoted: theo::quote_value deliberately
// returns the bid alone for a one-sided quote, and such a quote has no width, so it would enter
// the average carrying a confidence it has not earned. Zero width is fine; absent is not.
LegVol leg_vol(double F, double K, char right, const theo::Quote& q, double w_ref);

struct StrikeVolDetail { OptD w_call, w_put, w; double wt_call = 0.0, wt_put = 0.0; };

// Total vol at one strike, from both legs weighted by inverse width-in-vols. MISSING when
// neither leg inverts.
std::pair<OptD, StrikeVolDetail> strike_vol(const Board& quotes, double F, double K, double w_ref);

// --- natural cubic spline ------------------------------------------------------------------
//
// Hand-rolled rather than scipy: every module under pricing/ imports stdlib and config only, and
// this has to run in the seconds before a quote cycle. A natural cubic spline over <=21 knots is
// a tridiagonal solve -- the Thomas algorithm is O(n) and exact.
//
// Flat extrapolation outside the knots, not linear or cubic: past the last knot a cubic diverges
// and can drive the vol ratio negative, and these are precisely the far wings whose quotes are
// least trustworthy. Holding the endpoint ratio says "we stop learning out here", which is true.
class Spline {
public:
    Spline() = default;
    Spline(std::vector<double> x, std::vector<double> y);
    OptD operator()(double xq) const;
    const std::vector<double>& m() const { return m_; }   // the solved second-derivative array
private:
    std::vector<double> x_, y_, m_;
};

struct Bucket {
    std::string label;
    double x_nominal, x, strike, w, ratio;
    StrikeVolDetail detail;
};

class SkewFit {
public:
    // `strikes` is the listed grid the curve was fitted against. Needed only to project the
    // digital ladder back onto the monotone cone -- see projection(). A fit built without it (the
    // live SkewState EMA path) still gets the slope term, just not the projection; measured, the
    // projection is worth <0.001pp inside 2 sigma, so that degradation is nominal.
    SkewFit(OptD forward, OptD w_atm, std::optional<Spline> spline,
            std::vector<Bucket> buckets, std::string reason,
            const std::vector<double>& strikes = {});

    bool ok() const { return reason_.empty() && spline_.has_value(); }
    bool flat() const { return !spline_.has_value(); }
    const std::string& reason() const { return reason_; }
    std::size_t n_buckets() const { return buckets_.size(); }
    const std::vector<Bucket>& buckets() const { return buckets_; }
    OptD forward() const { return forward_; }

    // For fastcancel.reprice_shifted ONLY: the curve carries its own forward, and a re-price at a
    // moved spot has to move it too (the reference assigns `sf.forward`). The projection memo is
    // keyed on vol alone but was computed at a forward, so it is swapped out for the duration and
    // put back after.
    using ProjMemo = std::map<double, std::pair<std::vector<double>, std::vector<double>>>;
    void set_forward(OptD f) { forward_ = f; }
    ProjMemo take_proj() { ProjMemo m; m.swap(proj_); return m; }
    void put_proj(ProjMemo m) { proj_.swap(m); }
    OptD w_atm() const { return w_atm_; }

    OptD x_of(double K) const;
    // w(K) / w_atm. Exactly 1.0 when the fit refused, which is the flat-vol fallback.
    double ratio(double K) const;
    OptD w_of(double K) const;

    // ---- the digital -------------------------------------------------------------------
    //
    // A binary is the strike-DERIVATIVE of the call curve, and with a strike-dependent vol the
    // call depends on K twice:
    //
    //     C(K)   = BS(F, K, w(K))
    //     -dC/dK = N(d2)  -  F * phi(d1) * dw/dK
    //
    // THIS PORT SHIPPED THE FIRST TERM ONLY until the 2026-08-26 re-baseline, which is what the
    // reference itself did until 2026-08-24 and what notebooks/05 §7 measures as wrong by up to
    // +4.0 probability points AT THE MONEY. It also produced a false finding that survived three
    // days and two notebooks -- "our vol runs 5-13% above the Kalshi board" -- which evaporated
    // once the digital became the derivative. See docs/pricing-invariants.md §1-§3.
    //
    // The cheapest discriminator, if this is ever in doubt again: a vol-level error must VANISH
    // at the money. At |x| < 0.15 a 7% relative vol error moves the digital 0.21pp; the error
    // actually measured there was -4.06pp. A 19x gap settles it in one line.

    // d(ratio)/dK of the fitted curve at K, differenced step_x sigmas either side. Zero for a
    // refused fit, which is right: a flat curve has no slope and the term vanishes, so the
    // corrected digital degrades to exactly the old one wherever the smile is off.
    double dratio_dK(double K, double step_x = SLOPE_STEP_X) const;

    // `- F * phi(d1) * dw/dK`, in probability units, at ATM vol `w_atm`.
    //
    // `w_atm` absent defaults to the fit's own. The HOURLY board passes its horizon vol instead:
    // the ratio curve is read where it was observed (16:00 sigmas) but the vol it multiplies is
    // the horizon's, so dw/dK = w_horizon * dratio/dK and the term scales with it.
    double slope_term(double K, OptD w_atm = std::nullopt) const;

    // N(d2) at the smile's vol for K. The pre-2026-08-24 digital. Kept for the guard and for A/B:
    // ladder_monotone deliberately still runs on THIS, so which boards accept a fit is unchanged
    // by the correction and old and new runs share one population.
    OptD p_above_flatvol(double K) const;

    // P(S > K) = -dC/dK on the fitted curve, at ATM vol `w_atm` (absent: the fit's own).
    OptD p_above_at(double K, OptD w_atm = std::nullopt) const;
    OptD p_above(double K) const;

    // (strikes, adjustment) -- `projected - raw` on the listed ladder, which is ~always zero.
    //
    // A cubic spline through noisy bucket ratios is not butterfly-arbitrage-free, and the old
    // digital could not see that because it never used the slope. The corrected one can: the raw
    // corrected ladder is non-monotone on 98.8% of 1s boards, by a mean worst step of 0.47pp
    // (SPX) / 0.90pp (NDX) on ~3 and ~8 rungs of ~450, essentially all beyond |x| = 2.7.
    // Projecting onto the non-increasing cone repairs it and removes every negative rung.
    // Measured displacement: mean 0.003pp / 0.014pp over the whole ladder, and inside 2 sigma
    // 0.0000pp / 0.022pp. Carried as an ADJUSTMENT rather than a replacement so off-grid
    // boundaries keep the exact analytic value wherever the projection does not bind.
    //
    // THIS IS A REPAIR, NOT A FIX. The right fix is a smile that cannot admit butterfly
    // arbitrage in the first place. Until then the corrected digital's WINGS are not
    // trustworthy; inside +-2 sigma it is.
    struct Projection { const std::vector<double>* ks; const std::vector<double>* adj; };
    Projection projection(double wa) const;

private:
    OptD forward_, w_atm_;
    std::optional<Spline> spline_;
    std::vector<Bucket> buckets_;
    std::string reason_;
    std::vector<double> strikes_;
    // Memo keyed on round(wa, 12), matching the Python dict. Mutable because projection() is
    // logically const -- it is a pure function of (curve, wa) that happens to cache.
    mutable std::map<double, std::pair<std::vector<double>, std::vector<double>>> proj_;
};

// Total vol at the money, for callers that do not already have an atmvol::AtmFit.
//
// Walks outward from the nearest listed strike so a single uninvertible ATM quote does not sink
// the whole fit. NOTE: the daily pricer must pass its OWN spot here rather than atmvol's parity
// forward -- mixing two forwards into one curve makes the ratio absorb their difference and call
// it skew.
OptD atm_vol(const Board& quotes, const std::vector<double>& strikes, OptD F, int max_steps = 3);

// Fit the skew curve for one board snapshot. `strikes` is the FULL listed grid, not the
// subscribed subset.
SkewFit fit(const Board& quotes, std::vector<double> strikes, OptD forward, OptD w_atm,
            double step = BUCKET_STEP, double xmax = BUCKET_MAX, int min_buckets = MIN_BUCKETS);

// P(S > K) must be non-increasing in K. A spline through noisy quotes can wobble enough to
// invert the digital ladder, which shows up downstream as a bracket with negative probability --
// bracket_fair would clamp it to zero and the board would silently lose a rung.
bool ladder_monotone(const SkewFit& sf, const std::vector<double>& strikes, double tol = 1e-12);

}  // namespace kp::pricing::skew
