#include "kp/strategy/risktape.hpp"

#include <cmath>
#include <stdexcept>

#include "kp/pricing/bs.hpp"
#include "kp/pricing/greeks.hpp"
#include "kp/pycompat/round.hpp"

namespace kp::strategy::risktape {

namespace json = kp::json;
namespace th = kp::pricing::theo;
namespace sk = kp::pricing::skew;
namespace vc = kp::pricing::varcal;

namespace {

json::Value num(const OptD& v) { return v ? json::Value::make_number(*v) : json::Value(); }
double py_max(double a, double b) { return b > a ? b : a; }
double py_min(double a, double b) { return b < a ? b : a; }
bool truthy(const OptD& v) { return v && *v != 0.0; }

}  // namespace

OptD reference_strike(const th::Market& m) {
    return m.floor_strike ? m.floor_strike : m.cap_strike;
}

OptD vanilla_call_delta(OptD forward, OptD strike, const sk::SkewFit* skew_fit, OptD w_atm) {
    if (!forward || !strike || *forward <= 0 || *strike <= 0) return std::nullopt;
    OptD w;
    if (skew_fit && truthy(skew_fit->w_atm()))
        w = *skew_fit->w_atm() * skew_fit->ratio(*strike);
    else if (truthy(w_atm))
        w = w_atm;
    if (!truthy(w) || *w <= 0) return std::nullopt;
    const auto d = kp::pricing::bs::d1_d2(*forward, *strike, *w);
    return kp::pricing::bs::norm_cdf(d.d1);
}

std::optional<int> bucket_of(OptD delta) {
    if (!delta) return std::nullopt;
    const double d = py_min(py_max(*delta, 0.0), 1.0);
    const int n = (int)BUCKET_EDGES.size() - 1;
    for (int i = 0; i < n; ++i)
        if (d < BUCKET_EDGES[(std::size_t)i + 1] || i == n - 1) return i;
    return std::nullopt;
}

json::Value Detail::to_json() const {
    json::Value d;
    d.set("rows", json::Value::make_int(rows));
    d.set("errors", json::Value::make_int(errors));
    d.set("last_error", last_error ? json::Value::make_string(*last_error) : json::Value());
    d.set("interval_s", json::Value::make_number(interval_s));
    return d;
}

RiskTape::RiskTape(journal::Journal* journal, OptD interval_s)
    : journal_(journal), interval_s_(interval_s.value_or(RISK_TAPE_S)) {}

json::Value RiskTape::snapshot(const std::string& underlying, const kp::time::Instant& now,
                               const Positions& positions, const edgemgr::EdgeMap* em_map,
                               const std::vector<th::Market>& markets,
                               const kp::pricing::pricer::Pricer* pricer,
                               const vc::VarianceCalendar* calendar, OptD sigma_impl,
                               OptD sigma_rv, OptD spot, OptD fast_spot,
                               const std::vector<th::Market>& hourly_markets, double now_mono,
                               long long ts_ns, bool write) {
    // getattr(pricer, "spot"), getattr(pricer, "skew") -- the latter builds the lazy curve.
    const OptD forward = pricer ? pricer->spot() : std::nullopt;
    const sk::SkewFit* fit = pricer ? pricer->skew() : nullptr;
    return snapshot(underlying, now, positions, em_map, markets, forward, fit, calendar,
                    sigma_impl, sigma_rv, spot, fast_spot, hourly_markets, now_mono, ts_ns, write);
}

json::Value RiskTape::snapshot(const std::string& underlying, const kp::time::Instant& now,
                               const Positions& positions, const edgemgr::EdgeMap* em_map,
                               const std::vector<th::Market>& markets, OptD forward,
                               const sk::SkewFit* skew_fit, const vc::VarianceCalendar* calendar,
                               OptD sigma_impl, OptD sigma_rv, OptD spot, OptD fast_spot,
                               const std::vector<th::Market>& hourly_markets, double now_mono,
                               long long ts_ns, bool write) {
    // {ticker: market}; a repeated ticker keeps the LAST row, as the dict comprehension does.
    kp::pycompat::ODict<std::string, const th::Market*> by_ticker;
    for (const auto& m : markets) by_ticker.set(m.ticker, &m);

    // THE FALLBACK ATM VOL: w = sigma * sqrt(var_left / 252), flat, so the bucket axis never
    // collapses into `vega_unbucketed` merely for want of a curve.
    OptD w_fallback;
    if (truthy(sigma_impl) && calendar) {
        const double left = 1.0 - calendar->phi(now);
        if (left > 0) w_fallback = *sigma_impl * std::pow(left / 252.0, 0.5);
    }

    json::Value row;
    row.set("v", json::Value::make_int(1));
    row.set("ts_ns", json::Value::make_int(ts_ns));
    row.set("et", json::Value::make_string(journal::isoformat_et(now)));
    row.set("underlying", json::Value::make_string(underlying));
    row.set("spot", num(spot));
    row.set("fast_spot", num(fast_spot));
    row.set("forward", num(forward));
    row.set("sigma_impl", num(sigma_impl));
    row.set("sigma_rv", num(sigma_rv));
    row.set("n_positions", json::Value::make_int(0));
    row.set("net_contracts", json::Value::make_number(0.0));
    row.set("abs_contracts", json::Value::make_number(0.0));
    row.set("delta_dollars", json::Value::make_number(0.0));
    row.set("vega_dollars", json::Value::make_number(0.0));
    row.set("theta_dollars", json::Value::make_number(0.0));
    row.set("vega_bucketed", json::Value::make_number(0.0));
    row.set("vega_unbucketed", json::Value::make_number(0.0));
    row.set("skew_flat", skew_fit ? json::Value::make_bool(skew_fit->flat()) : json::Value());
    for (const char* n : BUCKET_NAMES) {
        row.set(std::string("vega_") + n, json::Value::make_number(0.0));
        row.set(std::string("pos_") + n, json::Value::make_number(0.0));
    }

    long n_positions = 0;
    double net = 0.0, gross = 0.0, delta_d = 0.0, vega_d = 0.0, theta_d = 0.0;
    double bucketed = 0.0, unbucketed = 0.0;
    std::array<double, 10> vega_b{}, pos_b{};
    for (const auto& [tk, p] : positions) {
        const double pos = p ? *p : 0.0;           // float(pos or 0.0)
        if (pos == 0.0) continue;
        const th::Market* const* mp = by_ticker.find(tk);
        if (!mp) continue;                          // another underlying's market
        const th::Market& m = **mp;
        const edgemgr::EdgeRecord* g = em_map ? em_map->find(tk) : nullptr;
        n_positions += 1;
        net += pos;
        gross += std::fabs(pos);
        const OptD d = g ? g->delta : std::nullopt;
        const OptD v = g ? g->vega : std::nullopt;
        const OptD t = g ? g->theta : std::nullopt;
        if (d) delta_d += pos * *d;
        if (t) theta_d += pos * *t;
        if (!v) continue;
        const double vega = pos * *v;
        vega_d += vega;
        const OptD cd = vanilla_call_delta(forward, reference_strike(m), skew_fit, w_fallback);
        const std::optional<int> b = bucket_of(cd);
        if (!b) { unbucketed += vega; continue; }
        vega_b[(std::size_t)*b] += vega;
        pos_b[(std::size_t)*b] += pos;
        bucketed += vega;
    }
    row.set("n_positions", json::Value::make_int(n_positions));
    row.set("net_contracts", json::Value::make_number(net));
    row.set("abs_contracts", json::Value::make_number(gross));
    row.set("delta_dollars", json::Value::make_number(delta_d));
    row.set("vega_dollars", json::Value::make_number(vega_d));
    row.set("theta_dollars", json::Value::make_number(theta_d));
    row.set("vega_bucketed", json::Value::make_number(bucketed));
    row.set("vega_unbucketed", json::Value::make_number(unbucketed));
    for (std::size_t i = 0; i < BUCKET_NAMES.size(); ++i) {
        row.set(std::string("vega_") + BUCKET_NAMES[i], json::Value::make_number(vega_b[i]));
        row.set(std::string("pos_") + BUCKET_NAMES[i], json::Value::make_number(pos_b[i]));
    }

    // THE VARIANCE CLOCK, both horizons, as fractions of one session.
    row.set("var_left_day", calendar ? json::Value::make_number(1.0 - calendar->phi(now)) : json::Value());

    // The soonest hourly settlement strictly after `now` (first wins a tie).
    std::optional<kp::time::Instant> settle;
    std::optional<std::string> tkr;
    for (const auto& m : hourly_markets) {
        const auto s = edgemgr::settlement_of(m);
        if (!s || *s <= now) continue;
        if (!settle || *s < *settle) { settle = s; tkr = m.ticker; }
    }
    // Kalshi spells every close_time with "Z", which settlement_of turns into +00:00.
    row.set("next_hourly", settle ? json::Value::make_string(journal::isoformat_utc(*settle))
                                  : json::Value());
    row.set("next_hourly_ticker", tkr ? json::Value::make_string(*tkr) : json::Value());
    row.set("minutes_to_hourly",
            settle ? json::Value::make_number(
                         kp::pycompat::py_round_nd(kp::time::total_seconds(now, *settle) / 60.0, 3))
                   : json::Value());
    OptD vl;
    if (settle && calendar) vl = kp::pricing::greeks::var_left(*calendar, now, *settle);
    row.set("var_left_hourly", num(vl));

    if (write && journal_) {
        try {
            journal_->write(row);
            rows_ += 1;
            last_ = now_mono;
        } catch (const std::exception& e) {
            errors_ += 1;
            last_error_ = "write: OSError: " + std::string(e.what()).substr(0, 80);
        }
    }
    return row;
}

Detail RiskTape::detail() const {
    Detail d;
    d.rows = rows_;
    d.errors = errors_;
    d.last_error = last_error_;
    d.interval_s = interval_s_;
    return d;
}

}  // namespace kp::strategy::risktape
