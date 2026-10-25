#include "kp/pricing/pricer.hpp"

#include <algorithm>
#include <cmath>

#include "kp/pricing/bs.hpp"
#include "kp/pycompat/round.hpp"

namespace kp::pricing::pricer {

namespace {

const theo::Quote& leg(const skew::Board& b, char right, double strike) {
    static const theo::Quote EMPTY{};
    const auto it = b.find(skew::LegKey{right, strike});
    return (it == b.end()) ? EMPTY : it->second;
}

}  // namespace

Pricer::Pricer(skew::Board quotes, std::vector<double> strikes, OptD spot,
               std::optional<skew::SkewFit> skew_fit)
    : quotes_(std::move(quotes)), strikes_(std::move(strikes)),
      skew_(std::move(skew_fit)) {
    std::sort(strikes_.begin(), strikes_.end());
    strike_set_.insert(strikes_.begin(), strikes_.end());
    skew_built_ = skew_.has_value();
    // Python: `spot if spot is not None else self.parity_spot()`. parity_spot reads strikes_ and
    // quotes_, both of which are already set -- the ordering here is load-bearing.
    spot_ = spot ? spot : parity_spot();
}

Pricer& Pricer::use_skew(const skew::SkewFit& fit) {
    skew_ = fit;
    skew_built_ = true;
    return *this;
}

const skew::SkewFit* Pricer::skew() const {
    if (!skew_built_) {
        const OptD w_atm = skew::atm_vol(quotes_, strikes_, spot_);
        skew_ = skew::fit(quotes_, strikes_, spot_, w_atm);
        skew_built_ = true;
    }
    return skew_ ? &*skew_ : nullptr;
}

OptD Pricer::val(char right, double strike) const {
    return theo::quote_value(leg(quotes_, right, strike));
}

OptD Pricer::parity_spot(double width, bool strict) const {
    // (|C - P|, strike, implied S). Python appends tuples and takes min() over them, which is
    // lexicographic over ALL THREE elements -- so a tie on |C - P| is broken by the strike and
    // then by the implied spot, not by list order. std::min_element with the same comparator.
    struct Row { double d, k, s; };
    std::vector<Row> pairs;
    for (double k : strikes_) {
        if (strict && !(theo::two_sided(leg(quotes_, 'C', k)) &&
                        theo::two_sided(leg(quotes_, 'P', k))))
            continue;
        const OptD c = val('C', k), p = val('P', k);
        if (!c || !p) continue;
        pairs.push_back({std::fabs(*c - *p), k, k + *c - *p});
    }
    if (pairs.empty()) return MISSING;
    const Row& best = *std::min_element(
        pairs.begin(), pairs.end(), [](const Row& a, const Row& b) {
            if (a.d != b.d) return a.d < b.d;
            if (a.k != b.k) return a.k < b.k;
            return a.s < b.s;
        });
    const double anchor = best.s;
    std::vector<double> near;
    for (const Row& r : pairs)
        if (std::fabs(r.k - anchor) <= width) near.push_back(r.s);
    // statistics.median, not nth_element: on an even count it averages the two middles.
    return near.empty() ? anchor : kp::pycompat::py_median(near);
}

std::pair<OptD, ImpliedDetail> Pricer::implied_w_at(double strike) const {
    ImpliedDetail d;
    const OptD F = spot_;
    if (!F) return {MISSING, d};
    // Both are computed for visibility; only the OTM one is used.
    const OptD c = val('C', strike), pu = val('P', strike);
    if (c) d.w_call = bs::implied_w(*F, strike, c, 'C');
    if (pu) d.w_put = bs::implied_w(*F, strike, pu, 'P');

    const char otm = (strike >= *F) ? 'C' : 'P';
    const OptD w = (otm == 'C') ? d.w_call : d.w_put;
    if (!w) return {MISSING, d};
    d.w_leg = otm;
    d.w = w;
    return {w, d};
}

OptD Pricer::digital_above_delta(double strike) const {
    const OptD F = spot_;
    if (!F) return MISSING;
    OptD w;
    if (strike_set_.count(strike)) {
        w = implied_w_at(strike).first;
    } else {
        const theo::Neighbors n = theo::neighbors(strike, strikes_);
        if (!n.below) return MISSING;
        const OptD w_lo = implied_w_at(*n.below).first;
        const OptD w_hi = implied_w_at(*n.above).first;
        if (!w_lo && !w_hi) return MISSING;
        if (!w_lo)      w = w_hi;
        else if (!w_hi) w = w_lo;
        else {
            const double f = (strike - *n.below) / (*n.above - *n.below);
            w = *w_lo + f * (*w_hi - *w_lo);
        }
    }
    if (!w) return MISSING;
    return bs::p_above(*F, strike, *w);
}

OptD Pricer::digital_above(double strike) const {
    const skew::SkewFit* sf = skew();
    if (sf && sf->w_atm()) return sf->p_above(strike);
    // No ATM vol at all -- not even a flat curve can be served. Fall back to the per-strike
    // implied vol at this boundary, which is what priced the board before the curve existed.
    // Strictly better than refusing: it is the same N(d2), just unsmoothed.
    return digital_above_delta(strike);
}

std::pair<OptD, theo::BracketDetail> Pricer::market_fair(const theo::Market& m) const {
    auto [fair, d] = theo::bracket_fair(m.strike_type, m.floor_strike, m.cap_strike,
                                        [this](double k) { return digital_above(k); });
    return {fair, d};
}

}  // namespace kp::pricing::pricer
