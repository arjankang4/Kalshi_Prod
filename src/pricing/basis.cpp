#include "kp/pricing/basis.hpp"

#include <cmath>
#include <cstdio>

#include "kp/pycompat/round.hpp"

namespace kp::pricing::basis {

namespace json = kp::json;

namespace {

json::Value num(const OptD& v) { return v ? json::Value::make_number(*v) : json::Value(); }
json::Value str(const std::optional<std::string>& s) {
    return s ? json::Value::make_string(*s) : json::Value();
}

// Python's max(a, b): b only when b > a, so a NaN or a tie keeps the first argument.
double py_max(double a, double b) { return b > a ? b : a; }

}  // namespace

json::Value SpotDetail::to_json() const {
    json::Value d;
    d.set("underlying", json::Value::make_string(underlying));
    d.set("basis", num(basis));
    d.set("n", json::Value::make_int(n));
    d.set("residual", num(residual));
    d.set("reason", str(reason));
    d.set("alarm", json::Value::make_bool(alarm));
    if (spot) d.set("spot", json::Value::make_number(*spot));
    return d;
}

json::Value TrackerDetail::to_json() const {
    json::Value d;
    d.set("underlying", json::Value::make_string(underlying));
    d.set("basis", num(basis));
    d.set("n", json::Value::make_int(n));
    d.set("half_life_s", json::Value::make_number(half_life_s));
    d.set("ready", json::Value::make_bool(ready));
    d.set("last_residual", num(last_residual));
    d.set("max_abs_residual", json::Value::make_number(max_abs_residual));
    d.set("alarm_pct", json::Value::make_number(alarm_pct));
    d.set("alarm_points", num(alarm_points));
    json::Value r = json::Value::make_object({});
    for (const auto& [k, v] : refused) r.set(k, json::Value::make_int(v));
    d.set("refused", r);
    return d;
}

BasisTracker::BasisTracker(std::string underlying, OptD half_life_s, OptD alarm_pct,
                           int min_samples)
    : underlying_(std::move(underlying)),
      half_life_s_(half_life_s.value_or(DEFAULT_HALF_LIFE_S)),
      alarm_pct_(alarm_pct.value_or(DEFAULT_ALARM_PCT)),
      min_samples_(min_samples) {}

void BasisTracker::bump(const std::string& key) {
    long& c = refused_[key];
    c += 1;
}

bool BasisTracker::observe(OptD futures_mid,
                           const std::optional<kp::massive::index::IndexQuote>& index_quote,
                           double now) {
    if (!futures_mid || !index_quote || !index_quote->value) return false;
    const std::optional<long long> ts = index_quote->ts_ns;
    if (ts && last_index_ts_ && *ts == *last_index_ts_) return false;
    last_index_ts_ = ts;
    last_level_ = *index_quote->value;
    const double b = *futures_mid - *index_quote->value;
    if (!ema_) {
        ema_ = b;
        t_ = now;
        n_ = 1;
        return true;
    }
    // TIME-WEIGHTED, not per-sample: a dropped tick must weight a 3s gap as a 3s gap.
    const double dt = py_max(0.0, now - *t_);
    const double w = half_life_s_ > 0 ? std::pow(0.5, dt / half_life_s_) : 0.0;
    ema_ = w * *ema_ + (1.0 - w) * b;
    t_ = now;
    n_ += 1;
    last_resid_ = b - *ema_;
    max_resid_ = py_max(max_resid_, std::fabs(*last_resid_));
    return true;
}

SpotResult BasisTracker::spot(OptD futures_mid) {
    SpotResult r;
    SpotDetail& d = r.detail;
    d.underlying = underlying_;
    d.basis = ema_;
    d.n = n_;
    d.residual = last_resid_;
    if (!futures_mid) {
        d.reason = "no_futures";
        bump("no_futures");
        return r;
    }
    if (!ready()) {
        d.reason = "basis_warming_" + std::to_string(n_) + "/" + std::to_string(min_samples_);
        bump("warming");
        return r;
    }
    // THE BREAK DETECTOR, on the residual measured at the last INDEX tick.
    const double band = alarm_pct_ * (last_level_ ? *last_level_ : 0.0);
    if (last_resid_ && band > 0 && std::fabs(*last_resid_) > band) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "basis_break_%+.2fpts", *last_resid_);
        d.reason = std::string(buf);
        d.alarm = true;
        bump("break");
        return r;
    }
    const double s = *futures_mid - *ema_;
    if (!std::isfinite(s) || s <= 0) {
        d.reason = "non_finite";
        return r;
    }
    d.spot = s;
    r.spot = s;
    return r;
}

TrackerDetail BasisTracker::detail() const {
    TrackerDetail d;
    d.underlying = underlying_;
    d.basis = ema_;
    d.n = n_;
    d.half_life_s = half_life_s_;
    d.ready = ready();
    d.last_residual = last_resid_;
    d.max_abs_residual = kp::pycompat::py_round_nd(max_resid_, 4);
    d.alarm_pct = alarm_pct_;
    if (last_level_) d.alarm_points = kp::pycompat::py_round_nd(alarm_pct_ * *last_level_, 3);
    d.refused = refused_;
    return d;
}

}  // namespace kp::pricing::basis
