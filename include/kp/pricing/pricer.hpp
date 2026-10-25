// Prices digitals off one snapshot of option quotes.
//
// Port of the `Pricer` class in Kalshi/pricing/theo.py. It lives in its own header
// rather than in theo_core because it depends on skew, and skew depends on theo_core's Quote and
// microprice -- keeping the class here is what breaks that cycle. The reference has no such
// constraint (Python resolves imports at module level) and keeps both in one file.
//
// NO TIME-TO-EXPIRY APPEARS ANYWHERE. `bs` is parameterised by total vol w = sigma*sqrt(T), and
// the options being read expire at exactly the instant the market settles, so the w they imply IS
// the w the digital needs. That identity is why this class is valid ONLY on the 16:00 board;
// pricing/hourly handles every other settlement.
#pragma once
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "kp/pricing/skew.hpp"
#include "kp/pricing/theo_core.hpp"

namespace kp::pricing::pricer {

using OptD = std::optional<double>;
inline constexpr std::nullopt_t MISSING = std::nullopt;

// Half-width, in index points, of the near-money window parity medians over.
inline constexpr double PARITY_WIDTH = 50.0;

// Everything behind one strike's implied vol, for the journal. Every field always present.
struct ImpliedDetail {
    OptD w_call, w_put, w;
    char w_leg = '\0';        // 'C' | 'P' | '\0' for Python None
};

class Pricer {
public:
    // `spot` absent means "derive it from put-call parity", exactly as the Python's `spot=None`.
    // The live loop passes the resolved spot (index when fresh), which skips parity_spot entirely.
    Pricer(skew::Board quotes, std::vector<double> strikes,
           OptD spot = MISSING, std::optional<skew::SkewFit> skew_fit = std::nullopt);

    // Install a curve fitted elsewhere, after construction.
    //
    // The live loop reuses one SkewState across cycles, but the curve's x-axis is measured from
    // the forward, so it cannot be fitted until the spot is resolved -- which happens after this
    // Pricer exists. Hence a setter rather than a constructor argument on that path.
    Pricer& use_skew(const skew::SkewFit& fit);

    OptD spot() const { return spot_; }
    // strategy/fastcancel.reprice_shifted moves the forward by a delta and restores it (the
    // reference assigns `pricer.spot`). A lazily built skew fit is NOT rebuilt -- as there.
    void set_spot(OptD s) { spot_ = s; }
    const std::vector<double>& strikes() const { return strikes_; }

    // The fitted vol curve, built once on first use. Lazy because parity_spot has to run first.
    // The live quote loop passes a curve in, so there it refits at SKEW_RECOMPUTE_S rather than
    // every cycle and prices off the smoothed curve.
    const skew::SkewFit* skew() const;
    // The curve as it stands -- installed, or already lazily built -- WITHOUT building one (the
    // reference's `_skew`, not the `.skew` property). For fastcancel.reprice_shifted.
    skew::SkewFit* installed_skew() { return skew_ ? &*skew_ : nullptr; }

    OptD val(char right, double strike) const;

    // S = K + C - P at 0DTE (discounting ~ 0). Median over near-money strikes, anchored at the
    // strike where |C - P| is smallest.
    //
    // `strict` IS THE WHOLE SAFETY PROPERTY and defaults on. Both legs at a strike must be
    // genuinely two-sided. Without it a deep-ITM call with no bid prices to exactly 0.00 -- absent
    // means zero, not unknown -- so |C - P| collapses to the tiny OTM premium, that strike wins
    // the anchor, and the result is S = K, its own strike. Measured 2026-08-19: spot 7387.5 on a
    // day whose true spot was ~7706. Bracketing strikes span about +-4.6% of spot, so a wrong
    // anchor is not a wing effect -- it reprices the entire board off a forward hundreds of
    // points out.
    //
    // Returning MISSING when no strike is two-sided fails SAFE: spot::resolve then reports
    // `..._+no_parity` with no value, and the loop skips the cycle rather than quoting off a
    // fabricated spot.
    OptD parity_spot(double width = PARITY_WIDTH, bool strict = true) const;

    // Total vol implied by the options AT `strike`. ONLY THE OUT-OF-THE-MONEY LEG IS USED.
    //
    // An ITM option is almost all intrinsic, so its time value is a small difference of two large
    // numbers and inherits the full width of its quote. Measured on the 2026-08-19 board at spot
    // 7704.30: the 7975 put was quoted 259.5 / 280.0 -- 20.5 points wide -- giving a microprice of
    // 270.93 against an intrinsic of 270.70, i.e. 0.23 of "time value" that is entirely quote
    // noise. Inverting that yields w = 0.0139 against an ATM w of 0.0024, and a far-OTM digital
    // priced off it came out at 0.6% instead of ~0.
    //
    // Vega weighting cannot substitute: put-call parity makes vega IDENTICAL for the call and the
    // put at a strike, so a vega-weighted mean of the two legs is just their plain average and
    // cannot discriminate between them at all. An earlier version did exactly that and was a no-op.
    //
    // There is no fallback to the ITM leg. If the OTM option cannot identify a vol, the honest
    // answer is that this strike does not identify one -- the ITM leg would supply a number built
    // from quote width, which is worse than no number.
    std::pair<OptD, ImpliedDetail> implied_w_at(double strike) const;

    // P(final index > strike) from the option price LEVEL at `strike`, via Black-Scholes.
    //
    // This is N(d2), not the delta N(d1). Off-grid boundaries -- KXINXDUD's 7691.76, NDX
    // boundaries off the 10-wide grid -- interpolate w between the bracketing strikes IN
    // TOTAL-VOL SPACE. Vol is far smoother across strikes than price is, so interpolating price
    // would import the curvature the whole smile has.
    OptD digital_above_delta(double strike) const;

    // P(final index > strike). The only estimator: Black-Scholes on the skew curve.
    //
    // N(d2) at a vol read off the fitted smile -- w(K) = w_atm * ratio(K) -- rather than at one
    // flat ATM vol. When the curve refuses to fit, ratio is 1.0 everywhere and this degrades
    // exactly to the flat-vol model.
    OptD digital_above(double strike) const;

    // Fair value for one Kalshi market. A thin adapter: all bracket arithmetic lives in the pure
    // theo::bracket_fair so the backtest can share it rather than maintain a parallel copy.
    std::pair<OptD, theo::BracketDetail> market_fair(const theo::Market& m) const;

private:
    skew::Board quotes_;
    std::vector<double> strikes_;
    std::set<double> strike_set_;
    OptD spot_;
    mutable std::optional<skew::SkewFit> skew_;
    mutable bool skew_built_ = false;
};

}  // namespace kp::pricing::pricer
