// The live realized-vol reader: a ring buffer of index levels, and the features read off it.
//
// Port of Kalshi/rvol/live.py.
//
// The quote loop has no second-bar stream -- it polls /v3/snapshot/indices every cycle. So the
// buffer is backfilled ONCE with today's index seconds (and the ETF proxy's extended-hours seconds
// for the pre-open leg), then appended with each polled IndexQuote. Features are read at each
// lookback's OWN fitted scale (dataset::LOOKBACKS), so the polling cadence does not reintroduce an
// Epps bias at that cadence: the estimator sees the scale its coefficient was fitted at.
//
// What this deliberately does NOT do is invent history. A failed backfill leaves the buffer empty
// and the long lookbacks NaN until it fills -- the lean inert rather than wrong.
//
// INJECTED I/O. The reference calls massive.index.closes_ms and massive.history.second_bars over
// the network. Both come in through `Fetch`, and this module never opens a socket. A fetch that
// fails THROWS (any std::exception); backfill catches it exactly where the Python catches its
// `except Exception`.
//
// INJECTED CLOCKS. `now` (the ET instant the Python took from datetime.now(config.ET)) and the
// monotonic reading for the LEAN_RECOMPUTE_S cache are parameters. Pass microsecond-aligned
// instants to reproduce a Python run exactly (datetime has microsecond resolution).
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "kp/massive/index.hpp"
#include "kp/pricing/varcal.hpp"
#include "kp/rvol/model.hpp"
#include "kp/time/et.hpp"

namespace kp::rvol::live {

inline constexpr double BUFFER_HOURS = 3.0;

// rvol.data.SOURCES, the two legs the live path fetches.
struct Source { const char* index; const char* proxy; const char* futures; };
// Throws std::out_of_range for an underlying with no sources (the reference's KeyError, which
// it raises OUTSIDE the try in backfill -- so it propagates rather than failing soft).
Source source_of(const std::string& underlying);

// One /v2/aggs 1-second bar, as much of it as the backfill reads. `c` absent == missing key/None.
struct Bar { std::int64_t t = 0; std::optional<double> c; };

struct Fetch {
    // massive.index.closes_ms(underlying, "YYYY-MM-DD") -> [(epoch_ms, level)], ascending, with
    // zero/negative levels already dropped (that filter is closes_ms's, not this module's).
    std::function<std::vector<std::pair<std::int64_t, double>>(const std::string& underlying,
                                                               const std::string& day)> closes_ms;
    // massive.history.second_bars(ticker, start_date, end_date) -> 1-second OHLC bars.
    std::function<std::vector<Bar>(const std::string& ticker, const std::string& start,
                                   const std::string& end)> second_bars;
};

// Thread-safe (t_ms, level) ring for one underlying, plus the pre-open proxy series.
class IndexBuffer {
public:
    explicit IndexBuffer(std::string underlying, double hours = BUFFER_HOURS);

    // Load today's (ET date of `now`) index seconds and the proxy's extended-hours seconds.
    // Returns backfilled(). On a fetch failure it logs and returns false -- and, as in the
    // reference, if closes_ms succeeded before second_bars failed the index rows STAY loaded.
    bool backfill(const kp::time::Instant& now, const Fetch& fetch);

    // Append one IndexQuote (nullptr == None). Ignores a quote with no timestamp and a repeat or
    // regression of the vendor timestamp; trims to the trailing `hours`. Throws
    // std::invalid_argument for a timestamped quote with no value (the reference raises a
    // TypeError there too, but only after appending the timestamp -- see the .cpp).
    bool append(const kp::massive::index::IndexQuote* quote);

    // Copies of the ring, under the lock.
    std::pair<std::vector<std::int64_t>, std::vector<double>> arrays() const;
    double span_s() const;

    // None until a backfill got as far as the proxy fetch.
    const std::optional<std::vector<std::int64_t>>& proxy_t() const { return proxy_t_; }
    const std::optional<std::vector<double>>& proxy_p() const { return proxy_p_; }
    bool backfilled() const { return backfilled_; }
    const std::string& underlying() const { return underlying_; }

private:
    std::string underlying_;
    double hours_;
    mutable std::mutex mu_;
    std::vector<std::int64_t> t_;
    std::vector<double> p_;
    std::optional<std::vector<std::int64_t>> proxy_t_;
    std::optional<std::vector<double>> proxy_p_;
    bool backfilled_ = false;
};

// Features off the buffer, cached for config::LEAN_RECOMPUTE_S of MONOTONIC time.
class LiveRealizedVol {
public:
    LiveRealizedVol(std::string underlying,
                    std::shared_ptr<const kp::pricing::varcal::VarianceCalendar> calendar,
                    std::shared_ptr<IndexBuffer> buffer, std::string root = "");

    // {"f_3h", "pf_3h", "cov_3h", "f_1h", ...} (NaN where a window is too sparse), or None when
    // the buffer holds fewer than two points. Within LEAN_RECOMPUTE_S of the last computation
    // (by `mono_s`) the CACHED features are returned unchanged -- even though `now` moved, which
    // is the reference's behaviour. Throws if the gap calendar is needed and missing.
    std::optional<model::Features> features(const kp::time::Instant& now, double mono_s,
                                            bool force = false);

    const kp::pricing::varcal::VarianceCalendar& calendar() const { return *cal_; }
    IndexBuffer& buffer() { return *buf_; }

private:
    std::string underlying_, root_;
    std::shared_ptr<const kp::pricing::varcal::VarianceCalendar> cal_;
    std::shared_ptr<IndexBuffer> buf_;
    std::optional<model::Features> cache_;
    double cache_t_ = 0.0;
};

}  // namespace kp::rvol::live
