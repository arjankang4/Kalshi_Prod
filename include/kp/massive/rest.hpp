// Massive REST: the key, the query-string encoder, the rate limiters and the two GET idioms.
//
// Port of the transport halves of massive/client.py (`rest`, `_rest_url`) and massive/history.py
// (`RateLimiter`, `get_url`, `get`). They are two idioms deliberately, as in the reference:
//
//   * client.rest    -- the live-path call. One try, no retry: an HTTP error status becomes a
//                       MassiveError carrying the first 300 bytes of the body.
//   * history.get_url -- the paging/backfill call. Rate-limited, and retries 429/5xx and transport
//                       failures five times at 0.5 * 2**attempt seconds; anything else raises
//                       MassiveHistoryError at once.
//
// One `Rest` per process. It is thread-safe provided the HttpGet it holds is (net_http_get() is):
// the limiters are the reference's module-level singletons, so they live here and are shared.
#pragma once
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "kp/json/value.hpp"
#include "kp/massive/transport.hpp"

namespace kp::massive {

class MassiveError : public std::runtime_error {             // massive/client.py
public:
    using std::runtime_error::runtime_error;
};
class MassiveHistoryError : public std::runtime_error {      // massive/history.py
public:
    using std::runtime_error::runtime_error;
};

// `**params` with None-valued entries dropped, in call order. A value of nullopt is Python's
// None: `{k: v for k, v in params.items() if v is not None}`.
using Params = std::vector<std::pair<std::string, std::optional<std::string>>>;

// urllib.parse.urlencode(dict) over already-stringified values: quote_plus on key and value
// (safe set: ASCII letters, digits, "_.-~"; space -> '+'; every other byte %XX upper-case).
//
// A local copy rather than kp::net::urlencode, on purpose: every URL this package builds is
// golden-tested byte for byte against the reference's, and keeping the encoder beside them is
// what lets those tests run without linking the socket layer. The two must agree; the golden
// URLs pin this one.
std::string urlencode(const std::vector<std::pair<std::string, std::string>>& params);

// config._load_env(ROOT / ".env.massive")["MASSIVE_API_KEY"], same parse: strip each line, skip
// blanks, '#' comments and lines without '=', split on the FIRST '=', strip the key, strip the
// value then strip '"' then strip '\''. Throws std::runtime_error if the file or key is missing.
std::string load_api_key(const std::string& kalshi_root);

// history.RateLimiter: a token bucket shared across threads. acquire() reserves the next slot
// (`_next = max(now, _next) + 1/per_sec`) under the lock and sleeps OUTSIDE it.
class RateLimiter {
public:
    RateLimiter(double per_sec, const Clock* clock);
    void acquire();
    double interval() const { return interval_; }

private:
    std::mutex mu_;
    const Clock* clock_;
    double interval_;
    double next_ = 0.0;
};

inline constexpr int MAX_RETRIES = 5;                  // history.MAX_RETRIES
inline constexpr double RATE_PER_SEC = 300.0;          // history.RATE_PER_SEC (/v3/quotes et al.)
inline constexpr double AGG_RATE_PER_SEC = 16.0;       // history.AGG_RATE_PER_SEC (/v2/aggs)
inline constexpr double FUTURES_RATE_PER_SEC = 10.0;   // futures.FUTURES_RATE_PER_SEC

class Rest {
public:
    // `base` is config.MASSIVE_REST_BASE unless a test says otherwise.
    Rest(std::string api_key, HttpGet get, Clock clock, std::string base = "https://api.massive.com");
    Rest(const Rest&) = delete;
    Rest& operator=(const Rest&) = delete;

    // The live wiring: the key from <kalshi_root>/.env.massive, net_http_get(), Clock::system().
    // (Defined in net_adapters.cpp, the one TU that links kp::net.)
    static std::unique_ptr<Rest> live(const std::string& kalshi_root);

    // client.rest(path, **params): `apiKey` appended LAST, after the caller's params.
    kp::json::Value rest(const std::string& path, const Params& params) const;
    // client._rest_url(url): a `next_url` with `apiKey=` appended (unencoded). Unmapped, as in the
    // reference: a non-2xx status throws a plain std::runtime_error("HTTP <code>: ..."), NOT a
    // MassiveError (the reference lets urllib's HTTPError escape).
    kp::json::Value rest_url(const std::string& url) const;

    // history.get_url(url, timeout, limiter): limiter defaults to the 300/s history bucket.
    kp::json::Value get_url(const std::string& url, double timeout_s = 60.0,
                            RateLimiter* limiter = nullptr) const;
    // history.get(path, **params).
    kp::json::Value get(const std::string& path, const Params& params) const;

    RateLimiter& history_limiter() const { return hist_lim_; }
    RateLimiter& agg_limiter() const { return agg_lim_; }
    RateLimiter& futures_limiter() const { return fut_lim_; }

    const std::string& base() const { return base_; }
    const Clock& clock() const { return clock_; }

private:
    std::string key_;
    HttpGet http_;
    Clock clock_;
    std::string base_;
    mutable RateLimiter hist_lim_;
    mutable RateLimiter agg_lim_;
    mutable RateLimiter fut_lim_;
};

}  // namespace kp::massive
