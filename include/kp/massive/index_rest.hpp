// The network half of massive/index.py: the batched /v3/snapshot/indices read and the 1-second
// history the realized-vol buffer backfills from. The IndexQuote value type is in
// kp/massive/index.hpp (kp_base), so pricing/spot never links this.
#pragma once
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "kp/massive/index.hpp"
#include "kp/massive/rest.hpp"

namespace kp::massive::index {

// The settlement underlying for each Kalshi series family. `I:SPX` -- not `I:GSPC` (empty), and
// emphatically not SPY.
const std::map<std::string, std::string>& INDEX_TICKER();

// index.snapshot(underlyings): {underlying: IndexQuote} in ONE request (`ticker.any_of`, the
// tickers sorted), so SPX and NDX carry one vendor timestamp. An underlying the vendor omits, or
// returns with a null value, is ABSENT -- never present-and-empty. Unknown underlyings are
// ignored; none known -> {} without a request. `ts_ns` is `last_updated`, epoch NANOSECONDS.
// Throws (MassiveHistoryError after get_url's retries) on failure -- run_04's `_index_quotes`
// catches and falls back to {}.
std::map<std::string, IndexQuote> snapshot(const Rest& rest,
                                           const std::vector<std::string>& underlyings = {"SPX", "NDX"});

// index.closes_ms(underlying, day): [(epoch_ms, close)] for one ET session ("YYYY-MM-DD"),
// ascending, non-positive and null closes dropped. The realized-vol buffer's substrate -- this is
// the fetch rvol/live's IndexBuffer.backfill injects. Throws on an unknown underlying (KeyError)
// and on transport failure.
std::vector<std::pair<long long, double>> closes_ms(const Rest& rest, const std::string& underlying,
                                                    const std::string& day);

}  // namespace kp::massive::index
