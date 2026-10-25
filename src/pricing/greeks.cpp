#include "kp/pricing/greeks.hpp"
#include "kp/pricing/bs.hpp"

#include <cmath>
#include <map>

namespace kp::pricing::greeks {

namespace {
constexpr std::int64_t NS_PER_MIN = 60LL * 1000000000LL;
constexpr std::int64_t NS_PER_SEC = 1000000000LL;
}

OptD delta_per_point(OptD F, OptD K, OptD w) {
    if (!F || !K || !w || *F <= 0 || *K <= 0 || *w <= 0) return MISSING;
    const auto d = bs::d1_d2(*F, *K, *w);
    return bs::norm_pdf(d.d2) / (*F * *w);
}

OptD vega_per_volpoint(OptD F, OptD K, OptD w, OptD var_left_) {
    if (!F || !K || !w || *F <= 0 || *K <= 0 || *w <= 0) return MISSING;
    if (!var_left_ || *var_left_ <= 0) return MISSING;
    const auto d = bs::d1_d2(*F, *K, *w);
    const double dP_dw = bs::norm_pdf(d.d2) * (-std::log(*F / *K) / (*w * *w) - 0.5);
    return dP_dw * std::sqrt(*var_left_ / TRADING_DAYS) * 0.01;
}

// greeks.py:118-120 -- the same-day 09:30 ET, built from the ET date of `now`.
static kp::time::Instant session_open(const kp::time::Instant& now) {
    return kp::time::et_same_day(now, 9, 30, 0);
}

OptD var_left(const varcal::VarianceCalendar& cal, const kp::time::Instant& now,
              const kp::time::Instant& settlement, bool floor) {
    if (settlement <= now) return MISSING;
    const varcal::OptD ratio = cal.remaining_ratio(now, settlement);
    if (!ratio) return MISSING;                 // varcal's own refusal, not overridden here
    const double left = (1.0 - cal.phi(now)) * *ratio;
    if (!floor) return left;
    const double f = MIN_VAR_LEFT_FRACTION * (1.0 - cal.phi(session_open(now)));
    return (left > f) ? left : f;               // Python max()
}

OptD total_vol(OptD sigma_annualised, OptD var_left_units) {
    if (!sigma_annualised || !var_left_units || *var_left_units <= 0) return MISSING;
    return *sigma_annualised * std::sqrt(*var_left_units / TRADING_DAYS);
}

OptD theta_per_minute(OptD F, OptD K, OptD sigma_annualised, const kp::time::Instant& now_in,
                      const kp::time::Instant& settlement, const varcal::VarianceCalendar& cal) {
    kp::time::Instant now = now_in;
    const double gap = kp::time::total_seconds(now, settlement);
    if (gap <= 0) return MISSING;               // already settled: no next minute to decay through
    if (THETA_MIN_SECONDS > 0 && gap < THETA_MIN_SECONDS)
        now = {settlement.ns - (std::int64_t)(THETA_MIN_SECONDS * NS_PER_SEC)};

    const OptD vl0 = var_left(cal, now, settlement, /*floor=*/false);
    if (!vl0) return MISSING;
    const OptD w0 = total_vol(sigma_annualised, vl0);
    // Python: `bs.p_above(F, K, w0) if w0 else MISSING` -- 0.0 is falsy, so a zero w declines.
    const bs::OptD p0 = (w0 && *w0 != 0.0 && F && K) ? bs::p_above(*F, *K, *w0) : MISSING;
    if (!p0) return MISSING;

    const kp::time::Instant nxt{now.ns + NS_PER_MIN};
    if (nxt >= settlement) {
        // Inside the last minute the option is a spot comparison, not a distribution.
        if (!F || !K) return MISSING;
        const double p1 = (*F > *K) ? 1.0 : 0.0;
        return p1 - *p0;
    }
    const OptD vl1 = var_left(cal, nxt, settlement, /*floor=*/false);
    if (!vl1) return MISSING;
    const OptD w1 = total_vol(sigma_annualised, vl1);
    const bs::OptD p1 = (w1 && *w1 != 0.0 && F && K) ? bs::p_above(*F, *K, *w1) : MISSING;
    if (!p1) return MISSING;
    return *p1 - *p0;
}

theo::OneSided one_sided_greeks(OptD g_above) {
    theo::OneSided o;
    if (!g_above) return o;
    o.p[(int)theo::Prob::less]             = -*g_above;
    o.p[(int)theo::Prob::less_or_equal]    = -*g_above;
    o.p[(int)theo::Prob::greater]          =  *g_above;
    o.p[(int)theo::Prob::greater_or_equal] =  *g_above;
    return o;
}

OptD bracket_greek(const std::string& strike_type, OptD floor_strike, OptD cap_strike,
                   const std::function<OptD(double)>& g_at) {
    const std::vector<theo::Term>* rule = theo::strike_type_rule(strike_type);
    if (rule == nullptr) return MISSING;
    const theo::OptD lo = theo::normalize_boundary(floor_strike);
    const theo::OptD hi = theo::normalize_boundary(cap_strike);
    std::map<double, theo::OneSided> memo;
    double total = 0.0;
    for (const theo::Term& term : *rule) {
        const theo::OptD b = (term.bound == theo::Bound::floor_strike) ? lo : hi;
        if (!b) return MISSING;
        auto it = memo.find(*b);
        if (it == memo.end()) it = memo.emplace(*b, one_sided_greeks(g_at(*b))).first;
        const OptD v = it->second.get(term.prob);
        if (!v) return MISSING;
        total += term.sign * *v;
    }
    return total;   // NO clamp. See the header.
}

MarketGreeks market_greeks(const std::string& strike_type, OptD floor_strike, OptD cap_strike,
                           OptD F, OptD sigma_annualised, const kp::time::Instant& now,
                           const kp::time::Instant& settlement,
                           const varcal::VarianceCalendar& cal) {
    MarketGreeks out;
    out.sigma = sigma_annualised;
    out.forward = F;

    const OptD vl = var_left(cal, now, settlement);
    if (!vl) { out.reason = "no_variance_left"; return out; }
    const OptD w = total_vol(sigma_annualised, vl);
    if (!w || *w <= 0) { out.reason = "no_vol"; return out; }
    out.w = w;
    out.var_left = vl;

    const OptD d = bracket_greek(strike_type, floor_strike, cap_strike,
                                 [&](double k) { return delta_per_point(F, k, w); });
    const OptD v = bracket_greek(strike_type, floor_strike, cap_strike,
                                 [&](double k) { return vega_per_volpoint(F, k, w, vl); });
    const OptD t = bracket_greek(strike_type, floor_strike, cap_strike,
                                 [&](double k) { return theta_per_minute(F, k, sigma_annualised,
                                                                         now, settlement, cal); });
    out.delta = d; out.vega = v; out.theta = t;
    if (!d || !v || !t) { out.reason = "no_greek"; return out; }
    out.R = std::sqrt(*d * *d + *v * *v + *t * *t);
    return out;
}

}  // namespace kp::pricing::greeks
