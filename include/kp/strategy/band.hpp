// Which markets are near enough the money to be worth quoting in depth.
//
// Port of Kalshi/strategy/band.py.
//
// The band decides the SHAPE of a quote -- three rungs a side or one -- and, with it, the
// market's inventory limit. It does NOT decide whether a market is quoted at all; that is
// budget::select_markets, which rations capital, and edgemgr, which refuses a market with no R.
//
// WHY SIGMA AND NOT STRIKES. Measured on the two SPX boards, 2026-08-21 11:49 ET (F = 7691.1,
// w to 16:00 = 0.00244, so one sigma is 18.8 index points):
//
//     KXINX  4pm brackets, 25 wide  ->  +-3 strikes = +-75 pts = +-3.99 sigma
//     KXINXU hourly rungs,   5 wide  ->  +-3 strikes = +-15 pts = +-0.80 sigma
//
// A strike count means five different things on two boards of the same underlying. Sigma also
// self-adjusts to the horizon for free, because w is the total vol to THAT market's own
// settlement -- an hourly rung ten minutes from close gets almost no band, which is correct: it
// has no optionality left anywhere but at the money.
//
// Pure -- no network, no clock, no state.
#pragma once
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "kp/pricing/theo_core.hpp"

namespace kp::strategy::band {

using OptD = std::optional<double>;
inline constexpr std::nullopt_t MISSING = std::nullopt;

// A `between` market has two boundaries and is not a point on the moneyness axis. Its distance
// is measured from the MIDPOINT of the bracket, which is where its digital is centred and where
// its Greeks peak -- the same place bracket_fair differences its two terms.
inline constexpr const char* BRACKET_TYPE = "between";

// The single level this market's moneyness is measured from, or MISSING if unpriceable.
//
// floor_strike for a `greater*` market, cap_strike for a `less*` one -- taken from
// theo::strike_type_rule rather than restated, so a new strike type cannot be handled here and
// not there.
OptD market_strike(const kp::pricing::theo::Market& m);

// Half-width of the band in index points: sigma * w * F. MISSING when it cannot be computed.
//
// w is TOTAL vol (sigma * sqrt(T)), the unit everything in this repo is parameterised by, so
// w * F is one standard deviation of the terminal level in points with no T appearing anywhere.
OptD band_width(OptD forward, OptD w, OptD sigma = MISSING);

// |K - F| in index points, or MISSING.
OptD distance(const kp::pricing::theo::Market& m, OptD forward);

struct BandDetail {
    std::string reason;             // empty = Python None
    OptD distance, half_width, limit, sigmas, band_frac;
    bool incumbent = false;
    bool has_detail = false;        // false on the refusal path, where only the 3 fields above set
};

// Does this market get three rungs?
//
// `incumbent` is whether it is ALREADY quoted in depth; an incumbent is held to the wider exit
// boundary, so the band is sticky and does not churn a ladder on every wobble of the forward.
//
// FAILS CLOSED: no strike, no forward or no w means one rung, not three. Depth is the expensive
// choice and an unknown does not earn it.
std::pair<bool, BandDetail> in_band(const kp::pricing::theo::Market& m, OptD forward, OptD w,
                                    OptD sigma = MISSING, bool incumbent = false,
                                    OptD exit_mult = MISSING);

// The rung sizes for a market: the full ladder in-band, its first rung outside.
//
// Slicing the SAME tuple rather than naming a second one is deliberate -- a wing rung must be the
// same size as a band rung, because the lot is the unit the inventory limits are counted in.
std::vector<std::string> counts_for(bool inband,
                                    const std::vector<std::string>* counts = nullptr);

// Net contracts we are willing to hold in this market, as a positive bound.
double position_limit(bool inband, std::optional<int> lot = std::nullopt);

struct Classified {
    bool inband = false;
    std::vector<std::string> counts;
    double position_limit = 0.0;
    BandDetail detail;
};

struct BandSummary { long n = 0, band = 0, wing = 0; double sigma = 0.0, exit_mult = 0.0; };

// Whole board. `w` is read per market from `em_w` -- edgemgr::refresh already computes it through
// the variance calendar for each market's own settlement, so nothing is re-priced here.
std::pair<std::map<std::string, Classified>, BandSummary> classify(
    const std::vector<kp::pricing::theo::Market>& markets, OptD forward,
    const std::map<std::string, OptD>& em_w,
    const std::set<std::string>& incumbents = {}, OptD sigma = MISSING);

// The rung sizes for `ticker`, from either a per-market map or one global tuple.
//
// Every consumer of `counts` takes either shape, so a caller that has not been taught about the
// band keeps working unchanged. One helper rather than three copies of the isinstance check,
// because three copies is how the two halves of a rule drift apart.
std::vector<std::string> resolve_counts(const std::map<std::string, std::vector<std::string>>* per_ticker,
                                        const std::vector<std::string>* global_counts,
                                        const std::string& ticker,
                                        const std::vector<std::string>* dflt = nullptr);

}  // namespace kp::strategy::band
