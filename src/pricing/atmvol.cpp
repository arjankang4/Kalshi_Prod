#include "kp/pricing/atmvol.hpp"
#include "kp/pricing/bs.hpp"
#include "kp/pycompat/round.hpp"

#include <algorithm>
#include <cmath>

namespace kp::pricing::atmvol {

namespace pc = kp::pycompat;

// Delegates, exactly as pricing.atmvol.two_sided does. One predicate, one place: three copies is
// what let the live parity path miss the guard the other two carried.
bool two_sided(const theo::Quote& q, bool require_ask_size) {
    return theo::two_sided(q, require_ask_size);
}

std::vector<Combo> combos(const Board& quotes, const std::vector<double>& strikes) {
    std::vector<Combo> out;
    static const theo::Quote EMPTY{};
    for (double k : strikes) {
        auto itc = quotes.find({'C', k});
        auto itp = quotes.find({'P', k});
        const theo::Quote& qc = (itc == quotes.end()) ? EMPTY : itc->second;
        const theo::Quote& qp = (itp == quotes.end()) ? EMPTY : itp->second;
        if (!(atmvol::two_sided(qc) && atmvol::two_sided(qp))) continue;
        const theo::OptD c = theo::quote_value(qc), p = theo::quote_value(qp);
        if (!c || !p) continue;
        out.push_back({std::fabs(*c - *p), k, k + *c - *p, *c, *p});
    }
    return out;
}

namespace {

// Python's `cs.sort()` on 5-tuples is LEXICOGRAPHIC OVER ALL FIVE ELEMENTS, not a sort on the
// first. Ties in |C - P| are common -- the field is a difference of two cent-grid prices, so it
// takes only a handful of values near the money -- and the tie-break then decides the anchor
// strike, hence the forward, hence every theo on the board.
bool combo_lt(const Combo& a, const Combo& b) {
    if (a.abs_cp    != b.abs_cp)    return a.abs_cp    < b.abs_cp;
    if (a.strike    != b.strike)    return a.strike    < b.strike;
    if (a.implied_s != b.implied_s) return a.implied_s < b.implied_s;
    if (a.c         != b.c)         return a.c         < b.c;
    return a.p < b.p;
}

}  // namespace

double grid_step(std::vector<double> strikes, double centre) {
    // STABLE, as Python's sorted(). On a symmetric grid |k - centre| ties for every pair
    // straddling the centre -- measured, 29 of 31 distinct keys tie -- so stability decides which
    // member of each pair lands inside the 25-strike cut.
    std::stable_sort(strikes.begin(), strikes.end(),
                     [&](double a, double b) { return std::fabs(a - centre) < std::fabs(b - centre); });
    if (strikes.size() > (std::size_t)PARITY_STRIKES) strikes.resize((std::size_t)PARITY_STRIKES);
    std::sort(strikes.begin(), strikes.end());
    double best = 0.0;
    bool any = false;
    for (std::size_t i = 0; i + 1 < strikes.size(); ++i) {
        const double g = strikes[i + 1] - strikes[i];
        if (strikes[i + 1] > strikes[i] && (!any || g < best)) { best = g; any = true; }
    }
    return any ? best : 1.0;
}

std::pair<OptD, ParityDetail> parity_forward(const Board& quotes,
                                             const std::vector<double>& strikes) {
    ParityDetail d;
    std::vector<Combo> cs = combos(quotes, strikes);
    if (cs.empty()) { d.reason = "no_two_sided_strikes"; d.n_parity = 0; return {MISSING, d}; }
    std::sort(cs.begin(), cs.end(), combo_lt);          // full lexicographic, see combo_lt
    const double anchor_cp = cs[0].abs_cp, anchor_k = cs[0].strike, naive_F = cs[0].implied_s;

    std::vector<Combo> near = cs;
    std::stable_sort(near.begin(), near.end(), [&](const Combo& a, const Combo& b) {
        return std::fabs(a.strike - naive_F) < std::fabs(b.strike - naive_F);
    });
    if (near.size() > (std::size_t)PARITY_STRIKES) near.resize((std::size_t)PARITY_STRIKES);

    std::vector<double> ks;
    ks.reserve(cs.size());
    for (const auto& r : cs) ks.push_back(r.strike);
    const double step = grid_step(ks, naive_F);

    double num = 0.0, den = 0.0;
    for (const auto& r : near) {
        const double raw = std::fabs(r.strike - naive_F) / step;
        const double base = (raw > 0.5) ? raw : 0.5;    // Python max()
        // `** 0.5`, which is pow -- NOT sqrt. They differ on ~0.086% of values.
        const double w = 1.0 / std::pow(base, 0.5);
        num += w * r.implied_s;
        den += w;
    }
    if (den <= 0) { d.reason = "no_parity_weight"; d.n_parity = 0; return {MISSING, d}; }
    d.reason.clear();
    d.anchor_strike = anchor_k;
    d.naive_forward = naive_F;
    d.n_parity = (int)near.size();
    d.anchor_cp = anchor_cp;
    return {num / den, d};
}

std::pair<OptD, VolDetail> atm_total_vol(const Board& quotes, const std::vector<double>& strikes,
                                         double forward, int k) {
    VolDetail d;
    std::vector<Combo> cs = combos(quotes, strikes);
    if (cs.empty()) { d.reason = "no_two_sided_strikes"; return {MISSING, d}; }
    std::sort(cs.begin(), cs.end(), combo_lt);          // by |C - P| -- most ATM first

    std::vector<double> per_strike, spreads;
    const std::size_t lim = std::min((std::size_t)std::max(k, 0), cs.size());
    for (std::size_t i = 0; i < lim; ++i) {
        const Combo& r = cs[i];
        const bs::OptD wc = bs::implied_w(forward, r.strike, r.c, 'C');
        const bs::OptD wp = bs::implied_w(forward, r.strike, r.p, 'P');
        int n = 0; double sum = 0.0;
        if (wc) { sum += *wc; ++n; }
        if (wp) { sum += *wp; ++n; }
        if (n == 0) continue;
        per_strike.push_back(sum / n);
        d.strikes_used.push_back(r.strike);
        if (wc && wp) {
            const double mid = 0.5 * (*wc + *wp);
            spreads.push_back(mid > 0 ? std::fabs(*wc - *wp) / mid : 0.0);
        }
    }

    if (per_strike.empty()) { d.reason = "no_invertible_atm_quote"; return {MISSING, d}; }

    const double w = pc::py_median(per_strike);
    if (w <= 0) { d.reason = "degenerate_vol"; d.n_vol = (int)per_strike.size(); return {MISSING, d}; }
    double disp = 0.0;
    if (per_strike.size() > 1) {
        const auto mm = std::minmax_element(per_strike.begin(), per_strike.end());
        disp = (*mm.second - *mm.first) / w;
    }
    const double cp = spreads.empty() ? 0.0 : *std::max_element(spreads.begin(), spreads.end());
    d.reason.clear();
    d.n_vol = (int)per_strike.size();
    d.cp_spread = cp;
    d.dispersion = disp;
    d.w_anchor = per_strike[0];
    return {w, d};
}

AtmFit fit(const Board& quotes, const std::vector<double>& strikes, bool gate, OptD index_forward) {
    const auto [F_par, fd] = parity_forward(quotes, strikes);

    double F;
    std::string src;
    if (F_par) { F = *F_par; src = "parity"; }
    else if (index_forward) {
        // Parity could not price itself; the index at least locates the board. The vol backed out
        // against it is still suspect, so this keeps parity's reason rather than clearing it.
        F = *index_forward; src = "index_fallback";
    } else {
        AtmFit bad;
        bad.reason = fd.reason;
        bad.n_parity = fd.n_parity;
        return bad;
    }

    AtmFit out;
    out.index_minus_parity = (index_forward && F_par) ? OptD(*index_forward - *F_par) : MISSING;

    const auto [w, wd] = atm_total_vol(quotes, strikes, F);
    out.forward = F;
    out.w = w;
    out.reason = !wd.reason.empty() ? wd.reason : (src == "index_fallback" ? fd.reason : std::string());
    out.forward_source = src;
    out.parity_forward_ = F_par;
    out.anchor_strike = fd.anchor_strike;
    out.naive_forward = fd.naive_forward;
    out.n_parity = fd.n_parity;
    out.n_vol = wd.n_vol;
    out.cp_spread = wd.cp_spread;
    out.dispersion = wd.dispersion;
    out.w_anchor = wd.w_anchor;
    out.strikes_used = wd.strikes_used;

    if (!w || !gate) return out;
    // Gate AFTER recording the numbers, so a rejected fit is still inspectable.
    if (out.cp_spread && *out.cp_spread > MAX_CP_VOL_SPREAD)        out.reason = "cp_vol_spread";
    else if (out.dispersion && *out.dispersion > MAX_VOL_DISPERSION) out.reason = "atm_vol_dispersion";
    return out;
}

}  // namespace kp::pricing::atmvol
