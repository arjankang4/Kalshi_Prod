#include "kp/pricing/theo_core.hpp"

#include <set>
#include "kp/pycompat/round.hpp"

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <map>

namespace kp::pricing::theo {

namespace pc = kp::pycompat;

// Python's min(hi, max(lo, x)). Spelled out rather than std::clamp: Python's max returns the
// first argument unless the second is strictly greater, and min the first unless the second is
// strictly less, which fixes the behaviour on signed zeros and NaN.
double clamp(double x, double lo, double hi) {
    const double m = (x > lo) ? x : lo;
    return (hi < m) ? hi : m;
}

OptD normalize_boundary(OptD x) {
    if (!x) return MISSING;
    const double f = *x;
    const double r = pc::py_round(f);      // banker's, as Python's round()
    return (std::fabs(f - r) <= BOUNDARY_ROUND_TOL) ? r : f;
}

std::vector<double> boundaries(const std::vector<Market>& markets) {
    // Python builds a set then returns sorted(out). A std::set gives both in one step; the
    // ordering is a total order on doubles here, so there is no stability question to answer.
    std::set<double> out;
    for (const Market& m : markets) {
        const std::vector<Term>* rule = strike_type_rule(m.strike_type);
        if (!rule) continue;
        for (const Term& t : *rule) {
            const OptD b = normalize_boundary(t.bound == Bound::floor_strike ? m.floor_strike
                                                                             : m.cap_strike);
            if (b) out.insert(*b);
        }
    }
    return {out.begin(), out.end()};
}

OneSided one_sided_probs(OptD p_above) {
    OneSided o;
    if (!p_above) return o;                // all four MISSING
    const double below = 1.0 - *p_above;
    o.p[(int)Prob::less] = below;
    o.p[(int)Prob::less_or_equal] = below;
    o.p[(int)Prob::greater] = *p_above;
    o.p[(int)Prob::greater_or_equal] = *p_above;
    return o;
}

namespace {

const std::map<std::string, std::vector<Term>>& rules() {
    static const std::map<std::string, std::vector<Term>> R = {
        {"greater",          {{+1, Bound::floor_strike, Prob::greater}}},
        {"greater_or_equal", {{+1, Bound::floor_strike, Prob::greater_or_equal}}},
        {"less",             {{+1, Bound::cap_strike,   Prob::less}}},
        {"less_or_equal",    {{+1, Bound::cap_strike,   Prob::less_or_equal}}},
        // lo <= S <= hi  ==  P(S >= lo) - P(S > hi). Mixing a non-strict with a strict term is
        // deliberate: it matches Kalshi's inclusive `between` rules text, and it is numerically
        // identical to the old P(>lo) - P(>hi) since the two spellings share a value.
        {"between",          {{+1, Bound::floor_strike, Prob::greater_or_equal},
                              {-1, Bound::cap_strike,   Prob::greater}}},
    };
    return R;
}

const char* prob_name(Prob p) { return PROB_KEYS[(int)p]; }
const char* bound_name(Bound b) { return b == Bound::floor_strike ? "floor_strike" : "cap_strike"; }

}  // namespace

const std::vector<Term>* strike_type_rule(const std::string& st) {
    auto it = rules().find(st);
    return it == rules().end() ? nullptr : &it->second;
}

bool is_priceable(const std::string& st) { return strike_type_rule(st) != nullptr; }

std::string unpriceable_reason(const std::string& st) {
    if (st == "functional") return "functional_strike";
    if (st == "custom")     return "custom_strike";
    if (st == "structured") return "structured_strike";
    if (st.empty())         return "null_strike_type";
    return "unknown_strike_type";
}

std::pair<OptD, BracketDetail> bracket_fair(const std::string& strike_type,
                                            OptD floor_strike, OptD cap_strike,
                                            const std::function<OptD(double)>& p_above) {
    BracketDetail d;
    d.strike_type = strike_type;
    d.lo = normalize_boundary(floor_strike);
    d.hi = normalize_boundary(cap_strike);

    const std::vector<Term>* rule = strike_type_rule(strike_type);
    if (rule == nullptr) {
        d.reason = unpriceable_reason(strike_type);
        d.error  = "unhandled strike_type '" + strike_type + "'";
        return {MISSING, d};
    }

    d.prob_used = strike_type;
    if      (strike_type == "less")             d.use_less = true;
    else if (strike_type == "less_or_equal")    d.use_less_or_equal = true;
    else if (strike_type == "greater")          d.use_greater = true;
    else if (strike_type == "greater_or_equal") d.use_greater_or_equal = true;
    else if (strike_type == "between")          d.use_between = true;

    // boundary -> probs, so a `between` calls p_above once per distinct boundary. Keyed by the
    // double's exact value, matching the Python dict.
    std::map<double, OneSided> memo;
    double total = 0.0;
    for (const Term& term : *rule) {
        const OptD b = (term.bound == Bound::floor_strike) ? d.lo : d.hi;
        if (!b) {
            d.reason = std::string("missing_") + bound_name(term.bound);
            d.error  = strike_type + " needs " + bound_name(term.bound);
            return {MISSING, d};
        }
        auto it = memo.find(*b);
        if (it == memo.end()) {
            const OptD pa = p_above(*b);
            it = memo.emplace(*b, one_sided_probs(pa)).first;
            if (term.bound == Bound::floor_strike) { d.p_above_lo = pa; d.probs_lo = it->second; d.have_lo = true; }
            else                                   { d.p_above_hi = pa; d.probs_hi = it->second; d.have_hi = true; }
        }
        const OptD val = it->second.get(term.prob);
        d.terms.push_back({term.sign, term.bound, *b, term.prob, val});
        if (!val) {
            d.reason = "no_digital";
            char buf[160];
            std::snprintf(buf, sizeof buf,
                          "no digital at boundary %.17g (missing quotes or no bracketing strikes)", *b);
            d.error = buf;
            return {MISSING, d};
        }
        total += term.sign * *val;
    }

    d.fair = clamp(total);
    return {d.fair, d};
}

double cbrt_py(double x) {
    // pow, NOT std::cbrt. See the header.
    return std::pow(x > 0.0 ? x : 0.0, 1.0 / 3.0);
}

OptD micro(OptD bid, OptD ask, double bid_size, double ask_size) {
    if (!bid || !ask) return MISSING;
    const double wb = cbrt_py(bid_size), wa = cbrt_py(ask_size);
    if (wb + wa == 0) return (*bid + *ask) / 2.0;
    return (*bid * wa + *ask * wb) / (wb + wa);
}

OptD quote_value(const Quote& q) {
    if (!q.present) return MISSING;                       // Python: `if not q`
    OptD bid = q.bp, ask = q.ap;
    if (!bid && !ask) return MISSING;
    if (!bid) bid = 0.0;                                  // absent bid means ZERO, not unknown
    if (!ask) return bid;                                 // one-sided: bid only
    return micro(bid, ask, q.bs.value_or(0.0), q.as.value_or(0.0));
}

bool two_sided(const Quote& q, bool require_ask_size) {
    if (!q.present) return false;                       // Python: `if not q`
    if (!q.bp || !q.ap) return false;
    if (q.bs.value_or(0.0) <= 0) return false;          // Python: `(q.get("bs") or 0) <= 0`
    return !require_ask_size || q.as.value_or(0.0) > 0;
}

Neighbors neighbors(double strike, const std::vector<double>& strikes) {
    Neighbors n;
    bool has_below = false, has_above = false;
    double below = 0.0, above = 0.0;
    for (double k : strikes) {
        if (k < strike) { if (!has_below || k > below) { below = k; has_below = true; } }
        if (k > strike) { if (!has_above || k < above) { above = k; has_above = true; } }
    }
    if (!has_below || !has_above) return n;               // both MISSING
    n.below = below;
    n.above = above;
    return n;
}

}  // namespace kp::pricing::theo
