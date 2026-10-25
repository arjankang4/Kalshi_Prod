// Fast spot: the futures level, carried on the index's own scale.
//
//     spot_fast = F_now - EMA(F - index, sampled when the index ticks)
//
// Port of pricing/basis.py (`BasisTracker`).
//
// NOT a change of pricing reference: KXINX settles on the INDEX at 16:00, so everything downstream
// keeps taking a number on the index's scale. The futures supply the motion, the EMA supplies the
// level. Measured 2026-08-26: SPX `futures - index` stdev 0.316 points against 1-3 points of index
// lag, which is the whole argument for it.
//
// THE EMA IS SAMPLED ON THE INDEX TICK (observe), never on a timer or on futures frames: the
// difference `F - index` is only meaningful when both are current, and the index is the slow leg.
//
// FAILURE IS SOFT AND DIRECTIONAL. No futures, a warming basis, or a residual outside the alarm
// band, and spot() returns MISSING with a reason; the caller falls back to pricing/spot.
//
// THE CLOCK IS INJECTED: the reference's `now=None` reads time.monotonic(); here `now` is always
// passed (seconds on the caller's monotonic clock), so replay and test are deterministic.
//
// NOT THREAD-SAFE, exactly like the reference: run_04 folds from a basis thread and reads spot()
// from the cycle thread with no lock (the GIL made each Python bytecode atomic). The C++ caller
// that shares one tracker across threads must hold a mutex around observe()/spot()/detail().
#pragma once
#include <optional>
#include <string>

#include "kp/json/value.hpp"
#include "kp/massive/index.hpp"
#include "kp/pycompat/odict.hpp"

namespace kp::pricing::basis {

using OptD = std::optional<double>;
inline constexpr std::nullopt_t MISSING = std::nullopt;

// Half-life of the basis EMA, seconds (the reference module constant; config.py does not set it).
inline constexpr double DEFAULT_HALF_LIFE_S = 45.0;

// Break-detector band as a FRACTION OF SPOT. The getattr default; config.BASIS_ALARM_PCT does not
// exist.
inline constexpr double DEFAULT_ALARM_PCT = 0.001;

// Index ticks before the EMA is trusted.
inline constexpr int MIN_SAMPLES = 20;

// The `detail` dict spot() returns. `spot` is present only on success (the reference adds the key
// only then), so to_json() omits it when absent rather than writing null.
struct SpotDetail {
    std::string underlying;
    OptD basis;
    long n = 0;
    OptD residual;
    std::optional<std::string> reason;
    bool alarm = false;
    OptD spot;
    kp::json::Value to_json() const;
};

struct SpotResult {
    OptD spot;            // the fast spot, or MISSING whenever it cannot be justified
    SpotDetail detail;
};

// BasisTracker.detail().
struct TrackerDetail {
    std::string underlying;
    OptD basis;
    long n = 0;
    double half_life_s = 0.0;
    bool ready = false;
    OptD last_residual;
    double max_abs_residual = 0.0;          // round(_, 4)
    double alarm_pct = 0.0;
    OptD alarm_points;                      // round(alarm_pct * last_level, 3)
    kp::pycompat::ODict<std::string, long> refused;   // first-refusal order, as the dict
    kp::json::Value to_json() const;
};

class BasisTracker {
public:
    explicit BasisTracker(std::string underlying, OptD half_life_s = MISSING,
                          OptD alarm_pct = MISSING, int min_samples = MIN_SAMPLES);

    // Fold one (futures, index) pair into the EMA. Call when the INDEX ticks. Returns true if it
    // was folded; a repeated index ts_ns is the same observation and is ignored.
    bool observe(OptD futures_mid, const std::optional<kp::massive::index::IndexQuote>& index_quote,
                 double now);

    bool ready() const { return ema_.has_value() && n_ >= min_samples_; }
    OptD basis() const { return ema_; }

    // THE CALL FastGuard makes: (fast_spot, detail) for the current futures mid.
    SpotResult spot(OptD futures_mid);

    TrackerDetail detail() const;

    const std::string& underlying() const { return underlying_; }
    double half_life_s() const { return half_life_s_; }
    double alarm_pct() const { return alarm_pct_; }
    int min_samples() const { return min_samples_; }
    long n() const { return n_; }

private:
    void bump(const std::string& key);

    std::string underlying_;
    double half_life_s_, alarm_pct_;
    int min_samples_;
    OptD ema_, t_;
    long n_ = 0;
    std::optional<long long> last_index_ts_;
    OptD last_resid_;
    double max_resid_ = 0.0;
    kp::pycompat::ODict<std::string, long> refused_;
    OptD last_level_;
};

}  // namespace kp::pricing::basis
