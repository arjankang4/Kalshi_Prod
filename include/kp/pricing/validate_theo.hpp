// The cycle's pricing entry point: resolve spot once, observe the smile once, price every board.
//
// Port of the pricing half of pricing/validate_theo.py -- `price_markets`, `resolve_and_price`,
// `_resolve_and_smooth` and `resolve_and_price_boards` -- plus hourly.py's `price_markets`. The
// `main()` there is a CLI over a recon capture and is not ported.
//
// What run_04.cycle() reads back from resolve_and_price_boards, and where it lives here:
//     pricers["daily"]   -> PricedBoards::daily     (theo.Pricer, always built)
//     pricers["hourly"]  -> PricedBoards::hourly    (HourlyPricer, only with hourly markets AND a
//                                                    calendar AND a `now`)
//     fairs_b            -> PricedBoards::fairs     {ticker: (fair, detail)}, insertion-ordered:
//                                                    daily markets first, then hourly, a repeated
//                                                    ticker overwriting IN PLACE as dict.update does
//     res                -> PricedBoards::res       spot::SpotResolution
// plus `smoothed`, the curve both pricers were handed (the reference keeps it local).
//
// TWO SEAMS THE REFERENCE DOES NOT HAVE, both clocks:
//   * `index_at_ns` -- the wall-clock instant the index quote's age is measured at (the reference
//     reads datetime.now() inside IndexQuote.age_s). Absent = read the clock, as the reference.
//   * the skew observer -- the reference calls `skew_state.observe(...)` with now=None, i.e. at
//     time.monotonic(). Pass `strategy::skewstate::observer(state, mono_now)`. It is a
//     std::function rather than a SkewState& because kp_pricing must not link kp_strategy.
#pragma once
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "kp/massive/index.hpp"
#include "kp/massive/snapshot.hpp"
#include "kp/pricing/hourly.hpp"
#include "kp/pricing/pricer.hpp"
#include "kp/pricing/skew.hpp"
#include "kp/pricing/spot.hpp"
#include "kp/pricing/theo_core.hpp"
#include "kp/pricing/varcal.hpp"
#include "kp/pycompat/odict.hpp"
#include "kp/time/et.hpp"

namespace kp::pricing::validate_theo {

using OptD = std::optional<double>;

// skew_state.observe(quotes, strikes, forward, w_atm) -> the served curve.
using SkewObserver = std::function<skew::SkewFit(const skew::Board&, const std::vector<double>&,
                                                 OptD forward, OptD w_atm)>;

// One entry of the merged {ticker: (fair, detail)}. The daily pricer yields a BracketDetail, the
// hourly one an HourlyDetail (which carries the same BracketDetail inside it). run_04 reads only
// `fair` and `bracket().reason`.
struct BoardFair {
    bool hourly = false;
    OptD fair;
    theo::BracketDetail daily;              // meaningful when !hourly
    hourly::HourlyDetail hour;              // meaningful when hourly
    const theo::BracketDetail& bracket() const { return hourly ? hour.bracket : daily; }
};

using Fairs = kp::pycompat::ODict<std::string, BoardFair>;

// (daily_pricer, spot_resolution, smoothed_curve): the part every board shares.
struct Resolved {
    pricer::Pricer pricer;
    spot::SpotResolution res;
    std::optional<skew::SkewFit> smoothed;   // only when a real (ok) curve was installed
};

Resolved resolve_and_smooth(const kp::massive::OptionSnapshot& snapshot, const std::string& root,
                            const std::string& expiry, const std::vector<double>& strikes,
                            const std::optional<kp::massive::index::IndexQuote>& index_quote,
                            const SkewObserver* skew_state = nullptr,
                            std::optional<long long> index_at_ns = std::nullopt);

struct PricedBoards {
    pricer::Pricer daily;
    std::optional<hourly::HourlyPricer> hourly;
    Fairs fairs;
    spot::SpotResolution res;
    std::optional<skew::SkewFit> smoothed;
};

// Price a 4pm bracket board AND any number of hourly ladders in one pass. ONE HourlyPricer covers
// every hour. The daily pricer takes the RESOLVED spot; the hourly one takes atmvol's parity
// forward with the index level as its fallback only. `calendar` must outlive the result (the
// HourlyPricer holds a pointer to it).
PricedBoards resolve_and_price_boards(
    const std::vector<theo::Market>& daily_markets, const std::vector<theo::Market>& hourly_markets,
    const kp::massive::OptionSnapshot& snapshot, const std::string& root, const std::string& expiry,
    const std::vector<double>& strikes, std::optional<kp::time::Instant> now,
    const varcal::VarianceCalendar* calendar,
    const std::optional<kp::massive::index::IndexQuote>& index_quote,
    const SkewObserver* skew_state = nullptr, std::optional<long long> index_at_ns = std::nullopt);

// resolve_and_price: (pricer, fairs, resolution) for one daily board.
struct Priced {
    pricer::Pricer pricer;
    Fairs fairs;
    spot::SpotResolution res;
};
Priced resolve_and_price(const std::vector<theo::Market>& markets,
                         const kp::massive::OptionSnapshot& snapshot, const std::string& root,
                         const std::string& expiry, const std::vector<double>& strikes,
                         const std::optional<kp::massive::index::IndexQuote>& index_quote,
                         const SkewObserver* skew_state = nullptr,
                         std::optional<long long> index_at_ns = std::nullopt);

// validate_theo.price_markets: no resolution, spot as given (absent -> parity).
std::pair<pricer::Pricer, Fairs> price_markets(const std::vector<theo::Market>& markets,
                                               const kp::massive::OptionSnapshot& snapshot,
                                               const std::string& root, const std::string& expiry,
                                               const std::vector<double>& strikes,
                                               OptD spot = std::nullopt);

// hourly.price_markets: the hourly mirror of the above, over an already-filtered board.
std::pair<hourly::HourlyPricer, Fairs> hourly_price_markets(
    const std::vector<theo::Market>& markets, const skew::Board& quotes,
    const std::vector<double>& strikes, const kp::time::Instant& now,
    const varcal::VarianceCalendar& calendar, OptD index_forward = std::nullopt);

}  // namespace kp::pricing::validate_theo
