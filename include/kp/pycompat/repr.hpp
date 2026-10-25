// Python's float repr(), exactly -- the text every f-string, str() and json.dumps() of a float
// produces, and therefore the text that lands in reason strings, journal rows and log lines.
//
// It matters wherever a number is SPELLED rather than computed: edgemgr's gate reason is
// f"within_{MIN_MINUTES_TO_CLOSE}min_of_close", which Python renders "within_2.0min_of_close" and a
// printf %g renders "within_2min_of_close" -- a different string, so a different journal key.
//
// Shortest round-trip digits (std::to_chars, same guarantee as CPython's dtoa mode 0), laid out by
// CPython's float_repr_style='short' rules: plain notation when -4 <= exponent < 16, otherwise
// d.ddde+XX with at least two exponent digits; an integral value keeps its ".0"; nan/inf spelled
// "nan"/"inf"/"-inf". json.dumps spells the non-finite ones differently -- see py_json_float.
#pragma once
#include <string>

namespace kp::pycompat {

std::string py_float_repr(double x);

// json.dumps(float): repr(), except NaN/Infinity/-Infinity (allow_nan=True, the default).
std::string py_json_float(double x);

}  // namespace kp::pycompat
