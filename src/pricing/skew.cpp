#include "kp/pricing/skew.hpp"

#include "kp/config.hpp"
#include "kp/pricing/bs.hpp"
#include "kp/pycompat/round.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

namespace kp::pricing::skew {

namespace pc = kp::pycompat;

std::vector<double> bucket_offsets(double step, double xmax) {
    const int n = (int)pc::py_round_i(xmax / step);
    std::vector<double> out;
    out.reserve((std::size_t)(2 * n + 1));
    for (int i = -n; i <= n; ++i) out.push_back(pc::py_round_nd(i * step, 10));
    return out;
}

std::string bucket_label(double x) {
    if (std::fabs(x) < 1e-12) return "atm";
    char b[32];
    // Python f"{x:+.1f}" -- explicit sign, one decimal.
    std::snprintf(b, sizeof b, "%+.1f", x);
    return b;
}

LegVol leg_vol(double F, double K, char right, const theo::Quote& q, double w_ref) {
    if (!q.present) return {MISSING, 0.0};                 // Python: `if not q`
    if (!q.bp || !q.ap || *q.ap < *q.bp) return {MISSING, 0.0};
    const theo::OptD price = theo::quote_value(q);
    if (!price) return {MISSING, 0.0};
    const bs::OptD w = bs::implied_w(F, K, price, right);
    if (!w || *w <= 0.0) return {MISSING, 0.0};
    const double vega = bs::vega_w(F, K, w_ref);
    if (vega <= 0.0) return {MISSING, 0.0};
    // Floored, not rejected, at zero width. A synthetic or crossed-to-touching quote has no width
    // to divide by, and the honest reading of a zero-wide market is "maximally informative", not
    // "uninformative" -- rejecting it would make an exact board unfittable.
    const double raw = (*q.ap - *q.bp) / vega;
    const double width_vol = (raw > MIN_WIDTH_VOL) ? raw : MIN_WIDTH_VOL;   // Python max()
    return {w, 1.0 / width_vol};
}

std::pair<OptD, StrikeVolDetail> strike_vol(const Board& quotes, double F, double K, double w_ref) {
    StrikeVolDetail d;
    static const theo::Quote EMPTY{};
    auto get = [&](char r) -> const theo::Quote& {
        auto it = quotes.find({r, K});
        return it == quotes.end() ? EMPTY : it->second;
    };
    const LegVol c = leg_vol(F, K, 'C', get('C'), w_ref);
    const LegVol p = leg_vol(F, K, 'P', get('P'), w_ref);
    d.w_call = c.w; d.w_put = p.w; d.wt_call = c.weight; d.wt_put = p.weight;
    const double tot = c.weight + p.weight;
    if (tot <= 0.0) return {MISSING, d};
    double acc = 0.0;
    if (c.w) acc += *c.w * c.weight;
    if (p.w) acc += *p.w * p.weight;
    d.w = acc / tot;
    return {d.w, d};
}

// --- Spline: natural cubic, Thomas algorithm --------------------------------------------
//
// Transcribed operation-for-operation from pricing/skew.py:151-197. The loop shape and the order
// of the arithmetic are the specification: with -ffp-contract=off the four multiply-adds in the
// evaluation below round twice each, exactly as the Python does.
Spline::Spline(std::vector<double> x, std::vector<double> y)
    : x_(std::move(x)), y_(std::move(y)) {
    const std::size_t n = x_.size();
    if (n < 3) { m_.assign(n, 0.0); return; }
    std::vector<double> h(n - 1);
    for (std::size_t i = 0; i + 1 < n; ++i) h[i] = x_[i + 1] - x_[i];
    std::vector<double> alpha(n, 0.0);
    for (std::size_t i = 1; i + 1 < n; ++i)
        alpha[i] = 3.0 * ((y_[i + 1] - y_[i]) / h[i] - (y_[i] - y_[i - 1]) / h[i - 1]);
    std::vector<double> l(n, 0.0), mu(n, 0.0), z(n, 0.0);
    l[0] = 1.0;
    for (std::size_t i = 1; i + 1 < n; ++i) {
        l[i]  = 2.0 * (x_[i + 1] - x_[i - 1]) - h[i - 1] * mu[i - 1];
        mu[i] = h[i] / l[i];
        z[i]  = (alpha[i] - h[i - 1] * z[i - 1]) / l[i];
    }
    std::vector<double> c(n, 0.0);
    for (std::size_t k = n - 1; k-- > 0;) c[k] = z[k] - mu[k] * c[k + 1];
    m_ = c;
}

OptD Spline::operator()(double xq) const {
    const std::size_t n = x_.size();
    if (n == 0) return MISSING;
    if (n == 1) return y_[0];
    if (xq <= x_[0])     return y_[0];        // flat extrapolation
    if (xq >= x_.back()) return y_.back();    // flat extrapolation
    std::size_t lo = 0, hi = n - 1;
    while (hi - lo > 1) {
        const std::size_t mid = (lo + hi) / 2;
        if (x_[mid] <= xq) lo = mid; else hi = mid;
    }
    const double h = x_[lo + 1] - x_[lo];
    if (n < 3) {
        const double f = (xq - x_[lo]) / h;
        return y_[lo] + f * (y_[lo + 1] - y_[lo]);
    }
    const double b  = (y_[lo + 1] - y_[lo]) / h - h * (2.0 * m_[lo] + m_[lo + 1]) / 3.0;
    const double dd = (m_[lo + 1] - m_[lo]) / (3.0 * h);
    const double dx = xq - x_[lo];
    return y_[lo] + b * dx + m_[lo] * dx * dx + dd * dx * dx * dx;
}

// --- SkewFit ------------------------------------------------------------------------------
SkewFit::SkewFit(OptD forward, OptD w_atm, std::optional<Spline> spline,
                 std::vector<Bucket> buckets, std::string reason,
                 const std::vector<double>& strikes)
    : forward_(forward), w_atm_(w_atm), spline_(std::move(spline)),
      buckets_(std::move(buckets)), reason_(std::move(reason)) {
    // Python: `tuple(sorted(float(k) for k in strikes)) if strikes else ()`.
    strikes_ = strikes;
    std::sort(strikes_.begin(), strikes_.end());
}

std::vector<double> pava_nonincreasing(const std::vector<double>& y) {
    const std::size_t n = y.size();
    std::vector<double> val(n, 0.0), wt(n, 0.0);
    std::size_t idx = 0;
    for (double v : y) {
        val[idx] = v;
        wt[idx] = 1.0;
        ++idx;
        while (idx > 1 && val[idx - 2] < val[idx - 1]) {
            const double w = wt[idx - 2] + wt[idx - 1];
            val[idx - 2] = (wt[idx - 2] * val[idx - 2] + wt[idx - 1] * val[idx - 1]) / w;
            wt[idx - 2] = w;
            --idx;
        }
    }
    std::vector<double> out;
    out.reserve(n);
    for (std::size_t k = 0; k < idx; ++k)
        out.insert(out.end(), (std::size_t)(long long)wt[k], val[k]);   // Python int(wt[k])
    return out;
}

double interp(double x, const std::vector<double>& xs, const std::vector<double>& ys) {
    if (xs.empty()) return 0.0;
    if (x <= xs.front()) return ys.front();
    if (x >= xs.back()) return ys.back();
    std::size_t lo = 0, hi = xs.size() - 1;
    while (hi - lo > 1) {
        const std::size_t mid = (lo + hi) / 2;       // Python // on non-negative ints
        if (xs[mid] <= x) lo = mid;
        else              hi = mid;
    }
    const double span = xs[hi] - xs[lo];
    if (span <= 0.0) return ys[lo];
    return ys[lo] + (x - xs[lo]) / span * (ys[hi] - ys[lo]);
}

OptD SkewFit::x_of(double K) const {
    // Python: `if not self.forward or K <= 0.0 or not self.w_atm` -- 0.0 is falsy, so a zero
    // forward or a zero w_atm reads as absent. Spell it out rather than using has_value().
    if (!forward_ || *forward_ == 0.0 || K <= 0.0 || !w_atm_ || *w_atm_ == 0.0) return MISSING;
    return std::log(K / *forward_) / *w_atm_;
}

double SkewFit::ratio(double K) const {
    if (flat()) return 1.0;
    const OptD x = x_of(K);
    if (!x) return 1.0;
    const OptD r = (*spline_)(*x);
    if (!r || !(RATIO_MIN <= *r && *r <= RATIO_MAX)) return 1.0;
    return *r;
}

OptD SkewFit::w_of(double K) const {
    if (!w_atm_) return MISSING;
    return *w_atm_ * ratio(K);
}

double SkewFit::dratio_dK(double K, double step_x) const {
    // Python: `if self.flat or not self.forward or not self.w_atm or K <= 0.0` -- 0.0 is falsy.
    if (flat() || !forward_ || *forward_ == 0.0 || !w_atm_ || *w_atm_ == 0.0 || K <= 0.0)
        return 0.0;
    const OptD x = x_of(K);
    if (!x) return 0.0;
    const double ka = *forward_ * std::exp((*x - step_x) * *w_atm_);
    const double kb = *forward_ * std::exp((*x + step_x) * *w_atm_);
    if (kb <= ka) return 0.0;
    return (ratio(kb) - ratio(ka)) / (kb - ka);
}

double SkewFit::slope_term(double K, OptD w_atm) const {
    const OptD wa = w_atm ? w_atm : w_atm_;
    if (flat() || !wa || *wa <= 0.0 || !forward_ || *forward_ == 0.0 || K <= 0.0) return 0.0;
    const double w_k = *wa * ratio(K);
    if (w_k <= 0.0) return 0.0;
    const double d1 = std::log(*forward_ / K) / w_k + w_k / 2.0;
    return -*forward_ * bs::norm_pdf(d1) * (*wa * dratio_dK(K));
}

OptD SkewFit::p_above_flatvol(double K) const {
    const OptD w = w_of(K);
    if (!w) return MISSING;
    if (!forward_) return MISSING;
    return bs::p_above(*forward_, K, *w);
}

SkewFit::Projection SkewFit::projection(double wa) const {
    // Python keys the memo on `round(float(wa), 12)`.
    const double key = kp::pycompat::py_round_nd(wa, 12);
    auto it = proj_.find(key);
    if (it != proj_.end()) return {&it->second.first, &it->second.second};

    auto store = [&](std::vector<double> ks, std::vector<double> adj) {
        auto r = proj_.emplace(key, std::make_pair(std::move(ks), std::move(adj)));
        return Projection{&r.first->second.first, &r.first->second.second};
    };
    if (flat() || strikes_.empty()) return store({}, {});

    const double lo = *forward_ * std::exp(-PROJECTION_XMAX * *w_atm_);
    const double hi = *forward_ * std::exp(PROJECTION_XMAX * *w_atm_);
    std::vector<double> ks;
    for (double k : strikes_) if (lo <= k && k <= hi) ks.push_back(k);
    if (ks.size() < 3) return store({}, {});

    std::vector<double> raw;
    raw.reserve(ks.size());
    for (double k : ks) {
        const double r = ratio(k);
        const double w_k = wa * r;
        // Python's conditional expression binds the whole sum, so w_k <= 0 yields a bare 0.0.
        raw.push_back(w_k > 0.0
                      ? (*bs::p_above(*forward_, k, w_k) + slope_term(k, wa))
                      : 0.0);
    }
    const std::vector<double> proj = pava_nonincreasing(raw);
    std::vector<double> adj(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) adj[i] = proj[i] - raw[i];
    return store(std::move(ks), std::move(adj));
}

OptD SkewFit::p_above_at(double K, OptD w_atm) const {
    const OptD wa = w_atm ? w_atm : w_atm_;
    if (!wa) return MISSING;
    const double r = ratio(K);
    const double w_k = *wa * r;
    if (w_k <= 0.0) return MISSING;
    const OptD p0 = bs::p_above(*forward_, K, w_k);
    if (!kp::config::DIGITAL_SLOPE_TERM || flat()) return p0;
    double p = *p0 + slope_term(K, wa);
    const Projection pr = projection(*wa);
    if (!pr.ks->empty()) p += interp(K, *pr.ks, *pr.adj);
    // Python `min(1.0, max(0.0, p))`, spelled out: std::clamp differs on NaN and signed zero.
    const double lo = (0.0 > p) ? 0.0 : p;
    return (1.0 < lo) ? 1.0 : lo;
}

OptD SkewFit::p_above(double K) const { return p_above_at(K); }

OptD atm_vol(const Board& quotes, const std::vector<double>& strikes, OptD F, int max_steps) {
    if (strikes.empty() || !F || *F == 0.0) return MISSING;
    std::vector<double> ks = strikes;
    std::sort(ks.begin(), ks.end());
    // Python's min(range(...), key=...) returns the FIRST minimum; std::min_element does too.
    std::size_t i0 = 0;
    double best = std::fabs(ks[0] - *F);
    for (std::size_t i = 1; i < ks.size(); ++i) {
        const double d = std::fabs(ks[i] - *F);
        if (d < best) { best = d; i0 = i; }
    }
    for (int step = 0; step <= max_steps; ++step) {
        // A LIST ordered by distance from the forward, not a set.
        //
        // Until 2026-08-24 the Python was `for i in ({i0} if step == 0 else {i0-step, i0+step})`
        // -- a set literal, so when both neighbours inverted, which one won came out of CPython's
        // hash order (ascending by `x & 7`, since ints hash to themselves and a set under
        // PySet_MINSIZE never resizes). This port emulated that exactly, and the emulation was
        // correct. It is now WRONG, because upstream fixed the underlying bug: the order was
        // arbitrary but stable within a process, so it never looked random -- it just silently
        // favoured one wing.
        //
        // The strike grid is uneven (5-wide near the money, up to 200-wide in the tails), so the
        // two candidates are equidistant in INDEX and not in price. The nearer one in price is
        // the better ATM proxy and is tried first; `i` breaks exact ties deterministically.
        //
        // NOTE the filter runs BEFORE the sort, matching the Python. Sorting first and
        // range-checking inside the loop is the same answer here, but only because the
        // out-of-range index is never dereferenced -- not a property worth relying on.
        std::vector<long long> cand;
        if (step == 0) {
            cand.push_back((long long)i0);
        } else {
            for (long long i : {(long long)i0 - step, (long long)i0 + step})
                if (i >= 0 && i < (long long)ks.size()) cand.push_back(i);
        }
        std::stable_sort(cand.begin(), cand.end(), [&](long long a, long long b) {
            const double da = std::fabs(ks[(std::size_t)a] - *F);
            const double db = std::fabs(ks[(std::size_t)b] - *F);
            if (da != db) return da < db;
            return a < b;
        });
        for (long long i : cand) {
            const auto [w, _d] = strike_vol(quotes, *F, ks[(std::size_t)i], W_PROBE);
            if (w) return w;
        }
    }
    return MISSING;
}

SkewFit fit(const Board& quotes, std::vector<double> strikes, OptD forward, OptD w_atm,
            double step, double xmax, int min_buckets) {
    std::sort(strikes.begin(), strikes.end());
    if (strikes.empty() || !forward || *forward == 0.0 || !w_atm || *w_atm <= 0.0)
        return SkewFit(forward, w_atm, std::nullopt, {}, "no_atm_fit");

    const double F = *forward, wa = *w_atm;
    std::vector<Bucket> buckets;
    std::set<double> seen;
    for (double x_nom : bucket_offsets(step, xmax)) {
        const double target = F * std::exp(x_nom * wa);
        // Python's min(strikes, key=...) returns the FIRST minimum.
        double K = strikes[0];
        double best = std::fabs(strikes[0] - target);
        for (std::size_t i = 1; i < strikes.size(); ++i) {
            const double d = std::fabs(strikes[i] - target);
            if (d < best) { best = d; K = strikes[i]; }
        }
        // Two knots landed on the same listed strike -- the grid is coarser than the bucket
        // spacing here. Taking it twice would double-weight one quote and, worse, pin the spline
        // to two different x for one observation.
        if (seen.count(K)) continue;
        const auto [w, d] = strike_vol(quotes, F, K, wa);
        if (!w) continue;
        const double r = *w / wa;
        if (!(RATIO_MIN <= r && r <= RATIO_MAX)) continue;
        seen.insert(K);
        // The abscissa is the strike's ACTUAL position in sigmas, not the knot's nominal one. The
        // vol was observed at K, so that is where it belongs on the curve; the nominal label
        // survives only as the bucket's identity for the live queue.
        buckets.push_back({bucket_label(x_nom), x_nom, std::log(K / F) / wa, K, *w, r, d});
    }

    if ((int)buckets.size() < min_buckets)
        return SkewFit(forward, w_atm, std::nullopt, buckets, "too_few_buckets");

    // STABLE, as Python's list.sort. Two buckets can share an x only if two knots resolved to the
    // same strike, which `seen` already prevents -- but stability is free and the rule is global.
    std::stable_sort(buckets.begin(), buckets.end(),
                     [](const Bucket& a, const Bucket& b) { return a.x < b.x; });
    std::vector<double> xs, ys;
    xs.reserve(buckets.size()); ys.reserve(buckets.size());
    for (const auto& b : buckets) { xs.push_back(b.x); ys.push_back(b.ratio); }
    // strikes= is load-bearing. Without it projection() short-circuits and the monotone repair
    // never runs -- which is exactly the defect the live SkewState path had on 2026-08-24: the one
    // and only curve the live pricer installed was built without it, and a negative bracket does
    // not fail loudly (bracket_fair ends in clamp(), so the rung silently becomes 0.00).
    SkewFit out(forward, w_atm, Spline(xs, ys), buckets, "", strikes);

    if (!ladder_monotone(out, strikes))
        return SkewFit(forward, w_atm, std::nullopt, buckets, "skew_non_monotone");
    return out;
}

bool ladder_monotone(const SkewFit& sf, const std::vector<double>& strikes, double tol) {
    // DELIBERATELY p_above_flatvol, not the corrected digital. Two reasons: it keeps the set of
    // boards that accept a fit identical to what it was before the slope term existed, so the
    // before/after comparison changes exactly one thing; and the corrected ladder is monotone BY
    // CONSTRUCTION after projection(), so running the guard on it would test nothing.
    OptD prev;
    for (double K : strikes) {
        const OptD p = sf.p_above_flatvol(K);
        if (!p) continue;
        if (prev && *p > *prev + tol) return false;
        prev = p;
    }
    return true;
}

}  // namespace kp::pricing::skew
