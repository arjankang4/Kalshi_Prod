// Python-semantics arithmetic shims.
//
// The port's bar is bit-identical output against the Kalshi reference. Most of the arithmetic
// translates literally, but a handful of Python builtins do NOT mean what the obvious C++
// spelling means. Each function here exists because the natural translation is measurably
// wrong, and each carries the measurement that proved it.
//
// Every one of these is exercised by tests/unit/test_pycompat.cpp against vectors generated
// from CPython itself. Do not "simplify" any of them.
#pragma once
#include <cmath>
#include <cstdint>
#include <vector>

namespace kp::pycompat {

// --- Python round(x), no ndigits -------------------------------------------------------
//
// Python rounds half to EVEN; std::round rounds half AWAY FROM ZERO. They differ on exact
// halves: round(2.5) is 2 in Python and 3 in C++.
//
// That is not hypothetical here. strategy/quoting.py:320 computes
//     [int(round(lo + (hi - lo) * u)) for lo, hi in ((1, 2), (2, 4))]
// and at u == 0.25 the second range evaluates round(2.5) exactly -- so std::round would rest
// tier rung 2 a full cent away from where Python puts it, on every market at that risk level,
// on both sides.
//
// C99 rint/nearbyint under the default FE_TONEAREST mode IS round-half-to-even, which is
// exactly Python's rule. Verified bit-identical to CPython over 200,000 random values plus
// every halfway case in [-2000, 2000]. nearbyint is preferred over rint because rint may
// raise FE_INEXACT, which is harmless but noisy.
//
// This depends on the rounding mode being FE_TONEAREST. Nothing in this project may call
// fesetround; tests/unit/test_fpenv.cpp asserts it.
// The `+ 0.0` is not decoration. Python's round() returns an INT, which has no signed zero,
// so float(round(-0.5)) is +0.0 -- while nearbyint(-0.5) is -0.0. Measured: 1 of 208,001
// fuzzed cases. IEEE-754 gives -0.0 + 0.0 == +0.0 and leaves every other finite value, plus
// inf and nan, untouched, so this is a branchless normalisation rather than a special case.
// It is safe only because -fno-signed-zeros is NOT set (see cmake/FloatingPoint.cmake).
inline double py_round(double x) noexcept { return std::nearbyint(x) + 0.0; }

// Python's round() returns an arbitrary-precision int. Every live-path use lands well inside
// 2^53, where a double is exact; the caller asserts rather than assumes.
std::int64_t py_round_i(double x);

// --- Python round(x, ndigits) ----------------------------------------------------------
//
// CPython's double_round formats x to n decimal places with correctly-rounded dtoa and reads
// it back. glibc's printf("%.*f") is likewise correctly rounded with exact ties to even, and
// strtod is correctly rounded, so the composition reproduces CPython exactly -- verified over
// 400,000 random values in [-200, 200] at n = 9, plus every tick-grid input, with 0
// mismatches.
//
// DO NOT rewrite this as nearbyint(x * 10^n) / 10^n. That is a different function: 10^9 is
// representable but the multiply introduces its own rounding before the one you wanted.
//
// Cost is ~200 ns. _floor_tick runs ~500x per quote cycle against a quote-cycle budget of
// about a second, so this is
// ~0.1 ms per cycle. It is not worth optimising; if it ever were, the answer is a memo table
// over the 99 reachable tick values checked against this function, not a closed form.
//
// Requires LC_NUMERIC == "C" so that '.' is the decimal separator. Call
// assert_c_numeric_locale() once at startup.
double py_round_nd(double x, int ndigits);

// Aborts unless LC_NUMERIC is the C locale. Call from main() before any pricing.
void assert_c_numeric_locale();

// --- tick rounding ---------------------------------------------------------------------
//
// strategy/quoting.py:63-68. The inner round(_, 9) is LOAD-BEARING, not defensive: dropping
// it puts the price one full tick low on 6 of the 99 tradeable cent levels, because e.g.
// 0.29 / 0.01 is 28.999999999999996 and floors to 28 rather than 29. Measured cases:
// 0.29, 0.47, 0.57, 0.58, 0.59.
// The int() round-trip is also load-bearing, for a second and separate reason: Python's
// math.floor/math.ceil return an *int*, which has no signed zero. So _ceil_tick(-0.0068) is
// math.ceil(-0.68) -> the int 0 -> 0 * 0.01 -> +0.0, whereas std::ceil(-0.68) is -0.0 and
// -0.0 * 0.01 stays -0.0. Measured: 459 of 50,111 fuzzed ceil_tick cases came back as -0.0
// against Python's +0.0 before this was added. The value is numerically inert downstream, but
// it is a different bit pattern, so it would fail every bit-comparison forever and would show
// up as "-0.0" in the journal.
//
// int64 is exact over the reachable domain: x is a price, so x / 0.01 lies in about [-5, 105].
inline constexpr double TICK = 0.01;
inline double floor_tick(double x) {
    return static_cast<double>(static_cast<std::int64_t>(std::floor(py_round_nd(x / TICK, 9)))) * TICK;
}
inline double ceil_tick(double x) {
    return static_cast<double>(static_cast<std::int64_t>(std::ceil(py_round_nd(x / TICK, 9)))) * TICK;
}

// --- statistics.median ------------------------------------------------------------------
//
// pricing/theo.py:273 and pricing/atmvol.py:188. Sorts a COPY ascending; for even n returns
// the MEAN of the two middle values, (a + b) / 2 -- not nth_element, not the lower middle.
double py_median(std::vector<double> v);

}  // namespace kp::pycompat
