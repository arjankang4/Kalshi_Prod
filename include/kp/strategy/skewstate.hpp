// The live skew curve: refit on a slow cadence, smoothed over a short window.
//
// Port of strategy/skewstate.py. `pricing/skew` is the pure fitter (one snapshot in, one curve
// out); this is the stateful half that knows what time it is.
//
//  * The queue is keyed by BUCKET LABEL, not strike, so a bucket keeps its history when spot moves
//    and the strike underneath it changes.
//  * It smooths RATIOS (w/w_atm), not vols: the level decays all day, the shape does not.
//  * The smoothed curve is rebuilt on each bucket's NOMINAL x.
//  * FAILING SOFT IS THE CONTRACT: any failure returns a FLAT curve with a reason, which prices as
//    the pre-skew model did.
//
// THE CLOCK IS INJECTED. The reference's observe(now=None) reads time.monotonic(); here the
// monotonic reading is a parameter. validate_theo calls observe() without a `now`, so the C++
// resolve_and_price_boards takes the reading and hands it through (see `observer()` below).
#pragma once
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "kp/json/value.hpp"
#include "kp/pricing/skew.hpp"

namespace kp::strategy::skewstate {

using OptD = std::optional<double>;
namespace skew = kp::pricing::skew;

// getattr(config, ..., default) -- all three exist in config and match.
inline constexpr double WINDOW_S = 60.0;      // config::SKEW_WINDOW_S
inline constexpr double RECOMPUTE_S = 3.0;    // config::SKEW_RECOMPUTE_S
inline constexpr double HALF_LIFE_S = 15.0;   // config::SKEW_HALF_LIFE_S

// A bucket needs this many live observations before its smoothed value is trusted.
inline constexpr int MIN_SAMPLES = 3;

// SkewState.detail(), for the journal. `queue_depth` is sorted by label, as the reference sorts it.
struct Detail {
    std::string underlying;
    std::optional<std::string> skew_reason;
    bool skew_flat = true;
    long n_fits = 0;
    long n_buckets_smoothed = 0, n_buckets_raw = 0;
    std::optional<std::string> raw_reason;
    std::vector<std::pair<std::string, long>> queue_depth;
    double window_s = 0.0, half_life_s = 0.0, recompute_s = 0.0;
    kp::json::Value to_json() const;
};

class SkewState {
public:
    // label -> (monotonic_t, ratio), oldest first.
    using Queue = std::deque<std::pair<double, double>>;

    explicit SkewState(std::string underlying, OptD window_s = std::nullopt,
                       OptD recompute_s = std::nullopt, OptD half_life_s = std::nullopt);

    // Fold one board snapshot in and return the curve to price with. Refits at most every
    // recompute_s (or on `force`); the smoothed curve is rebuilt on every call because the queue
    // ages between fits. `now` is a monotonic reading in seconds.
    const skew::SkewFit& observe(const skew::Board& quotes, const std::vector<double>& strikes,
                                 OptD forward, OptD w_atm, double now, bool force = false);

    // Build the served curve from the smoothed per-bucket ratios at time `t`. `strikes` arms the
    // monotone projection and the ladder guard; an empty grid degrades to the old behaviour.
    // Public because the reference's tests call `_rebuild` directly.
    skew::SkewFit rebuild(OptD forward, OptD w_atm, double t,
                          const std::vector<double>& strikes = {});

    // Recency-weighted mean of a bucket's ratios, weight 0.5 ** (age / half_life). MISSING below
    // MIN_SAMPLES.
    OptD ema(const std::string& label, double t) const;

    // The last served curve, or nullptr before the first observation.
    const skew::SkewFit* curve() const { return smoothed_ ? &*smoothed_ : nullptr; }
    const skew::SkewFit* raw() const { return raw_ ? &*raw_ : nullptr; }

    Detail detail() const;

    const std::string& underlying() const { return underlying_; }
    double window_s() const { return window_s_; }
    double recompute_s() const { return recompute_s_; }
    double half_life_s() const { return half_life_s_; }
    long n_fits() const { return n_fits_; }
    const std::optional<std::string>& reason() const { return reason_; }
    const std::map<std::string, Queue>& queues() const { return queues_; }

private:
    void push(const skew::SkewFit& raw_fit, double t);
    void prune(double t);

    std::string underlying_;
    double window_s_, recompute_s_, half_life_s_;
    // std::map, not insertion order: nothing iterates it for arithmetic (rebuild walks
    // bucket_offsets()), and detail() wants it sorted by label, which is what the reference emits.
    std::map<std::string, Queue> queues_;
    OptD last_fit_t_;                        // MISSING = never fitted, not "fitted at t=0"
    std::optional<skew::SkewFit> raw_, smoothed_;
    long n_fits_ = 0;
    std::optional<std::string> reason_ = std::string("no_observation");
};

// The seam validate_theo takes: observe `state` at monotonic time `now` (the reference's
// observe(..., now=None) inside _resolve_and_smooth).
using Observer = std::function<skew::SkewFit(const skew::Board&, const std::vector<double>&,
                                             OptD forward, OptD w_atm)>;
Observer observer(SkewState& state, double now);

}  // namespace kp::strategy::skewstate
