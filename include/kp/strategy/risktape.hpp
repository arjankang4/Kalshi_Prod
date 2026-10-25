// A row a second of what we are actually holding, for post-session risk analysis.
//
// Port of strategy/risktape.py. One flat row per (instant, underlying): positions, dollar
// delta/vega/theta, and -- the point of it -- VEGA BUCKETED BY THE 4PM VANILLA CALL DELTA at each
// market's reference strike, so "long the 25-delta wing, short the 60-delta body" is readable.
// A binary's vega is signed, so +$100 of portfolio vega can be two opposite books.
//
//   reference strike: floor_strike, else cap_strike (the bottom bracket, `less`, has no floor).
//   the vanilla: the 16:00 one, Black-76 N(d1) at the smile's w(K) (flat ATM vol fallback
//   reconstructed from sigma and the variance calendar when there is no curve).
//
// The row is built as a kp::json::Value in the reference dict's KEY ORDER and written through
// strategy::journal, so the risk.jsonl line is byte-identical. Clocks are parameters: `now_mono`
// (time.monotonic, the cadence) and `ts_ns` (time.time_ns, the row stamp).
#pragma once
#include <array>
#include <optional>
#include <string>
#include <vector>

#include "kp/json/value.hpp"
#include "kp/pricing/pricer.hpp"
#include "kp/pricing/skew.hpp"
#include "kp/pricing/theo_core.hpp"
#include "kp/pricing/varcal.hpp"
#include "kp/pycompat/odict.hpp"
#include "kp/strategy/edgemgr.hpp"
#include "kp/strategy/journal.hpp"
#include "kp/time/et.hpp"

namespace kp::strategy::risktape {

using OptD = std::optional<double>;

// Row cadence; config.RISK_TAPE_S does not exist, so the getattr default runs.
inline constexpr double RISK_TAPE_S = 2.0;

// Ten-delta buckets; the last is closed at 1.0.
inline constexpr std::array<double, 11> BUCKET_EDGES = {0.0, 0.1, 0.2, 0.3, 0.4, 0.5,
                                                        0.6, 0.7, 0.8, 0.9, 1.0};
inline constexpr std::array<const char*, 10> BUCKET_NAMES = {
    "d00_10", "d10_20", "d20_30", "d30_40", "d40_50",
    "d50_60", "d60_70", "d70_80", "d80_90", "d90_100"};

// The strike whose vanilla call delta buckets this market: floor, else cap, else MISSING.
OptD reference_strike(const kp::pricing::theo::Market& m);

// N(d1) of the 4pm vanilla call at `strike`, at the smile's w(K) when the fit has an ATM vol,
// else at `w_atm`; MISSING when nothing identifies a vol or an input is missing/non-positive.
OptD vanilla_call_delta(OptD forward, OptD strike, const kp::pricing::skew::SkewFit* skew_fit,
                        OptD w_atm = std::nullopt);

// Index into BUCKET_NAMES (delta clamped to [0, 1]), or MISSING for a MISSING delta.
std::optional<int> bucket_of(OptD delta);

// {ticker: signed net contracts}, in the feed's insertion order (the sums run in it). A value
// may be absent, as the reference's None.
using Positions = kp::pycompat::ODict<std::string, OptD>;

struct Detail {
    long rows = 0, errors = 0;
    std::optional<std::string> last_error;
    double interval_s = 0.0;
    kp::json::Value to_json() const;
};

class RiskTape {
public:
    // `journal` may be null (rows are built and returned, never written). Not owned.
    explicit RiskTape(journal::Journal* journal, OptD interval_s = std::nullopt);

    bool due(double now_mono) const { return (now_mono - last_) >= interval_s_; }

    // Build (and by default write) one row. Returns the row. `forward`/`skew_fit` are what the
    // reference reads off the pricer (`pricer.spot`, `pricer.skew`); the Pricer overload below
    // does exactly that, including building the pricer's lazy curve.
    kp::json::Value snapshot(const std::string& underlying, const kp::time::Instant& now,
                             const Positions& positions, const edgemgr::EdgeMap* em_map,
                             const std::vector<kp::pricing::theo::Market>& markets, OptD forward,
                             const kp::pricing::skew::SkewFit* skew_fit,
                             const kp::pricing::varcal::VarianceCalendar* calendar,
                             OptD sigma_impl, OptD sigma_rv, OptD spot, OptD fast_spot,
                             const std::vector<kp::pricing::theo::Market>& hourly_markets,
                             double now_mono, long long ts_ns, bool write = true);

    kp::json::Value snapshot(const std::string& underlying, const kp::time::Instant& now,
                             const Positions& positions, const edgemgr::EdgeMap* em_map,
                             const std::vector<kp::pricing::theo::Market>& markets,
                             const kp::pricing::pricer::Pricer* pricer,
                             const kp::pricing::varcal::VarianceCalendar* calendar,
                             OptD sigma_impl, OptD sigma_rv, OptD spot, OptD fast_spot,
                             const std::vector<kp::pricing::theo::Market>& hourly_markets,
                             double now_mono, long long ts_ns, bool write = true);

    Detail detail() const;

    long rows() const { return rows_; }
    long errors() const { return errors_; }
    double interval_s() const { return interval_s_; }

private:
    journal::Journal* journal_;
    double interval_s_;
    double last_ = 0.0;
    long rows_ = 0, errors_ = 0;
    std::optional<std::string> last_error_;
};

}  // namespace kp::strategy::risktape
