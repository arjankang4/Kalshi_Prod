// The edge lean: how the realized-vol view reaches a quote.
//
// Port of the LIVE half of Kalshi/pricing/lean.py -- `edge_adjust` only.
// `side_offset` and `clamp_price` are the older offset-and-clamp mechanism, reached from the
// backtest and from nothing on the live path; they moved the quoted PRICE, and are deliberately
// not ported. Verified live: edge_adjust is called from run_04_quote_loop.build_desired.
//
// IT NEVER MOVES THE THEO. The fair value stays the digital priced off IMPLIED vol through the
// smile, delta-adjusted on futures. What the realized-vol view changes is how much edge we DEMAND
// on each side -- charge more where it says we are about to be picked off, accept less where it
// says we are being offered something cheap. The worst case here is quoting wider on one side and
// tighter on the other; the worst case of the price-moving mechanism was a hard clamp putting a
// price on the wire the implied model never chose.
//
// THE MAPPING, in probability points. d = (p_rv - p_impl) * 100, clamped to +-CLAMP,
// u = |d| / CLAMP (CLAMP = LEAN_DISAGREEMENT_CLAMP):
//
//     defensive side (the one the disagreement says is dangerous):   required edge + DEF * u cents
//     aggressive side (the one it says is cheap):                    required edge - AGG * u cents
//
// with DEF = LEAN_DEFENSIVE_CENTS and AGG = LEAN_AGGRESSIVE_CENTS.
//
// SIGN. d > 0 means realized says the market is MORE likely than implied does, so we want to BUY
// yes: aggressive on the bid, defensive on the offer. d < 0 mirrors it.
//
// ASYMMETRY IS THE POINT. The two legs need not be equal: being wrong about a lean costs a fill at
// a bad price, being right only earns a slightly better one, so the payoffs are not symmetric.
#pragma once
#include <optional>
#include <string>
#include <utility>

namespace kp::pricing::lean {

using OptD = std::optional<double>;
inline constexpr std::nullopt_t MISSING = std::nullopt;

inline constexpr double LEAN_DISAGREEMENT_CLAMP = 8.0;    // probability POINTS
inline constexpr double LEAN_DEFENSIVE_CENTS   = 1.5;     // added to required edge at the clamp
inline constexpr double LEAN_AGGRESSIVE_CENTS  = 0.75;    // subtracted at the clamp

// Every field always present, mirroring the reference's dict. `lean_reason` empty means Python
// None, which happens only in the impossible case where neither branch below ran.
struct LeanDetail {
    OptD lean_d, lean_u;
    std::string lean_side;
    double lean_cents = 0.0;
    std::string lean_reason;
};

// (delta_micros, detail) -- how much to shift this side's required edge, in micro-dollars.
//
// Positive widens (defensive), negative tightens (aggressive). Returns 0 with a reason whenever
// the lean has nothing to say, so a caller can add it unconditionally.
//
// `side` is "bid" or "ask". Any other string is treated as not-the-aggressive-side and therefore
// scores defensive, exactly as the Python's `side == aggressive_side` comparison does -- this is
// carried across rather than validated, because validating it here would change behaviour.
//
// NOTE `clamp` is a divisor and the reference does not guard it: clamp = 0 raises
// ZeroDivisionError in Python. Nothing calls it that way -- the live path passes no override at
// all -- so the C++ deliberately does not invent a defined result for a case the reference
// refuses to produce one for.
std::pair<long, LeanDetail> edge_adjust(OptD p_impl, OptD p_rv, const std::string& side,
                                        OptD clamp = MISSING, OptD defensive = MISSING,
                                        OptD aggressive = MISSING);

// --- the lean backoff (lean.py:153-195) ----------------------------------------------------------
//
// A charge added to the ENTRY bar for a (market, side) we have just traded, so a single adverse
// move cannot be sold to over and over: BACKOFF_START_CENTS at the instant of the fill, decaying
// exponentially to BACKOFF_MID_CENTS at BACKOFF_HALF_S, then linear to 0c at BACKOFF_END_S. On
// the entry bar only, never the exit, and
// per (market, side), so flattening is never taxed. `age_s` is seconds since the side's last fill
// (`orders::MarketState::last_fill`), absent when the side is clean.
inline constexpr double BACKOFF_START_CENTS = 1.5;
inline constexpr double BACKOFF_MID_CENTS = 0.75;
inline constexpr double BACKOFF_HALF_S = 4.0;
inline constexpr double BACKOFF_END_S = 8.0;

// Cents. 0 for an absent or negative age, and from `end` on. The exponential leg is
// start * (mid/start) ** (age/half) -- a pow, as the reference's `**` is.
double backoff_cents(OptD age_s, OptD start = MISSING, OptD mid = MISSING, OptD half = MISSING,
                     OptD end = MISSING);

// int(round(backoff_cents(...) * 10_000)) -- micro-dollars, Python's half-to-even round.
long backoff_micros(OptD age_s, OptD start = MISSING, OptD mid = MISSING, OptD half = MISSING,
                    OptD end = MISSING);

}  // namespace kp::pricing::lean
