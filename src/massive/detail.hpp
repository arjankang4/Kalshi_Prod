// Python-semantics helpers private to src/massive. Not a public header: nothing outside this
// package should grow a dependency on a partial CPython emulation.
#pragma once
#include <cmath>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

#include "kp/json/value.hpp"

namespace kp::massive::detail {

// Python's TypeError, where the reference would raise one (float([1]), a list as a dict key).
// Derives from invalid_argument so a catch of "bad input" sees it; name it first when naming.
class PyTypeError : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

// Python's repr() of a json.loads() result: dict -> {'k': v, ...}, list -> [...], str -> '...'
// (single quotes unless the text holds a ' and no "), True/False/None, int as digits, float via
// float repr. What an f-string renders a vendor payload as -- `f"auth failed: {auth}"`,
// `f"status: {str(m)[:120]}"` -- so those messages come out byte-identical.
std::string py_repr(const kp::json::Value& v);

// str[:n] -- by code point in Python, by byte here. Identical for ASCII, which is what the vendor
// sends; a multi-byte character straddling the cut is kept whole rather than split.
std::string head(const std::string& s, std::size_t n);

// Python truthiness of a json value (0, 0.0, "", [], {}, None, False are falsy).
bool truthy(const kp::json::Value& v);

// float(x) of a json value. Numbers and bools convert; a string is parsed as float() would
// (surrounding whitespace allowed, the whole remainder must be a number); anything else raises.
// Throws std::invalid_argument, which the feeds turn into their `handle:` error.
double py_float(const kp::json::Value& v);

// int(x) of a json value: ints as-is, floats truncated toward zero (non-finite raises), bools
// 0/1, strings of optional sign + digits. Throws std::invalid_argument otherwise.
long long py_int(const kp::json::Value& v);

// Python int / int true division. CPython divides exactly-representable operands (|x| <= 2**53)
// with ONE correctly rounded double division, which is `(double)a / (double)b`; beyond that it
// does a correctly rounded long division, which long double approximates. Every call here is an
// age or a span of a few seconds in nanoseconds, i.e. deep inside the exact branch.
inline double py_true_div(long long a, long long b) {
    const long long LIM = 1LL << 53;
    if (a <= LIM && a >= -LIM && b <= LIM && b >= -LIM) return (double)a / (double)b;
    return (double)((long double)a / (long double)b);
}

// x ** 2 for a Python float, i.e. libm pow(|x|, 2.0) -- CPython's float_pow takes |x| for a
// negative base with an integral exponent and calls pow(). NOT x*x: glibc's pow is not correctly
// rounded, and the two differ on ~0.085% of doubles (measured, 42,642 of 50M). The volatile
// exponent keeps GCC from folding pow(x, 2.0) into x*x, which it does even without -ffast-math
// because it considers the rewrite exact.
inline double py_sq(double x) {
    static volatile double two = 2.0;
    return std::pow(std::fabs(x), two);
}

// builtins.sum over floats in CPython 3.12+: Neumaier-compensated, with the compensation added
// once at the end only when it is non-zero and finite. (3.12 changed sum(); a naive fold differs
// in the last bits on long sums.) Start value int 0, so the first float term is exact.
class PyFloatSum {
public:
    void add(double x) {
        const double t = f_ + x;
        if (std::fabs(f_) >= std::fabs(x)) c_ += (f_ - t) + x;
        else c_ += (x - t) + f_;
        f_ = t;
    }
    // An int item once the sum is already a float: CPython adds (double)value UNcompensated.
    // Measured: sum([1e16, 1.0, 1.0]) == 1.0000000000000002e16 but sum([1e16, 1.0, 1]) == 1e16.
    void add_int(double v) { f_ += v; }
    double result() const {
        double f = f_;
        if (c_ != 0.0 && std::isfinite(c_)) f += c_;
        return f;
    }

private:
    double f_ = 0.0, c_ = 0.0;
};

}  // namespace kp::massive::detail
