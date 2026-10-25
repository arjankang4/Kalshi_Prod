#include "kp/pricing/lean.hpp"

#include <cmath>

#include "kp/pycompat/round.hpp"

namespace kp::pricing::lean {

std::pair<long, LeanDetail> edge_adjust(OptD p_impl, OptD p_rv, const std::string& side,
                                        OptD clamp, OptD defensive, OptD aggressive) {
    LeanDetail d;
    d.lean_side = side;
    if (!p_impl || !p_rv) {
        d.lean_reason = "no_chain";
        return {0L, d};
    }
    const double cl = clamp.value_or(LEAN_DISAGREEMENT_CLAMP);
    const double df = defensive.value_or(LEAN_DEFENSIVE_CENTS);
    const double ag = aggressive.value_or(LEAN_AGGRESSIVE_CENTS);

    const double diff = (*p_rv - *p_impl) * 100.0;
    d.lean_d = kp::pycompat::py_round_nd(diff, 4);
    const double abs_diff = std::fabs(diff);
    const double u = (abs_diff < cl ? abs_diff : cl) / cl;
    d.lean_u = kp::pycompat::py_round_nd(u, 4);
    if (u <= 0.0) {
        d.lean_reason = "no_disagreement";
        return {0L, d};
    }

    // d > 0: realized says MORE likely -> buy yes. The bid is the aggressive side, the ask the
    // defensive one. d < 0 mirrors it.
    const bool is_aggressive = (diff > 0.0) ? (side == "bid") : (side == "ask");
    const double cents = is_aggressive ? -ag * u : df * u;
    d.lean_cents = kp::pycompat::py_round_nd(cents, 4);
    d.lean_reason = is_aggressive ? "aggressive" : "defensive";
    // cents -> micro-dollars, through Python's half-to-even round() on the UNROUNDED cents.
    return {(long)kp::pycompat::py_round_i(cents * 10000.0), d};
}

double backoff_cents(OptD age_s, OptD start, OptD mid, OptD half, OptD end) {
    if (!age_s || *age_s < 0) return 0.0;
    const double st = start.value_or(BACKOFF_START_CENTS);
    const double md = mid.value_or(BACKOFF_MID_CENTS);
    const double hf = half.value_or(BACKOFF_HALF_S);
    const double en = end.value_or(BACKOFF_END_S);
    const double a = *age_s;
    if (a >= en) return 0.0;
    if (a <= hf) return st > 0 ? st * std::pow(md / st, a / hf) : 0.0;
    return md * (en - a) / (en - hf);
}

long backoff_micros(OptD age_s, OptD start, OptD mid, OptD half, OptD end) {
    return (long)kp::pycompat::py_round_i(backoff_cents(age_s, start, mid, half, end) * 10000.0);
}

}  // namespace kp::pricing::lean
