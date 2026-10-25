#include "kp/storage/units.hpp"

#include <cmath>
#include <limits>

#include "kp/pycompat/repr.hpp"

namespace kp::storage::units {

namespace {

constexpr int PREC = 28;                 // decimal.getcontext().prec, which scaleb rounds to

struct Dec {
    enum Kind { D_FINITE, D_INF, D_QNAN, D_SNAN } kind = D_FINITE;
    bool neg = false;
    std::string coef;                    // digits, no leading zeros ("" means zero)
    long long exp = 0;
};

bool is_ws(unsigned char c) { return c == ' ' || (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x1F); }

// Strip Python whitespace (ASCII, NBSP U+00A0 and the other Unicode spaces as UTF-8).
std::string strip(const std::string& s) {
    auto lead = [&](std::size_t i) -> std::size_t {       // length of a whitespace char at i, or 0
        const unsigned char c = (unsigned char)s[i];
        if (is_ws(c)) return 1;
        if (c == 0xC2 && i + 1 < s.size() && ((unsigned char)s[i + 1] == 0xA0 || (unsigned char)s[i + 1] == 0x85)) return 2;
        if (c == 0xE3 && i + 2 < s.size() && (unsigned char)s[i + 1] == 0x80 && (unsigned char)s[i + 2] == 0x80) return 3;
        if (c == 0xE2 && i + 2 < s.size()) {
            const unsigned char b1 = (unsigned char)s[i + 1], b2 = (unsigned char)s[i + 2];
            if (b1 == 0x80 && ((b2 >= 0x80 && b2 <= 0x8A) || b2 == 0xA8 || b2 == 0xA9 || b2 == 0xAF)) return 3;
            if (b1 == 0x81 && b2 == 0x9F) return 3;
        }
        if (c == 0xE1 && i + 2 < s.size() && (unsigned char)s[i + 1] == 0x9A && (unsigned char)s[i + 2] == 0x80) return 3;
        return 0;
    };
    std::size_t b = 0;
    while (b < s.size()) { const auto k = lead(b); if (!k) break; b += k; }
    std::size_t e = b, i = b;
    while (i < s.size()) {
        const auto k = lead(i);
        if (k) { i += k; continue; }
        const unsigned char c = (unsigned char)s[i];
        i += c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        e = i;
    }
    return s.substr(b, e - b);
}

std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Decimal(str) syntax. false = InvalidOperation (ConversionSyntax).
bool parse(const std::string& raw, Dec& d) {
    std::string s;
    for (char c : strip(raw)) if (c != '_') s.push_back(c);
    std::size_t i = 0;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) { d.neg = s[i] == '-'; ++i; }
    const std::string rest = lower(s.substr(i));
    if (rest == "inf" || rest == "infinity") { d.kind = Dec::D_INF; return true; }
    for (const char* p : {"snan", "nan"}) {
        const std::string pre = p;
        if (rest.rfind(pre, 0) == 0) {
            for (std::size_t k = pre.size(); k < rest.size(); ++k) if (rest[k] < '0' || rest[k] > '9') return false;
            d.kind = pre == "snan" ? Dec::D_SNAN : Dec::D_QNAN;
            return true;
        }
    }
    std::string digits;
    long long frac = 0;
    bool any = false;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') { digits.push_back(s[i++]); any = true; }
    if (i < s.size() && s[i] == '.') {
        ++i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') { digits.push_back(s[i++]); ++frac; any = true; }
    }
    if (!any) return false;
    long long e = 0;
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        bool eneg = false;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) { eneg = s[i] == '-'; ++i; }
        bool eany = false;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            if (e < 1000000000000LL) e = e * 10 + (s[i] - '0');
            ++i;
            eany = true;
        }
        if (!eany) return false;
        if (eneg) e = -e;
    }
    if (i != s.size()) return false;
    std::size_t nz = 0;
    while (nz < digits.size() && digits[nz] == '0') ++nz;
    d.coef = digits.substr(nz);
    d.exp = e - frac;
    return true;
}

// scaleb(n) under the default context: exponent + n, then rounded to PREC digits, half-even.
void scaleb(Dec& d, int n) {
    d.exp += n;
    if ((long long)d.coef.size() <= PREC) return;
    const std::size_t drop = d.coef.size() - PREC;
    std::string keep = d.coef.substr(0, PREC);
    const std::string gone = d.coef.substr(PREC);
    bool up;
    if (gone[0] > '5') up = true;
    else if (gone[0] < '5') up = false;
    else {
        bool rest_nonzero = false;
        for (std::size_t k = 1; k < gone.size(); ++k) if (gone[k] != '0') { rest_nonzero = true; break; }
        up = rest_nonzero || ((keep.back() - '0') & 1);
    }
    if (up) {
        int k = PREC - 1;
        while (k >= 0 && keep[(std::size_t)k] == '9') keep[(std::size_t)k--] = '0';
        if (k >= 0) keep[(std::size_t)k]++;
        else { keep = "1" + keep.substr(0, PREC - 1); d.exp += 1; }   // 99..9 -> 100..0
    }
    d.exp += (long long)drop;
    std::size_t nz = 0;
    while (nz < keep.size() && keep[nz] == '0') ++nz;
    d.coef = keep.substr(nz);
}

std::int64_t to_int(const Dec& d, const std::string& shown, const char* kind, int places) {
    if (d.kind == Dec::D_INF) throw PyError("OverflowError", "cannot convert Infinity to integer");
    const std::string imprecise = std::string(kind) + " " + shown + " needs more than " +
                                  std::to_string(places) + "dp; canonical unit would truncate";
    if (d.kind == Dec::D_QNAN) throw PyError("PrecisionError", imprecise, true);
    if (d.coef.empty()) return 0;
    std::string c = d.coef;
    long long e = d.exp;
    if (e < 0) {
        if ((long long)c.size() <= -e) throw PyError("PrecisionError", imprecise, true);
        for (std::size_t k = c.size() - (std::size_t)(-e); k < c.size(); ++k)
            if (c[k] != '0') throw PyError("PrecisionError", imprecise, true);
        c.resize(c.size() - (std::size_t)(-e));
        e = 0;
    }
    if ((long long)c.size() + e > 19) throw PyError("OverflowError", "value outside int64: " + shown);
    unsigned __int128 v = 0;
    for (char ch : c) v = v * 10 + (unsigned)(ch - '0');
    for (long long k = 0; k < e; ++k) v *= 10;
    const unsigned __int128 lim = (unsigned __int128)std::numeric_limits<std::int64_t>::max() + (d.neg ? 1 : 0);
    if (v > lim) throw PyError("OverflowError", "value outside int64: " + shown);
    return d.neg ? (std::int64_t)(-(__int128)v) : (std::int64_t)v;
}

std::optional<std::int64_t> scaled_str(const std::string& s, int places, const char* kind) {
    if (s.empty()) return std::nullopt;
    const std::string shown = py_repr_str(s);
    Dec d;
    if (!parse(s, d) || d.kind == Dec::D_SNAN) throw PyError("PrecisionError", "not a decimal: " + shown, true);
    scaleb(d, places);
    return to_int(d, shown, kind, places);
}

// Decimal(float) is the EXACT binary value. x * 10^places is an integer iff x * 2^places is, since
// 5^places is odd; and within int64 the 28-digit rounding of scaleb can never hide a fraction (a
// double's fraction is never more than ~22 significant digits below its integer part).
std::optional<std::int64_t> scaled_float(double x, int places, const char* kind) {
    const std::string shown = kp::pycompat::py_float_repr(x);
    const std::string imprecise = std::string(kind) + " " + shown + " needs more than " +
                                  std::to_string(places) + "dp; canonical unit would truncate";
    if (std::isnan(x)) throw PyError("PrecisionError", imprecise, true);
    if (std::isinf(x)) throw PyError("OverflowError", "cannot convert Infinity to integer");
    if (x == 0.0) return 0;
    int E;
    const double f = std::frexp(std::fabs(x), &E);
    long long M = (long long)std::ldexp(f, 53);
    int e2 = E - 53;
    while ((M & 1) == 0) { M >>= 1; ++e2; }
    if (e2 + places < 0) throw PyError("PrecisionError", imprecise, true);
    __int128 v = M;
    for (int k = 0; k < places; ++k) v *= 5;
    for (int k = 0; k < e2 + places; ++k) {
        v *= 2;
        if (v > (__int128)std::numeric_limits<std::int64_t>::max() + 1) throw PyError("OverflowError", "value outside int64: " + shown);
    }
    if (x < 0) v = -v;
    if (v > std::numeric_limits<std::int64_t>::max() || v < std::numeric_limits<std::int64_t>::min())
        throw PyError("OverflowError", "value outside int64: " + shown);
    return (std::int64_t)v;
}

std::optional<std::int64_t> scaled(const kp::json::Value& v, int places, const char* kind) {
    using T = kp::json::Value::Type;
    switch (v.type()) {
        case T::Null: return std::nullopt;
        case T::String: return scaled_str(v.str(), places, kind);
        case T::Bool: return scaled_str(v.boolean() ? "1" : "0", places, kind);
        case T::Number:
            if (v.is_int()) return scaled_str(std::to_string(v.integer()), places, kind);
            return scaled_float(v.num(), places, kind);
        default:
            throw PyError("TypeError", "conversion from " + py_type_name(v) + " to Decimal is not supported");
    }
}

}  // namespace

std::optional<std::int64_t> to_micros(const kp::json::Value& v) { return scaled(v, MICROS, "price"); }
std::optional<std::int64_t> to_cc(const kp::json::Value& v) { return scaled(v, CENTI, "count"); }
std::optional<std::int64_t> to_micros(const std::string& s) { return scaled_str(s, MICROS, "price"); }
std::optional<std::int64_t> to_cc(const std::string& s) { return scaled_str(s, CENTI, "count"); }

}  // namespace kp::storage::units
