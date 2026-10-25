// Prices Kalshi's HOURLY index markets, which have no option expiring when they settle.
//
// Port of Kalshi/pricing/hourly.py.
//
// The daily board is priced off the option board itself (kp/pricing/pricer.hpp): fit a smile,
// take P(S > K) = -dC/dK of it. That only answers the question the option board is asked -- what
// the index does by 16:00 ET -- because 16:00 is the only instant a listed SPXW/NDXP contract
// expires at. The hourly boards (KXINXU, KXNASDAQ100U, events H1000..H1600) settle at
// 10:00..15:00, where nothing expires.
//
// So the hourly path is the daily path plus a distribution:
//
//     atmvol   quotes  -> forward F and total vol w_4pm to 16:00        (measured)
//     varcal   now, h  -> what fraction of the remaining variance lands by h   (fitted)
//     bs       F, K, w -> P(S > K) = N(d2)                              (assumed lognormal)
//
// and then theo::bracket_fair does the Kalshi bracket arithmetic, unchanged. That last point is
// the design: bracket_fair takes a p_above(K) callable and nothing else, so every strike_type
// rule, the boundary normalisation that collapses 7574.9999 into 7575, and the null-reason
// taxonomy are shared with the live daily pricer rather than reimplemented next to it.
//
// DORMANT, NOT DEAD. `config::QUOTE_HOURLY_SERIES` is empty today, so nothing live reaches this
// -- but that is a config toggle, not a deletion, and validate_theo.resolve_and_price_boards
// wires it in unconditionally when the tuple is non-empty. Omitting it from the port would break
// on restore rather than at build time, which is the worst possible moment.
//
// WHAT IS ASSUMED, stated plainly (carried across from the reference docstring):
//   * Zero skew is NOT assumed -- see p_above_for. But the curve is the 16:00 curve.
//   * Flat vol across the day. The 4pm number is stretched to a sub-hour horizon by the variance
//     calendar, which knows the average shape of a session but nothing about *today*. On an FOMC
//     or CPI day the 14:00 hour needs more variance left in it than the average day allocates.
//   * The parity forward is the forward to 16:00, not to h. Intraday dividends make those differ
//     by <0.02% except around quarterly ex-dates. Not modelled.
#pragma once
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "kp/pricing/atmvol.hpp"
#include "kp/pricing/skew.hpp"
#include "kp/pricing/theo_core.hpp"
#include "kp/pricing/varcal.hpp"
#include "kp/time/et.hpp"

namespace kp::pricing::hourly {

using OptD = std::optional<double>;
inline constexpr std::nullopt_t MISSING = std::nullopt;
using Board = skew::Board;

// Below this much variance left to 16:00, the scaling ratio's denominator is noise and every
// hourly market left is settling within seconds anyway.
inline constexpr double MIN_VARIANCE_LEFT = 1e-5;

// Presentation only. One intraday session counts as one "variance day", so a year is 252 of them.
// Nothing in the pricing path divides or multiplies by this -- the model works in total vol and
// scales by a ratio, in which the annualisation cancels. It exists so the fitted vol can be read
// in the units a trader thinks in, and so a wrong-looking number is obvious at a glance.
inline constexpr int TRADING_DAYS_PER_YEAR = 252;

// The 16:00 ET close, as (hour, minute).
inline constexpr int CLOSE_HOUR = 16;
inline constexpr int CLOSE_MINUTE = 0;

// The instant a Kalshi market settles against.
//
// **close_time, not expiration_time.** On KXINXU-26AUG20H1500-T7819.9999 the settlement instant
// is close_time = 2026-08-20T19:00:00Z (3pm ET), while expiration_time is 2026-08-27T22:00:00Z --
// a week later, when the contract is administratively expired. Using the latter as a time to
// expiry would price an hourly market as an eight-day option.
//
// The Python parses the ISO string here; C++ parses it once at the JSON boundary into
// theo::Market::close_time_s, so this is the trivial half of the reference function. The
// unparseable-string branch maps onto an absent close_time_s and is indistinguishable from a
// missing key -- which is what the Python does too, since both return None.
std::optional<kp::time::Instant> parse_settlement(const theo::Market& m);

// 16:00 ET on the ET date of `now` -- when the 0DTE options being read actually expire.
kp::time::Instant expiry_4pm(const kp::time::Instant& now);

// Python's datetime.isoformat() on a UTC-aware instant: "2026-08-20T19:00:00+00:00".
//
// Kalshi spells every close_time with a trailing "Z", which fromisoformat turns into a +00:00
// offset, which isoformat then renders back as "+00:00" and never as "Z". Whole seconds on every
// close_time observed, so no fractional part is ever emitted.
std::string isoformat_utc(const kp::time::Instant& t);

// (w, ratio, reason) for one settlement instant. Empty reason means priced.
struct HorizonVol {
    OptD w, ratio;
    std::string reason;
};

// The full record the journal reads, mirroring the reference's detail dict.
//
// The bracket half is theo::BracketDetail, byte-identical to what the daily pricer produces; the
// hourly extras hang off it. Only `fair` and `bracket.reason` reach the order path
// (run_04_quote_loop:169,180) -- everything else here is journal provenance.
struct HourlyDetail {
    theo::BracketDetail bracket;
    OptD spot, forward, w, w_4pm, var_ratio;
    std::optional<kp::time::Instant> settlement;
    OptD minutes_to_settlement, sigma_annualized, variance_left_to_4pm;
    std::string atm_reason, forward_source;
    OptD parity_forward, index_minus_parity, cp_spread, vol_dispersion;
};

// Prices any strike_type on any settlement instant up to today's 16:00.
//
// Construct one per quote snapshot; it holds no state across time. ONE INSTANCE COVERS EVERY
// HOUR -- market_fair reads each market's own close_time and rescales the 16:00 total vol through
// the variance calendar, so seven open events need one instance, not seven.
class HourlyPricer {
public:
    HourlyPricer(Board quotes, std::vector<double> strikes, kp::time::Instant now,
                 const varcal::VarianceCalendar& calendar,
                 std::optional<atmvol::AtmFit> fit = std::nullopt,
                 OptD index_forward = MISSING,
                 std::optional<skew::SkewFit> skew_fit = std::nullopt);

    // Install a curve fitted elsewhere, after construction. Mirrors Pricer::use_skew.
    //
    // The live loop reuses ONE strategy::skewstate::SkewState across cycles and across boards --
    // the smile is a property of the SPXW option board, not of the Kalshi board being priced, so
    // the 4pm brackets and every hourly ladder read the same curve. Without this setter the
    // hourly pricer could only take a curve at construction, and the smoothed one does not exist
    // until spot has been resolved.
    //
    // The curve is fitted against the 16:00 forward and 16:00 ATM vol -- see skew(). The hourly
    // rescaling happens in total_vol_to, not in the curve.
    HourlyPricer& use_skew(const skew::SkewFit& fit);

    // The fitted vol curve for this board, built once on first use.
    //
    // Fitted against the 16:00 forward and 16:00 ATM vol, because that is the expiry the listed
    // options actually have -- there is no separate option board at 15:00 to read a smile from.
    const skew::SkewFit* skew() const;
    // The curve as it stands -- installed, or already lazily built -- WITHOUT building one (the
    // reference's `_skew`, not the `.skew` property). For fastcancel.reprice_shifted.
    skew::SkewFit* installed_skew() { return skew_ ? &*skew_ : nullptr; }

    // Pricer calls this `spot`, and with r = q = 0 at 0DTE the forward is the spot. Same name so
    // the two are interchangeable at the seam.
    OptD spot() const { return fit_.forward; }
    // strategy/fastcancel.reprice_shifted moves the forward by a delta and restores it (the
    // reference assigns `pricer.fit.forward`). Caches keyed on the settlement are unaffected.
    void set_forward(OptD f) { fit_.forward = f; }
    OptD w_4pm() const { return fit_.w; }
    const atmvol::AtmFit& fit() const { return fit_; }
    const std::vector<double>& strikes() const { return strikes_; }
    const kp::time::Instant& now() const { return now_; }
    const kp::time::Instant& expiry_4pm_at() const { return expiry_4pm_; }

    // The fitted ATM vol as an annualised sigma, on the variance clock. Diagnostic only.
    //
    // w_4pm is the total stdev from now to 16:00, which is `1 - phi(now)` variance-days of
    // exposure, so sigma = w / sqrt((1 - phi(now)) / 252).
    //
    // Reading it on the VARIANCE clock rather than the clock clock matters: at 10:15 there are
    // 5h45m left of a 6.5h session (88% by the clock) but only 78% of the variance. Annualising
    // on wall time would report a sigma ~6% below the one the model is actually using, and the
    // two would silently disagree.
    OptD sigma_annualized() const;

    // (w, ratio, reason) for a settlement instant. Cached per instant, absence included --
    // the reference caches under the None key too.
    const HorizonVol& total_vol_to(const std::optional<kp::time::Instant>& settlement) const;

    // `p_above(K) -> P(S_settlement > K) | MISSING`, the callable bracket_fair consumes.
    //
    // The vol is per-strike, not one scalar: w(K) = w_atm(settlement) * ratio(K), where the ratio
    // comes off the skew curve fitted to the 16:00 board.
    //
    // **The ratio is read at the strike's position on the board where it was OBSERVED**, i.e. in
    // 16:00 sigma units, and then applied to the horizon's ATM vol. The same strike sits at more
    // sigmas when less variance remains, so evaluating the curve at the horizon's own x instead
    // would be a sticky-delta-across-horizons assumption -- plausible, untested here, and it
    // would make a 15:00 market read its skew off a part of the curve no quote was taken from.
    //
    // DO NOT "IMPROVE" THIS. docs/pricing-invariants.md §5: reading the ratio in 16:00 sigma
    // units is *why* the hourly boards applied a flatter smile (ratio IQR 0.09 vs 0.22) and
    // therefore never carried the missing-slope-term error -- w_ours/w_mkt was 1.00 on H1000-H1500
    // against 1.11 on H1600 at matched time to settlement. Evaluating at the horizon's own x
    // reads like an obvious improvement and would import the bug onto the one board that never
    // had it.
    std::function<OptD(double)> p_above_for(const std::optional<kp::time::Instant>& settlement) const;

    // Fair value for one Kalshi market. Same (fair, detail) contract as Pricer::market_fair.
    std::pair<OptD, HourlyDetail> market_fair(const theo::Market& m) const;

private:
    Board quotes_;
    std::vector<double> strikes_;
    kp::time::Instant now_;
    const varcal::VarianceCalendar* calendar_;
    atmvol::AtmFit fit_;
    kp::time::Instant expiry_4pm_;
    mutable std::map<std::optional<std::int64_t>, HorizonVol> w_cache_;
    mutable std::optional<skew::SkewFit> skew_;
    mutable bool skew_built_ = false;
};

}  // namespace kp::pricing::hourly
