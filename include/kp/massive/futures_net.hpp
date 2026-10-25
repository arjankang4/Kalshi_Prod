// The network half of massive/futures.py -- what the live path executes of it.
//
// The pure half (PRODUCT, quarterly_candidates, third_friday, front_month's selection rule) is
// owned elsewhere (kp/massive/futures_pure.hpp). front_month needs ONE network primitive,
// `session_volume`, and is meant to take it injected:
//
//     futures_pure::front_month(product, day, n, incumbent,
//         [&](const std::string& tk, int y, int m, int d) { return session_volume(rest, tk, y, m, d); });
//
// strategy/deltaadj.FuturesTicker's poll is `quote_page(rest, contract)` (= `fut._get(
// "/futures/v1/quotes/{ticker}", limit=1)`); it reads `results[0]`'s bid_price/ask_price/timestamp.
#pragma once
#include <string>
#include <vector>

#include "kp/json/value.hpp"
#include "kp/massive/futures_pure.hpp"
#include "kp/massive/rest.hpp"

namespace kp::massive::futures {

// PAGE_LIMIT and MIN_SPLIT_NS live in futures_pure.hpp (one definition for both halves).

// futures._get(path, **params): one GET through the 10/s futures limiter (with get_url's retries);
// a 200 whose body says `status: ERROR` raises MassiveHistoryError.
kp::json::Value get(const Rest& rest, const std::string& path, const Params& params);

// The FuturesTicker poll: newest top-of-book quote page for `contract` (e.g. "ESZ6"). limit=1 and
// the vendor's own descending order give the newest quote in one row; never ask for order=asc.
kp::json::Value quote_page(const Rest& rest, const std::string& contract);

// futures.aggs: bars over [start_ns, end_ns), ascending by window_start (NANOSECONDS). Never
// follows the vendor cursor (it 502s and drops the upper bound); a saturated page BISECTS the
// range instead. De-duplicated on window_start, stable-sorted, and the returned span is asserted
// inside the requested window (MassiveHistoryError if the vendor ignored the filter). `resolution`
// must be one of 1sec/1min/1second/1minute -- anything else throws std::invalid_argument, since
// the vendor answers it with a 400 that reads like a dead endpoint.
std::vector<kp::json::Value> aggs(const Rest& rest, const std::string& ticker,
                                  const std::string& resolution, long long start_ns,
                                  long long end_ns, long limit = PAGE_LIMIT);

// futures.session_volume: total RTH (09:30-16:00 ET) volume of one contract on one ET date, from
// 1-minute bars -- the front-month discriminator. Python's sum() semantics: exact integer sum while
// every volume is an int, compensated float sum once one is not; returned as a double.
double session_volume(const Rest& rest, const std::string& ticker, int year, int month, int day);

}  // namespace kp::massive::futures
