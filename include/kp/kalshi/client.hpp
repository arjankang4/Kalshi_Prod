// kalshi/client.py -> kp::kalshi::Client. The one real implementation of kp::kalshi::Api.
//
// _request's semantics, exactly, because each rule was paid for:
//   - reads are PACED (a next-slot pacer at `read_rate()` req/s, 8 until set_read_budget raises it);
//     writes are NEVER delayed -- they are CHARGED to the write bucket before the send;
//   - a GET is retried on 429/5xx (sleep 0.4 * 2^attempt) and, immediately, on a transport failure
//     of a REUSED keep-alive connection -- up to 6 attempts in all;
//   - a write is NEVER retried, on any status or any transport failure: a retried create is a
//     double-sent order. A 429 on a write additionally PENALISES the bucket by the same cost;
//   - signing covers `{ts}{METHOD}{/trade-api/v2 path}` without the query, re-signed per attempt.
//
// THE WRITE BUCKET IS PLUGGED IN, NOT OWNED. The TokenBucket (kalshi/client.TokenBucket, with its
// ORDER/CANCEL/DEFAULT costs and _max_batch) is ported separately in kp/kalshi/bucket.hpp. Client
// talks to it through `BucketHooks`, and `bind_bucket(b)` adapts any object exposing the reference's
// methods -- debit(tokens), penalise(tokens), resize(optional cap, optional rate), detail() -- so the
// integrator writes one line:
//     client.set_write_bucket(kp::kalshi::bind_bucket(my_token_bucket));
// Until then the client charges nothing (hooks unset), which is the kill path's own policy anyway.
//
// Threading: one Client is shared by every thread; the real transport keeps ONE keep-alive
// connection PER THREAD (the reference's threading.local), the pacer is process-wide under a lock.
#pragma once
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "kp/json/value.hpp"
#include "kp/kalshi/api.hpp"
#include "kp/kalshi/error.hpp"
#include "kp/net/http.hpp"

namespace kp::kalshi {

// Query parameters as the reference passes them: an ORDERED list, Python values (a null value is
// dropped, anything else goes through str(): ints as digits, floats as repr, bools True/False).
using Params = std::vector<std::pair<std::string, kp::json::Value>>;
inline kp::json::Value P(const std::string& s) { return kp::json::Value::make_string(s); }
inline kp::json::Value P(const char* s) { return kp::json::Value::make_string(s); }
inline kp::json::Value P(long long i) { return kp::json::Value::make_int(i); }
inline kp::json::Value P(int i) { return kp::json::Value::make_int(i); }
inline kp::json::Value P(long i) { return kp::json::Value::make_int(i); }

// time.monotonic / time.sleep / time.time, injectable for the golden replay.
struct ClientClock {
    virtual ~ClientClock() = default;
    virtual double monotonic();
    virtual void sleep(double s);
    virtual double time();
    static ClientClock& real();
};

// The request transport. Thread-safe; the real one keeps a keep-alive connection per thread.
// Transport failures throw net::HttpError carrying `reused`.
class Transport {
public:
    virtual ~Transport() = default;
    virtual kp::net::HttpResponse request(const std::string& method, const std::string& target,
                                          const kp::net::Headers& headers, const std::string& body,
                                          double timeout_s) = 0;
};
// libcurl over net::HttpClient, one per thread. `base` is scheme://netloc.
std::shared_ptr<Transport> curl_transport(const std::string& base);

struct BucketHooks {
    std::function<double(double)> debit;                 // charge before a write is sent
    std::function<double(double)> penalise;              // after a 429 on a write
    std::function<void(std::optional<double>, std::optional<double>)> resize;   // (capacity, refill)
    std::function<kp::json::Value()> detail;             // TokenBucket.detail()
};
template <class B>
BucketHooks bind_bucket(B& b) {
    BucketHooks h;
    h.debit = [&b](double t) { return (double)b.debit(t); };
    h.penalise = [&b](double t) { return (double)b.penalise(t); };
    h.resize = [&b](std::optional<double> c, std::optional<double> r) { b.resize(c, r); };
    h.detail = [&b]() { return b.detail(); };
    return h;
}

class Client : public Api {
public:
    // Costs, as kalshi/client.py spells them (bucket.hpp carries the same numbers; these exist so
    // this file compiles without it).
    static constexpr double ORDER_TOKEN_COST = 10;
    static constexpr double CANCEL_TOKEN_COST = 2;
    static constexpr double DEFAULT_TOKEN_COST = 10;
    static constexpr int TIER_SAMPLES = 3;
    static constexpr double READ_RATE_CAP = 20.0;
    static constexpr int MAX_RETRIES = 6;
    static constexpr std::size_t MAX_ORDERBOOK_TICKERS = 100;
    static constexpr long BATCH_FALLBACK = 20;

    struct Config {
        std::string api_base;             // https://host/trade-api/v2 (no trailing slash)
        std::string key_id;
        std::string private_key_path;
    };
    static Config from_env(const std::string& kalshi_root);

    using SignFn = std::function<std::string(const std::string& ts, const std::string& method,
                                             const std::string& path)>;

    // Null transport -> libcurl; null clock -> the real one; null sign -> RSA-PSS with the key file.
    explicit Client(Config cfg, std::shared_ptr<Transport> transport = nullptr,
                    ClientClock* clock = nullptr, SignFn sign = nullptr);

    void set_write_bucket(BucketHooks hooks) { bucket_ = std::move(hooks); }

    // --- the transport contract ----------------------------------------------------------------
    // `path` is relative to /trade-api/v2. `body` null -> no body. Throws KalshiError (non-2xx),
    // net::HttpError (transport), std::runtime_error (a 2xx whose body is not JSON).
    kp::json::Value request(const std::string& method, const std::string& path, const Params& params = {},
                            const kp::json::Value* body = nullptr, bool is_signed = false,
                            double timeout = 20, std::optional<double> tokens = std::nullopt);
    // The signed headers (KEY, SIGNATURE, TIMESTAMP, Content-Type), as _auth_headers.
    kp::net::Headers auth_headers(const std::string& method, const std::string& full_path);

    // --- market data ---------------------------------------------------------------------------
    kp::json::Value get(const std::string& path, const Params& params = {});
    void paged_iter(const std::string& path, const std::string& key, bool is_signed, const Params& params,
                    const std::function<void(const kp::json::Value&)>& each);
    kp::json::Value paged(const std::string& path, const std::string& key, bool is_signed = false,
                          const Params& params = {});
    kp::json::Value markets(const Params& params = {});                          // limit=1000 first
    // {ticker: book} over every chunk of <= 100 tickers (repeated `tickers` params).
    kp::json::Value orderbooks(const std::vector<std::string>& tickers, std::optional<long> depth = std::nullopt);

    // --- portfolio (signed) --------------------------------------------------------------------
    kp::json::Value balance();
    kp::json::Value resting_orders(const Params& params = {});                   // {"orders": [...], "cursor": null}
    // {ticker: float}; {} on failure -- unless `strict`, which RETHROWS: the loss limit must not
    // read "could not read" as "flat" (that marks every position at zero).
    kp::json::Value open_positions(bool strict = false);
    kp::json::Value cancel_order(const std::string& order_id);                   // WRITE, 2 tokens
    kp::json::Value batch_create_orders(const std::vector<kp::json::Value>& orders) override;   // WRITE
    kp::json::Value batch_cancel_orders(const std::vector<std::string>& order_ids) override;    // WRITE
    kp::json::Value fills(const Params& params = {});

    // --- budgets (never raise) ----------------------------------------------------------------
    kp::json::Value endpoint_costs();
    kp::json::Value write_budget();
    kp::json::Value set_write_budget(std::optional<double> refill_rate = std::nullopt, int samples = TIER_SAMPLES);
    kp::json::Value set_read_budget(std::optional<double> refill_rate = std::nullopt, double cap = READ_RATE_CAP,
                                    int samples = TIER_SAMPLES, double share = 1.0);
    double read_rate() const;
    void set_read_rate(double r);

    const Config& config() const { return cfg_; }

private:
    void throttle(const std::string& method);

    Config cfg_;
    std::string base_path_;                 // urlparse(api_base).path, e.g. /trade-api/v2
    std::shared_ptr<Transport> transport_;
    ClientClock* clock_;
    SignFn sign_;
    BucketHooks bucket_;
    mutable std::mutex rate_mu_;
    double rate_per_sec_ = 8.0;             // _RATE_PER_SEC
    double next_slot_ = 0.0;                // _next_slot
};

// f"{type(e).__name__}: {str(e)[:n]}" for the exceptions this client raises.
std::string py_exc_text(const std::exception& e, std::size_t n = 80);
// _max_batch(capacity, refill): max(1, int(min(nonzero figures) // 10)), 20 if neither is set.
// DEFINED in src/kalshi/bucket.cpp (the pure half) -- one definition, declared here for the client.
long max_batch(std::optional<double> capacity, std::optional<double> refill);

}  // namespace kp::kalshi
