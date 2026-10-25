#include "kp/strategy/skewstate.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace kp::strategy::skewstate {

namespace json = kp::json;

json::Value Detail::to_json() const {
    const auto ostr = [](const std::optional<std::string>& s) {
        return s ? json::Value::make_string(*s) : json::Value();
    };
    json::Value d;
    d.set("underlying", json::Value::make_string(underlying));
    d.set("skew_reason", ostr(skew_reason));
    d.set("skew_flat", json::Value::make_bool(skew_flat));
    d.set("n_fits", json::Value::make_int(n_fits));
    d.set("n_buckets_smoothed", json::Value::make_int(n_buckets_smoothed));
    d.set("n_buckets_raw", json::Value::make_int(n_buckets_raw));
    d.set("raw_reason", ostr(raw_reason));
    json::Value q = json::Value::make_object({});
    for (const auto& [k, n] : queue_depth) q.set(k, json::Value::make_int(n));
    d.set("queue_depth", q);
    d.set("window_s", json::Value::make_number(window_s));
    d.set("half_life_s", json::Value::make_number(half_life_s));
    d.set("recompute_s", json::Value::make_number(recompute_s));
    return d;
}

SkewState::SkewState(std::string underlying, OptD window_s, OptD recompute_s, OptD half_life_s)
    : underlying_(std::move(underlying)),
      window_s_(window_s.value_or(WINDOW_S)),
      recompute_s_(recompute_s.value_or(RECOMPUTE_S)),
      half_life_s_(half_life_s.value_or(HALF_LIFE_S)) {}

void SkewState::push(const skew::SkewFit& raw_fit, double t) {
    for (const auto& b : raw_fit.buckets()) queues_[b.label].emplace_back(t, b.ratio);
    prune(t);
}

void SkewState::prune(double t) {
    const double cutoff = t - window_s_;
    for (auto it = queues_.begin(); it != queues_.end();) {
        auto& q = it->second;
        while (!q.empty() && q.front().first < cutoff) q.pop_front();
        if (q.empty()) it = queues_.erase(it);
        else ++it;
    }
}

OptD SkewState::ema(const std::string& label, double t) const {
    const auto it = queues_.find(label);
    if (it == queues_.end() || it->second.empty() || (int)it->second.size() < MIN_SAMPLES)
        return std::nullopt;
    double num = 0.0, den = 0.0;
    for (const auto& [ts, r] : it->second) {
        const double wt = std::pow(0.5, (t - ts) / half_life_s_);
        num += wt * r;
        den += wt;
    }
    if (den > 0.0) return num / den;
    return std::nullopt;
}

skew::SkewFit SkewState::rebuild(OptD forward, OptD w_atm, double t,
                                 const std::vector<double>& strikes) {
    std::vector<std::pair<double, double>> pts;
    for (double x_nom : skew::bucket_offsets()) {
        const OptD r = ema(skew::bucket_label(x_nom), t);
        if (!r || !(skew::RATIO_MIN <= *r && *r <= skew::RATIO_MAX)) continue;
        pts.emplace_back(x_nom, *r);
    }
    if ((int)pts.size() < skew::MIN_BUCKETS) {
        reason_ = "too_few_smoothed_buckets";
        return skew::SkewFit(forward, w_atm, std::nullopt, {}, *reason_);
    }
    // pts.sort(): tuples, lexicographic. The x are distinct nominal knots, already ascending.
    std::stable_sort(pts.begin(), pts.end());
    std::vector<double> xs, ys;
    for (const auto& [x, r] : pts) { xs.push_back(x); ys.push_back(r); }
    skew::Spline sp(xs, ys);
    // `strike` is None on a smoothed bucket and `w` is None without an ATM vol; the C++ Bucket
    // carries doubles, so those are NaN. Nothing prices off either (only n_buckets is read).
    const double NaN = std::numeric_limits<double>::quiet_NaN();
    std::vector<skew::Bucket> buckets;
    for (const auto& [x, r] : pts) {
        skew::Bucket b;
        b.label = skew::bucket_label(x);
        b.x_nominal = x;
        b.x = x;
        b.strike = NaN;
        b.w = (w_atm && *w_atm != 0.0) ? *w_atm * r : NaN;
        b.ratio = r;
        buckets.push_back(b);
    }
    std::vector<double> ks = strikes;
    std::sort(ks.begin(), ks.end());
    skew::SkewFit out(forward, w_atm, sp, buckets, "", ks);
    if (!ks.empty() && !skew::ladder_monotone(out, ks)) {
        reason_ = "skew_non_monotone";
        return skew::SkewFit(forward, w_atm, std::nullopt, buckets, *reason_);
    }
    reason_.reset();
    return out;
}

const skew::SkewFit& SkewState::observe(const skew::Board& quotes,
                                        const std::vector<double>& strikes, OptD forward,
                                        OptD w_atm, double now, bool force) {
    const double t = now;
    const bool due = force || !last_fit_t_ || (t - *last_fit_t_) >= recompute_s_;
    if (due) {
        last_fit_t_ = t;
        raw_ = skew::fit(quotes, strikes, forward, w_atm);
        if (!raw_->buckets().empty()) {
            push(*raw_, t);
            n_fits_ += 1;
        } else {
            reason_ = raw_->reason().empty() ? std::string("raw_fit_empty") : raw_->reason();
        }
        prune(t);
    }
    smoothed_ = rebuild(forward, w_atm, t, strikes);
    return *smoothed_;
}

Detail SkewState::detail() const {
    Detail d;
    d.underlying = underlying_;
    d.skew_reason = reason_;
    d.skew_flat = smoothed_ ? smoothed_->flat() : true;
    d.n_fits = n_fits_;
    d.n_buckets_smoothed = smoothed_ ? (long)smoothed_->n_buckets() : 0;
    d.n_buckets_raw = raw_ ? (long)raw_->n_buckets() : 0;
    if (raw_ && !raw_->reason().empty()) d.raw_reason = raw_->reason();
    for (const auto& [k, q] : queues_) d.queue_depth.emplace_back(k, (long)q.size());
    d.window_s = window_s_;
    d.half_life_s = half_life_s_;
    d.recompute_s = recompute_s_;
    return d;
}

Observer observer(SkewState& state, double now) {
    return [&state, now](const skew::Board& q, const std::vector<double>& k, OptD fwd, OptD wa) {
        return state.observe(q, k, fwd, wa, now);
    };
}

}  // namespace kp::strategy::skewstate
