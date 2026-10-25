// Theoretical value: cube-root microprice -> digital probabilities -> bracket fair value.
//
// Port of the PURE half of Kalshi/pricing/theo.py -- everything that does not
// need a board or a skew curve. `Pricer` lives in kp/pricing/pricer.hpp. Splitting here breaks
// the theo <-> skew import cycle the Python resolves lazily (theo.Pricer.skew builds on first
// use), and it lets the bracket arithmetic be golden-tested with nothing but a lambda.
//
// Departures from the original spec as literally written (all three flagged in the plan, and
// carried across verbatim):
//   1. Call digital is (C(K_lo) - C(K_hi))/w. The spec's (C(7705)-C(7695)) is negative, since
//      calls decrease in strike.  The put form in the spec is already correctly signed.
//   2. The divisor is the *actual* strike width (K_hi - K_lo), not a hardcoded 2. The spec's
//      "/2" fits SPY $1 strikes; on the SPX 7695/7705 strikes it also names, the width is 10.
//   3. The microprice cube-roots BOTH sizes. The spec's worked example gives $1.04 by mixing
//      cbrt(8)=2 with the raw 8; applying the stated rule consistently gives $1.0333.
//
// Bracket arithmetic is a lookup table, not an if-chain. Kalshi's `rules_primary` text is what
// pins each type's inclusivity, and it does NOT match the OpenAPI field docs (`floor_strike` is
// documented as the minimum value that settles YES, i.e. inclusive, yet `greater`/`less` use
// their bound exclusively):
//
//     greater           floor_strike=7691.76   "is above 7691.76"             S >  K
//     greater_or_equal  floor_strike=7870      "is above 7869.9999"           S >= K
//     less              cap_strike=7375        "is below 7375"                S <  K
//     less_or_equal     cap_strike             (none listed today)            S <= K
//     between           floor=7375 cap=7399.9999  "is between 7375-7399.9999" lo <= S <= hi
//
// On why four one-sided structures when only two numbers exist: for a continuous index
// P(S > K) == P(S >= K), so the four collapse to two. The atom P(S == K) is not literally zero
// -- Kalshi settles to the index rounded to 2dp -- but it is negligible. With 0DTE sigma ~0.8%
// of 7700 (~62 pts), peak density is ~1/(62*sqrt(2pi)) ~ 0.0064/pt, so a 0.01-wide atom is
// ~6e-5, i.e. 0.006 cents against a 1-cent tick. Five minutes from the close (sigma ~7 pts) it
// is ~6e-4; only inside the last ~30 seconds does it reach ~2e-3, and only for the bracket
// straddling spot. `digital_above` reads a spread off a 5- or 10-point strike grid, so its own
// resolution floor is ~1/w per point -- orders of magnitude coarser than the atom it would need
// to resolve.
//
// So the strict/non-strict split is carried as SEMANTICS (the record says which event was
// priced, auditable against Kalshi's rules text) and deliberately not as NUMERICS. It is one
// place to change if a settlement-grid model is ever fitted.
#pragma once
#include "kp/json/value.hpp"
#include <array>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace kp::pricing::theo {

using OptD = std::optional<double>;
inline constexpr std::nullopt_t MISSING = std::nullopt;

// Kalshi spells one boundary two ways: a `between` market caps at 7574.9999 and the next floors
// at 7575. Rounding only inside a tolerance collapses those without moving a genuinely
// fractional level -- KXINXDUD floors at the prior close, e.g. 7691.76.
inline constexpr double BOUNDARY_ROUND_TOL = 0.02;

double clamp(double x, double lo = 0.0, double hi = 1.0);
OptD   normalize_boundary(OptD x);

// One row of a Kalshi board, carrying only what the pricing path reads.
//
// This is a DATA CONTRACT, not a convenience: every module downstream of the board -- the pricer,
// the band, capital selection, the edge manager, the ladder -- reads markets through this struct,
// so a field added here is added once. The Python passes raw `/markets` dicts around and each
// consumer does its own `.get()`, which is why `strike_type` has three spellings of "absent"
// (missing key, JSON null, empty string) all meaning the same thing. Here absence is one thing.
//
// NOTE what is deliberately NOT here: `status`. The documented /markets enum
// ([unopened, open, paused, closed, settled]) does not match the field -- a live market reads
// `active` or `finalized`, `status=open` returns ZERO markets against a trading board, and
// `status=active` is rejected as an invalid filter. Anything reasoning about liveness must use
// close_time, as discover_boards does.
struct Market {
    std::string ticker;
    std::string strike_type;      // empty = Python None
    OptD floor_strike, cap_strike;
    std::optional<long long> close_time_s;   // epoch seconds; the hourly pricer reads it
    // Carried for the loop and the journal, not for pricing.
    std::string event_ticker;                 // empty = Python None
    std::optional<std::string> close_time;    // the raw string off the API, as received
};

// A market as the REST `/markets` rows deliver it. `close_time` is parsed the way
// edgemgr.settlement_of does it -- datetime.fromisoformat after "Z" -> "+00:00", naive read as
// UTC -- and an unparseable string leaves close_time_s absent, which every reader treats exactly
// as the reference treats settlement_of() returning None. Kalshi stamps whole seconds; a
// fractional part is truncated toward the past (never observed on the live API).
Market market_from_json(const kp::json::Value& row);

// datetime.fromisoformat(s.replace("Z", "+00:00")) -> epoch nanoseconds, or nullopt where
// Python raises ValueError. Accepts YYYY-MM-DD[T ]HH:MM[:SS[.f{1,6}]][Z|+HH:MM|-HH:MM]; naive = UTC.
std::optional<long long> parse_iso_ns(const std::string& s);

// Every distinct boundary a PRICEABLE market actually prices from, ascending.
//
// Rule-aware rather than "every non-null strike field": an unpriceable type has no boundary worth
// subscribing option strikes for, and a one-sided market prices from one end only.
std::vector<double> boundaries(const std::vector<Market>& markets);

// --- one-sided structures ---------------------------------------------------------------
enum class Prob { less = 0, less_or_equal = 1, greater = 2, greater_or_equal = 3 };
inline constexpr std::array<const char*, 4> PROB_KEYS = {
    "less", "less_or_equal", "greater", "greater_or_equal"};

// The four one-sided probabilities at one boundary, derived from P(S > K). All four are
// returned even though only two are distinct -- see the header comment for why the split is
// semantic, not numeric.
struct OneSided { std::array<OptD, 4> p; OptD get(Prob k) const { return p[(int)k]; } };
OneSided one_sided_probs(OptD p_above);

// One signed term of a bracket's payoff, in terms of a one-sided probability at a boundary.
enum class Bound { floor_strike, cap_strike };
struct Term { int sign; Bound bound; Prob prob; };

// Rule table, keyed by Kalshi's own strike_type spellings so this is a lookup rather than a
// translation. Returns an empty span for an unpriceable type.
const std::vector<Term>* strike_type_rule(const std::string& strike_type);
bool is_priceable(const std::string& strike_type);

// Distinguishable reasons beat one generic "unhandled": these price off something other than a
// scalar strike, so no digital can reach them.
std::string unpriceable_reason(const std::string& strike_type);

// --- bracket fair value -----------------------------------------------------------------
struct TermDetail { int sign; Bound bound; double boundary; Prob prob; OptD value; };

// The detail record always carries the same fields so downstream tables stay rectangular.
// Exactly one use_* is true for a priceable market; all are false otherwise.
struct BracketDetail {
    std::string strike_type;
    OptD lo, hi;
    std::string prob_used;
    bool use_less = false, use_less_or_equal = false;
    bool use_greater = false, use_greater_or_equal = false, use_between = false;
    bool have_lo = false, have_hi = false;
    OneSided probs_lo, probs_hi;
    OptD p_above_lo, p_above_hi;
    std::vector<TermDetail> terms;
    OptD fair;
    std::string reason, error;
};

// `p_above(K) -> P(S > K) | MISSING`. Pure: no board, no spot, no Pricer -- which is what lets
// the backtest share this exact arithmetic instead of reimplementing it.
std::pair<OptD, BracketDetail> bracket_fair(const std::string& strike_type,
                                            OptD floor_strike, OptD cap_strike,
                                            const std::function<OptD(double)>& p_above);

// --- microprice -------------------------------------------------------------------------
//
// theo.py:175. NOT std::cbrt: the Python is `max(x, 0.0) ** (1.0/3.0)`, which is pow, and the
// two disagree on most inputs (27 -> 3.0 vs 3.0000000000000004). This feeds every option
// microprice, hence every theo, hence every order. tests/unit/test_fpenv.cpp pins it.
double cbrt_py(double x);

// Cube-root size-weighted microprice. Weight leans toward the side with more size: heavy offer
// -> fair sits near the bid. Equivalent to bid + w_bid * (ask - bid) with
// w_bid = cbrt(bs) / (cbrt(bs) + cbrt(as)).
OptD micro(OptD bid, OptD ask, double bid_size, double ask_size);

// One Massive Q frame. bp/ap/bs/as are ABSENT, not zero, when there is no side -- Massive omits
// the fields entirely, which is very common for OTM 0DTE options.
struct Quote { OptD bp, ap, bs, as; bool present = false; };

// Massive Q frame -> microprice.
//
// An absent bid means ZERO, not unknown, so it is read as 0.00 x 0 -- which the cube-root
// weighting then pins the fair value to, since the bid carries no size. Reading it as "unknown"
// instead silently nulls ~half of all 0DTE quotes and every fair value with them.
OptD quote_value(const Quote& q);

// Is this quote a genuine two-sided market? THE canonical predicate.
//
// Stricter than quote_value, deliberately, and the strictness is load-bearing. Massive omits
// bp/bs entirely when there is no bid and quote_value correctly reads that as 0.00 -- the right
// call for pricing a wing, fatal for anchoring parity. A deep-ITM call whose bid has gone prices
// to 0.00, which makes |C - P| smallest at ITS strike, which wins the anchor and implies S at that
// strike. Measured on real tape: spot 7387.5 on a day whose true spot was 7706.
//
// It lives HERE and not in atmvol because theo is the lower module, and because the rule had been
// implemented three separate times in the reference -- here it was simply missing, which is what
// let the LIVE parity path carry the bug the other two guarded against. atmvol delegates.
//
// require_ask_size=false is the backtest's weaker form (bid size only); a vol anchor needs both
// sides quoted because the microprice interpolates between them.
bool two_sided(const Quote& q, bool require_ask_size = true);

// Closest listed strikes strictly below and strictly above `strike`.
struct Neighbors { OptD below, above; };
Neighbors neighbors(double strike, const std::vector<double>& strikes);

}  // namespace kp::pricing::theo
