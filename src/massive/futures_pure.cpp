#include "kp/massive/futures_pure.hpp"

#include <algorithm>
#include <cstdio>

#include "kp/time/et.hpp"

namespace kp::massive::futures {

std::optional<std::string> product(const std::string& u) {
    if (u == "SPX") return std::string("ES");
    if (u == "NDX") return std::string("NQ");
    return std::nullopt;
}

std::string product_for(const std::string& u) { return product(u).value_or(u); }

std::optional<std::string> cache_ticker(const std::string& u) { return product(u); }

bool valid_resolution(const std::string& r) {
    for (const char* x : RESOLUTIONS)
        if (r == x) return true;
    return false;
}

int month_for_code(char c) {
    switch (c) {
        case 'H': return 3;
        case 'M': return 6;
        case 'U': return 9;
        case 'Z': return 12;
        default: return 0;
    }
}

char code_for_month(int m) {
    switch (m) {
        case 3: return 'H';
        case 6: return 'M';
        case 9: return 'U';
        case 12: return 'Z';
        default: return '\0';
    }
}

std::string Date::iso() const {
    char b[16];
    std::snprintf(b, sizeof b, "%04d-%02d-%02d", year, month, day);
    return b;
}

// Howard Hinnant's days_from_civil / civil_from_days (proleptic Gregorian, as datetime.date).
long days_from_civil(const Date& dt) {
    long y = dt.year;
    const long m = dt.month, d = dt.day;
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const long yoe = y - era * 400;
    const long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

Date civil_from_days(long z) {
    z += 719468;
    const long era = (z >= 0 ? z : z - 146096) / 146097;
    const long doe = z - era * 146097;
    const long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long y = yoe + era * 400;
    const long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const long mp = (5 * doy + 2) / 153;
    const long d = doy - (153 * mp + 2) / 5 + 1;
    const long m = mp + (mp < 10 ? 3 : -9);
    return Date{(int)(y + (m <= 2)), (int)m, (int)d};
}

int weekday(const Date& d) {
    // 1970-01-01 was a Thursday (weekday 3).
    const long z = days_from_civil(d);
    return (int)(((z % 7) + 7 + 3) % 7);
}

std::optional<Parsed> parse_ticker(const std::string& t0, int today_year) {
    std::string t = t0;
    if (!t.empty() && t.back() == '\n') t.pop_back();          // `$` matches before a final \n
    if (t.size() != 4) return std::nullopt;
    const auto upper = [](char c) { return c >= 'A' && c <= 'Z'; };
    if (!upper(t[0]) || !upper(t[1])) return std::nullopt;
    const int month = month_for_code(t[2]);
    if (!month) return std::nullopt;
    if (t[3] < '0' || t[3] > '9') return std::nullopt;
    const int base = today_year;
    int year = base - (base % 10) + (t[3] - '0');
    if (year < base - 5) year += 10;
    return Parsed{t.substr(0, 2), month, year};
}

Date third_friday(int year, int month) {
    int seen = 0;
    const long first = days_from_civil(Date{year, month, 1});
    for (int i = 0; i < 31; ++i) {
        const Date d = civil_from_days(first + i);
        if (d.month == month && weekday(d) == 4 && ++seen == 3) return d;
    }
    return Date{year, month, 1};   // unreachable: every month has a third Friday
}

std::vector<std::pair<std::string, Date>> quarterly_candidates(const std::string& product_code,
                                                               const Date& day, int n) {
    std::vector<std::pair<std::string, Date>> out;
    int year = day.year, month = 0;
    for (int m : QUARTERLY_MONTHS)
        if (m <= day.month) month = std::max(month, m);
    if (month == 0) { year -= 1; month = 12; }
    for (int it = 0; it < n + 2; ++it) {
        const Date tf = third_friday(year, month);
        if (tf >= day) {
            // year % 10 is a Python modulo: non-negative for the (always positive) years here.
            out.emplace_back(product_code + std::string(1, code_for_month(month))
                                 + std::to_string(((year % 10) + 10) % 10),
                             tf);
            if ((int)out.size() == n) break;
        }
        std::size_t i = 0;
        while (QUARTERLY_MONTHS[i] != month) ++i;
        if (i == 3) { year += 1; month = QUARTERLY_MONTHS[0]; }
        else month = QUARTERLY_MONTHS[i + 1];
    }
    return out;
}

std::optional<std::string> front_month(const std::string& product_code, const Date& day,
                                       const SessionVolumeFn& session_volume, int n_candidates,
                                       const std::optional<std::string>& incumbent) {
    std::vector<std::string> cands;
    for (const auto& [tk, _e] : quarterly_candidates(product_code, day, n_candidates))
        cands.push_back(tk);
    if (incumbent && !incumbent->empty()
        && std::find(cands.begin(), cands.end(), *incumbent) == cands.end())
        cands.insert(cands.begin(), *incumbent);          // always re-probe the sitting contract
    std::optional<std::string> best;
    double best_vol = 0;
    for (const auto& tk : cands) {
        const double vol = session_volume(tk, day);
        if (vol > best_vol) { best = tk; best_vol = vol; }
    }
    if (best_vol >= MIN_FRONT_VOLUME) return best;
    return std::nullopt;
}

long long et_ns(const Date& day, int hour, int minute) {
    const kp::time::Instant t = kp::time::from_et(day.year, day.month, day.day, hour, minute);
    // timestamp() is a float of whole seconds here; `* 1e9` then int(), as the reference does.
    const double ts = (double)(t.ns / 1000000000LL) + (double)(t.ns % 1000000000LL) / 1e9;
    return (long long)(ts * 1e9);
}

std::pair<long long, long long> rth_window_ns(const Date& day) {
    return {et_ns(day, 9, 30), et_ns(day, 16, 0)};
}

}  // namespace kp::massive::futures
