// The live edge manager: dollar Greeks per market, refreshed on a slow cadence.
//
// Port of strategy/edgemgr.py. `pricing/edge` is the pure curve; this is
// the stateful half that knows what time it is and caches. Same split as pricing/varcal (pure
// calendar) vs strategy/leanchain (live state).
//
// WHY A SLOW CADENCE. The Greeks move with spot, vol and the clock, none of which change
// materially between quote cycles -- but the board is re-priced every cycle, so recomputing three
// Greeks across ~400 markets on every pass would be most of the loop's work for none of its value.
// config::EDGE_RECOMPUTE_S bounds only the Greek work; the theo and the book stay fast.
//
// NO R, NO QUOTE. This module does not fail SOFT: a market without a usable R is refused rather
// than falling back to config::QUOTE_OFFSET and a flat bar: a quote we cannot risk-price is a
// quote we should not be showing, and "degrade to the old quoter" quietly reintroduces a pricing
// model we have since decided against.
//
// FIVE paths cannot produce an R and all return quotable=false: no calendar, no calibration
// artifact, missing board inputs (forward/sigma), no settlement timestamp, and a per-market R
// that comes back absent. The ONE exception is the operator override (`--edge` off), which
// installs a FlatCurve on purpose and says so loudly at startup.
//
// THE CLOCK IS INJECTED. The reference reads time.monotonic() internally for the cache window;
// here it is a parameter, so the cadence is reproducible under replay and under test.
#pragma once
#include <optional>
#include <string>
#include <vector>

#include "kp/pricing/edge.hpp"
#include "kp/pricing/greeks.hpp"
#include "kp/pricing/theo_core.hpp"
#include "kp/pricing/varcal.hpp"
#include "kp/pycompat/odict.hpp"
#include "kp/time/et.hpp"

namespace kp::strategy::edgemgr {

using OptD = std::optional<double>;
inline constexpr std::nullopt_t MISSING = std::nullopt;

// The instant a market's payoff is fixed, or MISSING.
//
// close_time, NOT expected_expiration_time -- the latter is a data-availability stamp and sits
// close_time + 3h on most daily rows. The reference normalises a string-or-datetime here rather
// than at the call site, because the first version relied on the caller stashing a parsed copy
// and every cycle then died comparing a str to a datetime. C++ parses at the JSON boundary, so
// this reads theo::Market::close_time_s and the normalisation is already done.
std::optional<kp::time::Instant> settlement_of(const kp::pricing::theo::Market& m);

// One market's required edge and risk percentile. Every field is always present in the struct;
// the reference's dict OMITS some keys on some paths, and an omitted key reads as absent to
// every consumer (`band.classify` does `.get("w")`), so absence is modelled as an empty optional.
//
// THE CHARGE IN TWO PARTS (edge.py, 2026-08-26). `edge_micros` stays the total required to POST;
// `floor_micros` and `bonus_micros` are carried beside it so the quote path can build the entry
// bar from floor + lean + bonus + backoff. The adverse gate adds `adverse_dollars`/`move_scale`
// to a record it refuses.
struct EdgeRecord {
    std::optional<long> edge_micros, floor_micros, bonus_micros;
    OptD edge;
    double risk_u = 0.0, scale_pos = 0.0;
    OptD R, delta, vega, theta, w, var_left, minutes_to_close, adverse_dollars, move_scale;
    bool quotable = false;
    std::string source, reason;      // reason empty = Python None
};

using EdgeMap = kp::pycompat::ODict<std::string, EdgeRecord>;

struct EdgeSummary {
    long n = 0, n_priced = 0, refused = 0;
    OptD edge_min, edge_med, edge_max;
    bool enabled = false;
    std::string reason;
    bool priced = false;             // false = the reference's short {n, source, reason} shape
};

class EdgeManager {
public:
    // Passing a calendar and/or curve skips the artifact load, which is what the gates and this
    // port's tests do. A failed curve load installs a FlatCurve AND sets enabled=false -- that is
    // a failure, not the operator's choice, and the two must not be confused.
    EdgeManager(std::string underlying,
                std::optional<kp::pricing::varcal::VarianceCalendar> calendar = std::nullopt,
                std::optional<kp::pricing::edge::Curve> curve = std::nullopt,
                std::optional<bool> enabled = std::nullopt,
                const std::string& root = "");

    bool available() const { return calendar_.has_value() && curve_.has_value(); }

    // Set by an operator asking for the flat quoter. DELIBERATELY a separate flag from `enabled`,
    // which also goes false when the calibration artifact is missing.
    void set_operator_flat(bool v) { operator_flat_ = v; }
    bool operator_flat() const { return operator_flat_; }
    bool enabled() const { return enabled_; }
    const std::string& reason() const { return reason_; }

    // Recompute R and the required edge for every market.
    //
    // Cached: a call inside the recompute window returns the previous map untouched, which is
    // what keeps a fast quote loop from paying for a slow calculation on every pass. `mono_s` is the
    // injected monotonic clock the window is measured on -- it is NOT the same clock as `now`.
    //
    // `move_scale` is THE ADVERSE-SELECTION GATE's input: the futures move expected over the
    // window between a tick moving and our cancel landing (config::QUOTE_EXPOSURE_WINDOW_S, nearly
    // all vendor + network).
    // A resting quote is a free option over that window, so a market whose |delta| * move_scale
    // exceeds the edge charged is refused with reason "adverse_selection". MISSING = gate off.
    const EdgeMap& refresh(
        const std::vector<kp::pricing::theo::Market>& markets, OptD forward, OptD sigma,
        const kp::time::Instant& now, double mono_s, bool force = false,
        OptD move_scale = std::nullopt);

    EdgeRecord get(const std::string& ticker) const;
    EdgeSummary summary() const;

    const EdgeMap& cache() const { return cache_; }
    const std::optional<kp::pricing::varcal::VarianceCalendar>& calendar() const { return calendar_; }
    const std::optional<kp::pricing::edge::Curve>& curve() const { return curve_; }
    const std::string& underlying() const { return underlying_; }

private:
    EdgeRecord flat(const std::string& reason = "") const;
    EdgeRecord flat_override() const;
    EdgeRecord no_r(const std::string& reason = "") const;

    std::string underlying_, reason_;
    bool enabled_ = false, operator_flat_ = false;
    std::optional<kp::pricing::varcal::VarianceCalendar> calendar_;
    std::optional<kp::pricing::edge::Curve> curve_;
    kp::pricing::edge::FlatCurve flat_curve_;
    EdgeMap cache_;             // insertion-ordered, as the reference's dict is
    double cache_t_ = 0.0;
    bool has_cache_ = false;
    std::optional<kp::time::Instant> last_computed_;
};

}  // namespace kp::strategy::edgemgr
