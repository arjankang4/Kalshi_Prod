#include "kp/app/run04.hpp"

#include <cmath>
#include <cstdio>

#include "kp/config.hpp"
#include "kp/pycompat/round.hpp"

namespace kp::app::run04 {

namespace th = kp::pricing::theo;

std::string pct0(double x) {
    char b[64];
    std::snprintf(b, sizeof b, "%.0f%%", kp::pycompat::py_round(x * 100.0));
    return b;
}

Verdict verdict(const RunStats& st, double expected_cycles, bool in_session) {
    Verdict d;
    const long n = st.cycles, errors = st.errors, no_spot = st.no_spot, placed = st.placed;
    const long n_ok = std::max(0L, n - errors);
    d.cycles = n;
    d.expected = (long)expected_cycles;          // int(): truncation toward zero
    d.errors = errors;
    d.error_rate = n ? (double)errors / (double)n : 1.0;
    d.no_spot = no_spot;
    d.spot_rate = n_ok ? (double)(n_ok - no_spot) / (double)n_ok : 0.0;
    d.placed = placed;
    d.in_session = in_session;

    if (n == 0) {
        d.reasons.push_back("no cycles ran");
        d.verdict = "FAIL"; d.exit_code = 1; return d;
    }
    if (d.spot_rate <= 0.0) {
        if (in_session) {
            d.reasons.push_back("spot never resolved while inside the session gate");
            d.verdict = "NO_SPOT"; d.exit_code = 1; return d;
        }
        d.reasons.push_back("market not open");
        d.verdict = "NO_SESSION"; d.exit_code = 0; return d;
    }
    if ((double)n < MIN_CYCLE_FRACTION * expected_cycles)
        d.reasons.push_back("only " + std::to_string(n) + " of an expected "
                            + std::to_string((long)expected_cycles) + " cycles");
    if (d.error_rate >= MAX_ERROR_RATE) d.reasons.push_back("error rate " + pct0(d.error_rate));
    if (d.spot_rate < MIN_SPOT_RATE)
        d.reasons.push_back("spot resolved on only " + pct0(d.spot_rate) + " of cycles");
    if (placed == 0) d.reasons.push_back("no order was ever planned");
    d.verdict = d.reasons.empty() ? "PASS" : "FAIL";
    d.exit_code = d.reasons.empty() ? 0 : 1;
    return d;
}

std::vector<th::Market> still_open(const std::vector<th::Market>& markets, long long now_ns) {
    std::vector<th::Market> out;
    for (const auto& m : markets) {
        if (!m.close_time || m.close_time->empty()) { out.push_back(m); continue; }
        const auto t = th::parse_iso_ns(*m.close_time);
        if (!t) { out.push_back(m); continue; }
        if (*t > now_ns) out.push_back(m);
    }
    return out;
}

OptD sigma(const kp::pricing::atmvol::AtmFit* fit, const kp::pricing::varcal::VarianceCalendar& cal,
           const kp::time::Instant& now) {
    if (fit == nullptr || !fit->ok()) return std::nullopt;
    const double left = 1.0 - cal.phi(now);
    if (left <= 1e-6) return std::nullopt;
    return *fit->w / std::pow(left / 252.0, 0.5);
}

long long py_timestamp_ns(const kp::time::Instant& now) {
    long long us = now.ns / 1000;
    if (now.ns % 1000 < 0) --us;                          // datetime floors to microseconds
    const double ts = (double)us / 1e6;                   // td / timedelta(seconds=1)
    return (long long)(ts * 1e9);                         // int() truncates toward zero
}

bool in_trading_session(const kp::time::Instant& now) {
    const auto p = kp::time::to_et(now);
    // Python weekday(): Monday = 0. Days since 1970-01-01 (a Thursday) of the ET civil date.
    const long long days = [&] {
        long long y = p.year; unsigned m = (unsigned)p.month, d = (unsigned)p.day;
        y -= m <= 2;
        const long long era = (y >= 0 ? y : y - 399) / 400;
        const unsigned yoe = (unsigned)(y - era * 400);
        const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + (long long)doe - 719468;
    }();
    const long long wd = ((days % 7) + 7 + 3) % 7;      // 1970-01-01 was weekday 3
    if (wd >= 5) return false;
    const auto& o = kp::config::TRADING_SESSION_OPEN;
    const auto& c = kp::config::TRADING_SESSION_CLOSE;
    const auto hm = std::make_pair((long)p.hour, (long)p.minute);
    return std::make_pair(o[0], o[1]) <= hm && hm < std::make_pair(c[0], c[1]);
}

std::string run_stamp(const kp::time::Instant& now) {
    long long s = now.ns / 1000000000LL;
    if (now.ns % 1000000000LL < 0) --s;
    const long long days = s >= 0 ? s / 86400 : (s - 86399) / 86400;
    const long long sod = s - days * 86400;
    // civil_from_days
    long long z = days + 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y = (long long)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
    char b[32];
    std::snprintf(b, sizeof b, "%04lld%02u%02uT%02lld%02lld%02lldZ", y, m, d, sod / 3600,
                  (sod / 60) % 60, sod % 60);
    return b;
}

std::string occ_expiry(const kp::time::Instant& now) {
    const auto p = kp::time::to_et(now);
    char b[16];
    std::snprintf(b, sizeof b, "%02d%02d%02d", p.year % 100, p.month, p.day);
    return b;
}

std::string board_stamp(const kp::time::Instant& now) {
    static const char* MON[12] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                  "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
    const auto p = kp::time::to_et(now);
    char b[16];
    std::snprintf(b, sizeof b, "%02d%s%02d", p.year % 100, MON[p.month - 1], p.day);
    return b;
}

}  // namespace kp::app::run04
