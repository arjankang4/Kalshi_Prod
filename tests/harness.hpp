// A 60-line check/FAIL harness, mirroring strategy/test_quoting.py.
//
// Deliberately not GTest or Catch2. The Python tests say why (test_quoting.py:4): "No pytest
// dependency -- this has to be runnable in the minutes before a session." The C++ side keeps
// that property, and keeps the identical label strings so a failure here greps against the
// Python one.
#pragma once
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace kp::test {

inline std::vector<std::string>& fails() { static std::vector<std::string> f; return f; }

inline void section(const std::string& s) { std::printf("\n%s\n", s.c_str()); }

inline void check(const std::string& label, bool ok, const std::string& detail = "") {
    std::printf("  %s %s%s%s\n", ok ? "ok  " : "FAIL", label.c_str(),
                detail.empty() ? "" : " -- ", detail.c_str());
    if (!ok) fails().push_back(label);
}

template <class T>
inline void eq(const std::string& label, const T& got, const T& want) {
    const bool ok = (got == want);
    if (ok) { check(label, true); return; }
    check(label, false, "got=" + std::to_string(got) + " want=" + std::to_string(want));
}

// Bit-pattern equality. This is the bar for every ported numeric function: "close" is what
// the golden vectors exist to rule out.
inline bool same_bits(double a, double b) {
    std::uint64_t x, y;
    std::memcpy(&x, &a, 8);
    std::memcpy(&y, &b, 8);
    return x == y;
}

// Monotonic ordering over doubles, so adjacent doubles are adjacent integers.
inline std::int64_t ordered(double d) {
    std::int64_t b;
    std::memcpy(&b, &d, 8);
    return b >= 0 ? b : (std::int64_t)((std::uint64_t)1 << 63) - b;
}

inline std::int64_t ulp_gap(double a, double b) {
    if (std::isnan(a) || std::isnan(b)) return (std::isnan(a) && std::isnan(b)) ? 0 : INT64_MAX;
    if (std::isinf(a) || std::isinf(b)) return (a == b) ? 0 : INT64_MAX;
    std::int64_t d = ordered(a) - ordered(b);
    return d < 0 ? -d : d;
}

inline void bits(const std::string& label, double got, double want) {
    if (same_bits(got, want)) { check(label, true); return; }
    char buf[160];
    std::snprintf(buf, sizeof buf, "got=%.17g (%a) want=%.17g (%a) ulp=%lld",
                  got, got, want, want, (long long)ulp_gap(got, want));
    check(label, false, buf);
}

inline int report(const char* suite) {
    if (fails().empty()) { std::printf("\n%s: all ok\n", suite); return 0; }
    std::printf("\n%s: %zu FAILED\n", suite, fails().size());
    for (const auto& f : fails()) std::printf("  - %s\n", f.c_str());
    return 1;
}

}  // namespace kp::test
