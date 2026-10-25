#include "kp/strategy/band.hpp"

#include <cmath>

#include "kp/config.hpp"
#include "kp/pycompat/round.hpp"

namespace kp::strategy::band {

namespace th = kp::pricing::theo;

namespace {

std::vector<std::string> default_counts() {
    std::vector<std::string> v;
    for (const auto& s : kp::config::QUOTE_TIER_COUNTS) v.emplace_back(s);
    return v;
}

}  // namespace

OptD market_strike(const th::Market& m) {
    const std::vector<th::Term>* terms = th::strike_type_rule(m.strike_type);
    if (terms == nullptr || terms->empty()) return MISSING;
    if (m.strike_type == BRACKET_TYPE) {
        const OptD lo = th::normalize_boundary(m.floor_strike);
        const OptD hi = th::normalize_boundary(m.cap_strike);
        if (!lo || !hi) return MISSING;
        return 0.5 * (*lo + *hi);
    }
    return th::normalize_boundary((*terms)[0].bound == th::Bound::floor_strike
                                  ? m.floor_strike : m.cap_strike);
}

OptD band_width(OptD forward, OptD w, OptD sigma) {
    if (!forward || !w || *forward <= 0.0 || *w <= 0.0) return MISSING;
    const double s = sigma.value_or(kp::config::BAND_SIGMA);
    return s * *w * *forward;
}

OptD distance(const th::Market& m, OptD forward) {
    const OptD k = market_strike(m);
    if (!k || !forward) return MISSING;
    return std::fabs(*k - *forward);
}

std::pair<bool, BandDetail> in_band(const th::Market& m, OptD forward, OptD w, OptD sigma,
                                    bool incumbent, OptD exit_mult) {
    const double em = exit_mult.value_or(kp::config::BAND_EXIT_MULT);
    const OptD d = distance(m, forward);
    const OptD half = band_width(forward, w, sigma);
    BandDetail det;
    if (!d || !half) {
        det.reason = !d ? "no_strike" : "no_width";
        det.distance = d;
        det.half_width = half;
        return {false, det};
    }
    const double limit = *half * (incumbent ? em : 1.0);
    const bool ok = *d <= limit + 1e-9;
    // `sigmas` is the market's actual moneyness in standard deviations -- d / (w*F) -- NOT its
    // position within the band. The distinction matters in a log: "1.4 sigma out" is a fact about
    // the market, while "0.7 of the way to the edge" changes meaning whenever BAND_SIGMA is tuned.
    const double one_sigma = *half / sigma.value_or(kp::config::BAND_SIGMA);
    det.has_detail = true;
    det.distance = kp::pycompat::py_round_nd(*d, 4);
    det.half_width = kp::pycompat::py_round_nd(*half, 4);
    det.limit = kp::pycompat::py_round_nd(limit, 4);
    // Python guards these with `if one_sigma` / `if half`, i.e. a truthiness test that is false
    // only at exactly 0.0 -- which band_width has already excluded, but carried across anyway.
    det.sigmas = (one_sigma != 0.0) ? OptD(kp::pycompat::py_round_nd(*d / one_sigma, 3)) : MISSING;
    det.band_frac = (*half != 0.0) ? OptD(kp::pycompat::py_round_nd(*d / *half, 3)) : MISSING;
    det.incumbent = incumbent;
    return {ok, det};
}

std::vector<std::string> counts_for(bool inband, const std::vector<std::string>* counts) {
    const std::vector<std::string> c =
        (counts != nullptr && !counts->empty()) ? *counts : default_counts();
    if (inband) return c;
    return c.empty() ? c : std::vector<std::string>{c.front()};
}

double position_limit(bool inband, std::optional<int> lot) {
    const int l = lot.value_or(kp::config::LOT_SIZE);
    const int lots = inband ? kp::config::POSITION_LIMIT_LOTS_BAND
                            : kp::config::POSITION_LIMIT_LOTS_WING;
    return (double)lots * (double)l;
}

std::pair<std::map<std::string, Classified>, BandSummary> classify(
        const std::vector<th::Market>& markets, OptD forward,
        const std::map<std::string, OptD>& em_w,
        const std::set<std::string>& incumbents, OptD sigma) {
    std::map<std::string, Classified> out;
    long n_band = 0;
    for (const auto& m : markets) {
        const auto it = em_w.find(m.ticker);
        const OptD w = (it != em_w.end()) ? it->second : MISSING;
        const auto [ok, det] = in_band(m, forward, w, sigma, incumbents.count(m.ticker) != 0);
        if (ok) ++n_band;
        Classified c;
        c.inband = ok;
        c.counts = counts_for(ok);
        c.position_limit = position_limit(ok);
        c.detail = det;
        out[m.ticker] = std::move(c);
    }
    BandSummary s;
    s.n = (long)out.size();
    s.band = n_band;
    s.wing = s.n - n_band;
    s.sigma = sigma.value_or(kp::config::BAND_SIGMA);
    s.exit_mult = kp::config::BAND_EXIT_MULT;
    return {std::move(out), s};
}

std::vector<std::string> resolve_counts(
        const std::map<std::string, std::vector<std::string>>* per_ticker,
        const std::vector<std::string>* global_counts, const std::string& ticker,
        const std::vector<std::string>* dflt) {
    const std::vector<std::string> d =
        (dflt != nullptr && !dflt->empty()) ? *dflt : default_counts();
    if (per_ticker != nullptr) {
        const auto it = per_ticker->find(ticker);
        if (it != per_ticker->end() && !it->second.empty()) return it->second;
        return d;
    }
    if (global_counts != nullptr && !global_counts->empty()) return *global_counts;
    return d;
}

}  // namespace kp::strategy::band
