// Resolve the spot price for an underlying: real index level, with put-call parity as fallback.
//
// Port of Kalshi/pricing/spot.py.
//
// Until 2026-08-21 there was no choice to make -- `I:SPX` was not entitled, so spot was whatever
// put-call parity said. With the Index entitlement the index is the truer number, but it is not
// strictly better in every respect, and that is why this is a module rather than a one-line swap:
//
//   * THE INDEX IS TRUER. It is the thing KXINX actually settles on. Parity infers it from option
//     quotes and inherits every defect of those quotes -- most sharply the deep-ITM call with no
//     bid, which prices to 0.00, wins the min |C-P| anchor and implies S = its own strike.
//     Measured in the backtest: spot 7387.5 on a day whose true spot was ~7706.
//
//   * PARITY IS MORE INTERNALLY CONSISTENT. It is derived from the SAME quote snapshot the theos
//     are, so a stale or skewed option board moves parity and the theos together and the errors
//     partially cancel. An index level is correct but arrives on a different feed with its own
//     clock, so adopting it introduces a skew that did not previously exist.
//
// So: index when it is fresh, parity when it is not, and record BOTH every time along with their
// divergence. That divergence is the single best health signal available on either feed -- it goes
// wrong when the option board thins out AND when the index feed stalls, and it costs one
// subtraction.
#pragma once
#include <optional>
#include <string>

#include "kp/massive/index.hpp"

namespace kp::pricing::spot {

using OptD = std::optional<double>;
inline constexpr std::nullopt_t MISSING = std::nullopt;

// What spot is, where it came from, and how much the two sources disagreed.
//
// `value` is the number to price with. `source` is "index" or "parity" or empty. `divergence` is
// `index - parity` in points and is absent unless both were available -- which is exactly when it
// is meaningful. Flat struct with every field always present, so the journal row stays rectangular.
struct SpotResolution {
    OptD value;
    std::string source;              // "index" | "parity" | "" (Python None)
    OptD index;
    OptD parity;
    OptD index_age_s;
    std::optional<std::string> market_status;
    std::string reason;              // "" is Python None
    OptD divergence;
    OptD divergence_pct;
    bool alarm = false;
};

// (parity spot, live IndexQuote or absent) -> SpotResolution.
//
// Pure apart from the clock: takes the two numbers, not the feeds. `max_stale_s` absent means
// config::INDEX_MAX_STALE_S.
//
// `at_ns` is the seam the Python does not have. Python reads the wall clock twice inside this
// function -- once for `age_s` and once inside `is_fresh` -- so the age it RECORDS and the
// freshness it DECIDES are two different observations. Passing an instant makes them one and makes
// the resolution replayable; omitting it reproduces the Python exactly, drift included.
//
// The freshness test is on the VENDOR's timestamp, not on when we made the call, so a
// served-from-cache response cannot masquerade as live.
SpotResolution resolve(OptD parity_spot,
                       const std::optional<kp::massive::index::IndexQuote>& index_quote = std::nullopt,
                       OptD max_stale_s = MISSING,
                       std::optional<long long> at_ns = std::nullopt);

}  // namespace kp::pricing::spot
