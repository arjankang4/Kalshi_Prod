#include "kp/rvol/features.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <tuple>

#include "kp/rvol/npcompat.hpp"

namespace kp::rvol::features {

namespace {

using kp::time::Instant;

// Howard Hinnant's days_from_civil: proleptic Gregorian date -> days since 1970-01-01.
std::int64_t days_from_civil(std::int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

constexpr std::int64_t US_DAY = 86400LL * 1000000LL;

// The ET wall clock of an instant as naive microseconds -- what Python compares and subtracts
// when both operands carry config.ET.
std::int64_t naive_us(const Instant& t) {
    const kp::time::EtParts p = kp::time::to_et(t);
    return days_from_civil(p.year, (unsigned)p.month, (unsigned)p.day) * US_DAY
         + ((std::int64_t)p.hour * 3600 + (std::int64_t)p.minute * 60 + p.second) * 1000000LL
         + p.nanos / 1000;
}

bool naive_lt(const Instant& a, const Instant& b) { return naive_us(a) < naive_us(b); }

// Python max(a, b): b only if strictly greater, else a.
const Instant& naive_max(const Instant& a, const Instant& b) { return naive_lt(a, b) ? b : a; }
const Instant& naive_min(const Instant& a, const Instant& b) { return naive_lt(b, a) ? b : a; }

Instant open_of(const Instant& t) { return kp::time::et_same_day(t, OPEN_H, OPEN_M); }

}  // namespace

double wall_seconds(const Instant& a, const Instant& b) {
    return (double)(naive_us(b) - naive_us(a)) / 1e6;
}

RvCov averaged_rv(const std::vector<std::int64_t>& ts_ms, const std::vector<double>& px,
                  std::int64_t t0_ms, std::int64_t t1_ms, int scale_s, int n_offsets) {
    const double NaN = std::nan("");
    if (t1_ms <= t0_ms || ts_ms.size() < 2) return {NaN, 0.0};
    n_offsets = n_offsets ? n_offsets : scale_s;
    const std::int64_t step = (std::int64_t)scale_s * 1000;
    const double span = (double)(t1_ms - t0_ms);
    std::vector<double> rates, covered;
    for (int off = 0; off < n_offsets; ++off) {
        // step // n_offsets: both positive, so C++ division is Python's floor division.
        const std::int64_t g0 = t0_ms + off * (n_offsets ? step / n_offsets : 0);
        // np.arange(g0, t1_ms + 1, step): ceil((stop - start) / step) points, none if negative.
        const std::int64_t delta = t1_ms + 1 - g0;
        const std::int64_t n = delta > 0 ? (delta + step - 1) / step : 0;
        if (n < 2) continue;
        // idx = searchsorted(ts, grid, side="right") - 1; ok = idx >= 0.
        std::vector<std::int64_t> g;
        std::vector<double> p;
        g.reserve(n);
        p.reserve(n);
        std::size_t n_ok = 0;
        for (std::int64_t i = 0; i < n; ++i) {
            const std::int64_t gi = g0 + i * step;
            const auto it = std::upper_bound(ts_ms.begin(), ts_ms.end(), gi);
            if (it == ts_ms.begin()) continue;           // idx == -1
            ++n_ok;
            g.push_back(gi);
            const double v = px[(std::size_t)(it - ts_ms.begin()) - 1];
            if (v > 0) p.push_back(v);                     // p = p[p > 0]
        }
        if (n_ok < 2) continue;
        if (p.size() < 2) continue;
        const std::vector<double> lp = np::log(p);
        std::vector<double> r2;
        r2.reserve(lp.size());
        for (std::size_t i = 1; i < lp.size(); ++i) {
            const double r = lp[i] - lp[i - 1];            // np.diff
            if (std::isfinite(r)) r2.push_back(r * r);     // r[isfinite(r)] ** 2
        }
        if (r2.empty()) continue;
        const double cov_span = (double)(g.back() - g.front());
        if (cov_span <= 0) continue;
        rates.push_back(np::sum(r2) / cov_span);
        covered.push_back(cov_span / span);
    }
    if (rates.empty()) return {NaN, 0.0};
    return {np::mean(rates) * span, np::mean(covered)};
}

const kp::pricing::gapcal::GapCalendar& gap_calendar(const std::string& underlying,
                                                     const std::string& profile,
                                                     const std::string& root) {
    static std::mutex mu;
    static std::map<std::tuple<std::string, std::string, std::string>,
                    std::unique_ptr<kp::pricing::gapcal::GapCalendar>> cache;
    std::lock_guard<std::mutex> lk(mu);
    auto key = std::make_tuple(underlying, profile, root);
    auto it = cache.find(key);
    if (it == cache.end()) {
        // A failed load throws and caches nothing, as functools.lru_cache does.
        auto g = std::make_unique<kp::pricing::gapcal::GapCalendar>(
            kp::pricing::gapcal::GapCalendar::load(underlying, profile, root));
        it = cache.emplace(std::move(key), std::move(g)).first;
    }
    return *it->second;
}

double variance_time(const kp::pricing::varcal::VarianceCalendar& cal, const Instant& t0,
                     const Instant& t1, const kp::pricing::gapcal::GapCalendar* gcal,
                     const std::string& root) {
    const Instant open_dt = open_of(t1);
    double v = 0.0;
    const Instant lo = naive_max(t0, open_dt);
    if (naive_lt(lo, t1)) v += cal.phi(t1) - cal.phi(lo);
    if (naive_lt(t0, open_dt)) {
        const auto& g = gcal ? *gcal : gap_calendar(cal.underlying(), "default", root);
        v += cal.overnight_share() * g.share_between(t0, open_dt);
    }
    return (0.0 > v) ? 0.0 : v;                            // max(v, 0.0); NaN passes through
}

double annualized(double session_var) {
    if (!std::isfinite(session_var) || session_var < 0) return std::nan("");
    return std::sqrt(session_var * TRADING_DAYS);
}

std::int64_t ms(const Instant& t) {
    std::int64_t us = t.ns / 1000;
    if (t.ns % 1000 < 0) --us;                              // floor, as datetime truncates
    const double ts = (double)us / 1e6;
    return (std::int64_t)(ts * 1000.0);                     // int() truncates toward zero
}

Instant wall_minus_minutes(const Instant& t, int minutes) {
    const kp::time::EtParts p = kp::time::to_et(t);
    std::int64_t secs = days_from_civil(p.year, (unsigned)p.month, (unsigned)p.day) * 86400
                      + (std::int64_t)p.hour * 3600 + (std::int64_t)p.minute * 60 + p.second
                      - (std::int64_t)minutes * 60;
    // The result carries fold=0, and zoneinfo resolves the wall clock with it: an AMBIGUOUS
    // reading (the repeated November hour) takes the FIRST occurrence -- the EDT one, the earlier
    // instant -- and a NONEXISTENT one (the skipped March hour) takes the offset in force BEFORE
    // the transition (EST). kp::time::from_et goes through mktime(tm_isdst=-1), which on this
    // glibc resolves the repeated hour to EST, so it is not used here: both ET offsets are tried
    // and the one whose wall clock round-trips wins, the earlier instant on a tie.
    const std::int64_t naive_s = secs;
    const std::int64_t cand_edt = naive_s + 4 * 3600, cand_est = naive_s + 5 * 3600;
    auto round_trips = [&](std::int64_t s) {
        const kp::time::EtParts q = kp::time::to_et(Instant{s * 1000000000LL});
        const std::int64_t back = days_from_civil(q.year, (unsigned)q.month, (unsigned)q.day) * 86400
                                + (std::int64_t)q.hour * 3600 + (std::int64_t)q.minute * 60 + q.second;
        return back == naive_s;
    };
    std::int64_t s;
    if (round_trips(cand_edt)) s = cand_edt;          // EDT valid (and earlier when ambiguous)
    else if (round_trips(cand_est)) s = cand_est;
    else s = cand_est;                                // nonexistent: the pre-transition offset
    return Instant{s * 1000000000LL + p.nanos};
}

WindowSigma window_sigma(const kp::pricing::varcal::VarianceCalendar& cal,
                         const std::vector<std::int64_t>& ts_idx, const std::vector<double>& px_idx,
                         const std::vector<std::int64_t>& ts_px, const std::vector<double>& px_px,
                         const Instant& t0, const Instant& t1, int scale_s,
                         const kp::pricing::gapcal::GapCalendar* gcal, const std::string& root) {
    const double NaN = std::nan("");
    const Instant open_dt = open_of(t1);
    double proxy_var = 0.0, total_rv = 0.0, cov_num = 0.0, cov_den = 0.0;

    if (naive_lt(t0, open_dt)) {
        const Instant& end = naive_min(open_dt, t1);
        const RvCov p = averaged_rv(ts_px, px_px, ms(t0), ms(end), scale_s);
        if (std::isfinite(p.rv)) {
            total_rv += p.rv;
            proxy_var += p.rv;
        }
        const double w = wall_seconds(t0, end);
        cov_num += p.coverage * w;
        cov_den += w;
    }

    const Instant lo = naive_max(t0, open_dt);
    if (naive_lt(lo, t1)) {
        const RvCov i = averaged_rv(ts_idx, px_idx, ms(lo), ms(t1), scale_s);
        if (std::isfinite(i.rv)) total_rv += i.rv;
        const double w = wall_seconds(lo, t1);
        cov_num += i.coverage * w;
        cov_den += w;
    }

    const double coverage = cov_den > 0 ? cov_num / cov_den : 0.0;
    // Evaluated BEFORE the gates, as the reference does: a missing gap calendar raises here even
    // for a window that was about to be refused for coverage.
    const double vt = variance_time(cal, t0, t1, gcal, root);
    if (vt <= 0 || coverage < MIN_COVERAGE || total_rv <= 0) return {NaN, NaN, coverage};
    const double proxy_frac = total_rv > 0 ? proxy_var / total_rv : 0.0;
    return {annualized(total_rv / vt), proxy_frac, coverage};
}

}  // namespace kp::rvol::features
