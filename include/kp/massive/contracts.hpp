// massive/client.py's contracts reference, and pricing/universe.option_root_and_strikes -- the
// network half of the strike-grid discovery. It lives here rather than in src/pricing because
// kp_pricing must never link kp_net (the reference keeps pricing/ free of I/O the same way).
#pragma once
#include <optional>
#include <string>
#include <vector>

#include "kp/json/value.hpp"
#include "kp/massive/rest.hpp"

namespace kp::massive::client {

// client.option_contracts(underlying, expiration_date): every contract for one expiry, following
// `next_url` until it is absent. `underlying` is a PARAMETER with no default -- the reference binds
// `config.OPTION_UNDERLYING` as a default argument at import time, which silently hands an NDX
// caller the SPX strip; that trap is not reproduced. `expiration_date` "YYYY-MM-DD" or nullopt.
std::vector<kp::json::Value> option_contracts(const Rest& rest, const std::string& underlying,
                                              const std::optional<std::string>& expiration_date);

// pricing/universe.option_root_and_strikes: the PM-settled root, its sorted distinct strikes, and
// the contracts filtered to that root -- IN THAT ORDER. On monthly opex an expiry lists SPX beside
// SPXW (NDX beside NDXP); Kalshi settles on the 4pm close, so the AM root prices the wrong event.
struct RootStrikes {
    std::optional<std::string> root;          // nullopt iff the vendor listed no contracts
    std::vector<double> strikes;               // sorted({c["strike_price"]})
    std::vector<kp::json::Value> contracts;    // filtered to `root`
};
RootStrikes option_root_and_strikes(const Rest& rest, const std::string& expiry_date,
                                    const std::string& underlying);

}  // namespace kp::massive::client
