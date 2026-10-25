// Market rows off the REST API, and the one timestamp parser every reader of them shares.
#include <algorithm>
#include <cctype>
#include <string>

#include "kp/pricing/theo_core.hpp"

namespace kp::pricing::theo {

namespace {

long long days_from_civil(long long y, unsigned m, unsigned d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}

bool digits(const std::string& s, std::size_t at, std::size_t n, int& out) {
    if (at + n > s.size()) return false;
    out = 0;
    for (std::size_t i = at; i < at + n; ++i) {
        if (!std::isdigit((unsigned char)s[i])) return false;
        out = out * 10 + (s[i] - '0');
    }
    return true;
}

}  // namespace

std::optional<long long> parse_iso_ns(const std::string& raw) {
    std::string s = raw;
    if (!s.empty() && s.back() == 'Z') s = s.substr(0, s.size() - 1) + "+00:00";
    int Y, M, D, h = 0, mi = 0, sec = 0;
    if (!digits(s, 0, 4, Y) || s.size() < 10 || s[4] != '-' || !digits(s, 5, 2, M) || s[7] != '-'
        || !digits(s, 8, 2, D))
        return std::nullopt;
    std::size_t i = 10;
    long long frac_ns = 0;
    if (i < s.size() && (s[i] == 'T' || s[i] == ' ')) {
        ++i;
        if (!digits(s, i, 2, h)) return std::nullopt;
        i += 2;
        if (i < s.size() && s[i] == ':') {
            if (!digits(s, i + 1, 2, mi)) return std::nullopt;
            i += 3;
            if (i < s.size() && s[i] == ':') {
                if (!digits(s, i + 1, 2, sec)) return std::nullopt;
                i += 3;
                if (i < s.size() && (s[i] == '.' || s[i] == ',')) {
                    // 3.11+: any number of fraction digits, truncated to microseconds.
                    std::size_t j = i + 1, n = 0;
                    long long f = 0;
                    while (j < s.size() && std::isdigit((unsigned char)s[j])) {
                        if (n < 6) f = f * 10 + (s[j] - '0');
                        ++j; ++n;
                    }
                    if (n == 0) return std::nullopt;
                    for (std::size_t k = std::min<std::size_t>(n, 6); k < 9; ++k) f *= 10;
                    frac_ns = f;
                    i = j;
                }
            }
        }
    }
    long long off_s = 0;
    if (i < s.size()) {
        if (s[i] != '+' && s[i] != '-') return std::nullopt;
        const int sign = s[i] == '-' ? -1 : 1;
        int oh, om;
        if (!digits(s, i + 1, 2, oh) || i + 3 >= s.size() || s[i + 3] != ':' || !digits(s, i + 4, 2, om))
            return std::nullopt;
        if (i + 6 != s.size()) return std::nullopt;
        off_s = sign * (oh * 3600LL + om * 60LL);
    }
    if (M < 1 || M > 12 || D < 1 || h > 23 || mi > 59 || sec > 59) return std::nullopt;
    static const int MDAYS[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (Y % 4 == 0 && Y % 100 != 0) || Y % 400 == 0;
    if (D > MDAYS[M - 1] + (M == 2 && leap ? 1 : 0)) return std::nullopt;
    const long long days = days_from_civil(Y, (unsigned)M, (unsigned)D);
    const long long epoch_s = days * 86400LL + h * 3600LL + mi * 60LL + sec - off_s;
    return epoch_s * 1000000000LL + frac_ns;
}

Market market_from_json(const kp::json::Value& row) {
    Market m;
    m.ticker = row.opt_str("ticker").value_or("");
    m.strike_type = row.opt_str("strike_type").value_or("");
    m.floor_strike = row.opt_num("floor_strike");
    m.cap_strike = row.opt_num("cap_strike");
    m.event_ticker = row.opt_str("event_ticker").value_or("");
    m.close_time = row.opt_str("close_time");
    if (m.close_time) {
        if (const auto ns = parse_iso_ns(*m.close_time)) {
            // floor division: a fractional second before the epoch still floors toward the past
            long long q = *ns / 1000000000LL;
            if (*ns % 1000000000LL < 0) --q;
            m.close_time_s = q;
        }
    }
    return m;
}

}  // namespace kp::pricing::theo
