#include "kp/pycompat/repr.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <string>
#include <system_error>

namespace kp::pycompat {

std::string py_float_repr(double x) {
    if (std::isnan(x)) return "nan";
    if (std::isinf(x)) return x > 0 ? "inf" : "-inf";
    if (x == 0.0) return std::signbit(x) ? "-0.0" : "0.0";

    // Shortest digits in scientific form: [-]d[.ddd]e[+-]XX
    char buf[64];
    const auto r = std::to_chars(buf, buf + sizeof buf, x, std::chars_format::scientific);
    std::string s(buf, r.ptr);
    const bool neg = s[0] == '-';
    if (neg) s.erase(0, 1);
    const auto epos = s.find('e');
    const int exp10 = std::stoi(s.substr(epos + 1));
    std::string digits;
    for (char c : s.substr(0, epos))
        if (c != '.') digits.push_back(c);
    const int nd = (int)digits.size();
    // CPython: decpt = exp10 + 1; plain when -4 < decpt <= 16.
    const int decpt = exp10 + 1;
    std::string out;
    if (decpt > -4 && decpt <= 16) {
        if (decpt <= 0) {
            out = "0." + std::string(-decpt, '0') + digits;
        } else if (decpt >= nd) {
            out = digits + std::string(decpt - nd, '0') + ".0";
        } else {
            out = digits.substr(0, decpt) + "." + digits.substr(decpt);
        }
    } else {
        out = digits.substr(0, 1);
        if (nd > 1) out += "." + digits.substr(1);
        char eb[16];
        std::snprintf(eb, sizeof eb, "e%c%02d", exp10 < 0 ? '-' : '+', std::abs(exp10));
        out += eb;
    }
    return neg ? "-" + out : out;
}

std::string py_json_float(double x) {
    if (std::isnan(x)) return "NaN";
    if (std::isinf(x)) return x > 0 ? "Infinity" : "-Infinity";
    return py_float_repr(x);
}

}  // namespace kp::pycompat
