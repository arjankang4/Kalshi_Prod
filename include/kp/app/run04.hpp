// The pure half of strategy/run_04_quote_loop.py: the functions `cycle` and `main` call that do no
// I/O. Kept apart from the loop so each can be golden-tested against the reference directly.
#pragma once
#include <optional>
#include <string>
#include <vector>

#include "kp/pricing/atmvol.hpp"
#include "kp/pricing/theo_core.hpp"
#include "kp/pricing/varcal.hpp"
#include "kp/time/et.hpp"

namespace kp::app::run04 {

using OptD = std::optional<double>;

inline constexpr double QUOTE_INTERVAL_S = 1.0;
inline constexpr double WARMUP_S = 15.0;
inline constexpr double BOARD_REFRESH_S = 120.0;
inline constexpr double MIN_CYCLE_FRACTION = 0.5;
inline constexpr double MAX_ERROR_RATE = 0.10;
inline constexpr double MIN_SPOT_RATE = 0.5;

// --- the verdict -------------------------------------------------------------------------
// A scheduled check that cannot fail is not a check: PASS, FAIL, NO_SESSION (spot never resolved
// on a closed market, exit 0) and NO_SPOT (spot never resolved INSIDE the session gate, exit 1).
struct RunStats { long cycles = 0, errors = 0, no_spot = 0, placed = 0; };
struct Verdict {
    std::string verdict;             // "PASS" | "FAIL" | "NO_SESSION" | "NO_SPOT"
    int exit_code = 1;
    long cycles = 0, expected = 0, errors = 0, no_spot = 0, placed = 0;
    double error_rate = 1.0, spot_rate = 0.0;
    bool in_session = false;
    std::vector<std::string> reasons;
};
Verdict verdict(const RunStats& stats, double expected_cycles, bool in_session);

// Python's f"{x:.0%}" -- round-half-even of x*100 to an integer, then "%". Used by the verdict
// reasons and the VERDICT line, which operators grep.
std::string pct0(double x);

// --- the market list between refreshes ---------------------------------------------------
// Drop markets whose close_time has passed, locally. No stamp, or an unparseable one, is KEPT
// (the refresh will settle it). `now_ns` is the loop's `now`.
std::vector<kp::pricing::theo::Market> still_open(
    const std::vector<kp::pricing::theo::Market>& markets, long long now_ns);

// Annualised ATM implied vol on the intraday variance clock, or nullopt:
//   fit.w / ((1 - phi(now)) / 252) ** 0.5, refused when the fit is not ok or <= 1e-6 is left.
OptD sigma(const kp::pricing::atmvol::AtmFit* fit, const kp::pricing::varcal::VarianceCalendar& cal,
           const kp::time::Instant& now);

// --- config.py's clock helpers -----------------------------------------------------------
// int(datetime.now(timezone.utc).timestamp() * 1e9) -- the "now" IndexQuote.age_s measures
// against when no clock is passed, i.e. inside spot.resolve. NOT the exact nanosecond clock:
// timestamp() is float seconds from microseconds, and the product is truncated.
long long py_timestamp_ns(const kp::time::Instant& now);

bool in_trading_session(const kp::time::Instant& now);          // Mon-Fri, [09:30, 16:00) ET
std::string run_stamp(const kp::time::Instant& now);            // %Y%m%dT%H%M%SZ, UTC
std::string occ_expiry(const kp::time::Instant& now);           // %y%m%d of the ET date
std::string board_stamp(const kp::time::Instant& now);          // f"{d:%y%b%d}".upper(), ET date

}  // namespace kp::app::run04
