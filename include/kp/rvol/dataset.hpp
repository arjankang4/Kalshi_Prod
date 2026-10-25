// The two constants of Kalshi/rvol/dataset.py that the LIVE path reads.
//
// The rest of dataset.py -- building the (features, targets) observation matrix from cached bars
// -- is offline and stays in Python. What the live reader needs from it is the lookback set and,
// critically, each lookback's OWN sampling scale: rvol/live.py passed the default 30s to every
// lookback until 2026-08-24, serving f_3h at 30s against a beta fitted at 60s and f_5m at 30s
// against one fitted at 10s. Scale is the LEVEL for an index (the Epps effect: I:NDX reads 9.26%
// at 1s and 13.76% at 30s), so that was a silent level error, not an approximation.
#pragma once
#include <array>
#include <string_view>

namespace kp::rvol::dataset {

struct Lookback {
    std::string_view name;
    int minutes;
    int scale_s;     // LOOKBACK_SCALE_S[name]
};

// LOOKBACKS = (("3h", 180), ("1h", 60), ("30m", 30), ("15m", 15), ("5m", 5)), in that order, with
// LOOKBACK_SCALE_S = {"3h": 60, "1h": 60, "30m": 60, "15m": 10, "5m": 10} folded in.
inline constexpr std::array<Lookback, 5> LOOKBACKS = {{
    {"3h", 180, 60}, {"1h", 60, 60}, {"30m", 30, 60}, {"15m", 15, 10}, {"5m", 5, 10},
}};

}  // namespace kp::rvol::dataset
