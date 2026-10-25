#include "kp/pycompat/round.hpp"

#include <algorithm>
#include <cassert>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace kp::pycompat {

std::int64_t py_round_i(double x) {
    const double r = std::nearbyint(x);
    // 2^53. Beyond this a double cannot represent consecutive integers, so Python's
    // arbitrary-precision int and our int64 would part company.
    if (!(std::fabs(r) <= 9007199254740992.0)) {
        throw std::overflow_error("py_round_i: magnitude exceeds 2^53");
    }
    return static_cast<std::int64_t>(r);
}

double py_round_nd(double x, int ndigits) {
    if (!std::isfinite(x)) return x;               // CPython returns inf/nan unchanged
    // CPython short-circuits when no decimal digit can survive; mirror it so huge magnitudes
    // never reach the buffer. 1e17 > 2^53, so x is already integral there.
    if (std::fabs(x) >= 1e17) return x;
    char buf[512];
    const int len = std::snprintf(buf, sizeof buf, "%.*f", ndigits, x);
    assert(len > 0 && len < static_cast<int>(sizeof buf));
    (void)len;
    return std::strtod(buf, nullptr);
}

void assert_c_numeric_locale() {
    const char* n = std::setlocale(LC_NUMERIC, nullptr);
    if (n == nullptr || (std::strcmp(n, "C") != 0 && std::strcmp(n, "POSIX") != 0)) {
        std::fprintf(stderr,
                     "FATAL: LC_NUMERIC is \"%s\", must be \"C\".\n"
                     "py_round_nd formats through printf and would emit a comma decimal\n"
                     "separator, silently corrupting every tick-rounded price.\n",
                     n ? n : "(null)");
        std::abort();
    }
}

double py_median(std::vector<double> v) {
    const std::size_t n = v.size();
    assert(n > 0);
    std::sort(v.begin(), v.end());
    if (n & 1u) return v[n / 2];
    return (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

}  // namespace kp::pycompat
