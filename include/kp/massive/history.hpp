// The parts of massive/history.py the live path reaches: the PM-settled root picker and the
// /v2/aggs pager (index and ETF second bars, for the realized-vol backfill).
//
// The retry/rate-limit transport (`get_url`, `RateLimiter`, `MassiveHistoryError`,
// `AGG_RATE_PER_SEC`) is in kp/massive/rest.hpp. The quotes tape (`quote_pages`, `AsOfIndex`,
// `quote_asof`) and `contracts_for_expiry` are backfill-only and not ported.
#pragma once
#include <optional>
#include <string>
#include <vector>

#include "kp/json/value.hpp"
#include "kp/massive/rest.hpp"

namespace kp::massive::history {

inline constexpr long AGG_PAGE_LIMIT = 50000;

// The PM-settled root of a 0DTE board, from the contracts list: roots are `ticker[2:-15]`
// ("O:SPXW260821C07700000" -> "SPXW"); SPXW then NDXP win outright, a single root is taken as-is,
// no contracts -> nullopt, anything else throws MassiveHistoryError (never an arbitrary pick).
std::optional<std::string> pm_settled_root(const std::vector<kp::json::Value>& contracts);

// history.aggregate_bars: every /v2/aggs bar for `ticker` over [start, end] (ET dates or epoch-ms,
// as strings -- they go into the PATH verbatim), ascending by `t`. Pages on `next_url` until a page
// comes back EMPTY (not until the cursor disappears -- the /v3/quotes lesson), de-duplicates on
// `t` across page boundaries, and stable-sorts on `t`. Each page goes through the 16/s aggregates
// limiter and a 200-with-`status: ERROR` body raises MassiveHistoryError. `t` is epoch MS.
std::vector<kp::json::Value> aggregate_bars(const Rest& rest, const std::string& ticker,
                                            long multiplier, const std::string& timespan,
                                            const std::string& start, const std::string& end,
                                            bool adjusted = true, long limit = AGG_PAGE_LIMIT);

// history.second_bars: 1-second bars over [start_date, end_date], ET calendar dates "YYYY-MM-DD".
// This is what rvol/live backfills the ETF proxy from (`second_bars("SPY", d, d)`).
std::vector<kp::json::Value> second_bars(const Rest& rest, const std::string& ticker,
                                         const std::string& start_date, const std::string& end_date,
                                         bool adjusted = true, long limit = AGG_PAGE_LIMIT);

}  // namespace kp::massive::history
