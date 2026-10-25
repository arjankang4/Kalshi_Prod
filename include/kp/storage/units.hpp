// storage/units.py: Kalshi's fixed-point strings -> int64 micro-dollars / centi-contracts.
//
// The reference does this with `Decimal(s).scaleb(n)` and refuses (PrecisionError) anything that is
// not then integral. Two properties of that spelling are load-bearing and reproduced here:
//   - Decimal accepts more than Kalshi sends: surrounding whitespace, a sign, "1.", ".5", exponents,
//     and underscores ANYWHERE (the C decimal module strips them all).
//   - scaleb ROUNDS TO THE CONTEXT PRECISION (28 significant digits, half-even) before the
//     integrality test, so '0.1234560000000000000000000000001' is 123456, not a PrecisionError.
// Not reproduced (documented divergences, none reachable from Kalshi's API): non-ASCII Unicode
// digits (Decimal accepts '١٢'; here it is "not a decimal"), and results outside int64 (Python's
// int is unbounded; here OverflowError).
#pragma once
#include <cstdint>
#include <optional>
#include <string>

#include "kp/json/value.hpp"
#include "kp/storage/pyerr.hpp"

namespace kp::storage::units {

inline constexpr int MICROS = 6;
inline constexpr int CENTI = 2;

// None for None or "" (as the reference). Throws PyError: PrecisionError (a ValueError) for
// syntax or excess precision, OverflowError for +-Infinity, TypeError for a list/dict/bool.
std::optional<std::int64_t> to_micros(const kp::json::Value& v);
std::optional<std::int64_t> to_cc(const kp::json::Value& v);
std::optional<std::int64_t> to_micros(const std::string& s);
std::optional<std::int64_t> to_cc(const std::string& s);

}  // namespace kp::storage::units
