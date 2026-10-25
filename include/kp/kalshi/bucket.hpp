// The PURE half of kalshi/client.py: the write-token bucket, the protocol constants, the batch-size
// rule, and the create-order body. No socket, no clock of its own, no file -- everything here is
// deterministic given its arguments, which is what lets the order path (strategy/orders, budget,
// fastcancel) be golden-tested and what the networked kalshi::Client reuses rather than restates.
//
// FOR THE CLIENT PORTER -- what the reference's `_request` does with these, so the two halves stay
// one rule:
//   * a WRITE (is_write_method) is charged BEFORE the send: `write_bucket.debit(cost)`, where cost
//     is the caller's `tokens` (ORDER_TOKEN_COST * n for a batched create, CANCEL_TOKEN_COST * n
//     for a batched cancel, CANCEL_TOKEN_COST for a single cancel) or DEFAULT_TOKEN_COST;
//   * a write is NEVER retried, on any status. A 429 on a write charges again
//     (`write_bucket.penalise(cost)`) and throws KalshiError(429) immediately;
//   * a READ retries on 429/5xx up to MAX_RETRIES, sleeping 0.4 * 2**attempt.
// `orders::apply` only ASKS the bucket (`can_afford`); it never charges. Charging in exactly one
// place (the transport) is what keeps the mirror honest.
#pragma once
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

#include "kp/json/value.hpp"

namespace kp::kalshi {

using OptD = std::optional<double>;

// --- protocol constants (client.py:59-90, :298, :350) -------------------------------------------
// Every order in a batch is billed separately: 10 tokens per create, 2 per cancel.
inline constexpr long ORDER_TOKEN_COST = 10;
inline constexpr long CANCEL_TOKEN_COST = 2;
// The account's default for any endpoint not overridden (GET /account/endpoint_costs).
inline constexpr long DEFAULT_TOKEN_COST = 10;
inline constexpr int TIER_SAMPLES = 3;
inline constexpr int READ_TIER_SAMPLES = TIER_SAMPLES;
inline constexpr double RATE_PER_SEC = 8.0;            // reads, requests/s, the conservative floor
inline constexpr int MAX_RETRIES = 6;
inline constexpr double WRITE_TOKENS_PER_SEC = 300.0;  // measured tier default
inline constexpr double WRITE_CAPACITY = 900.0;
inline constexpr double READ_RATE_CAP = 20.0;
// max_batch when the account's limits could not be read.
inline constexpr long BATCH_FALLBACK = 20;

// POST, DELETE, PUT, PATCH (case-insensitive, as `method.upper()`).
bool is_write_method(const std::string& method);

// Monotonic seconds. The default reads std::chrono::steady_clock; tests inject a fake.
using Clock = std::function<double()>;
double steady_seconds();

struct BucketDetail {
    double available = 0.0;     // round(available(), 1)
    double capacity = 0.0;
    double refill_rate = 0.0;
    double spent = 0.0;         // round(spent, 1)
    long denied = 0;
    kp::json::Value to_json() const;   // {"available","capacity","refill_rate","spent","denied"}
};

// client.TokenBucket: Kalshi's own accounting mirrored locally -- `capacity` tokens refilling at
// `refill_rate`/s. IT NEVER BLOCKS AND NEVER REFUSES: `debit` always succeeds and may drive the
// level negative (the kill path must never be gated by our own bookkeeping). Callers ASK
// (`available`, `can_afford`); the transport CHARGES (`debit`, `penalise`).
//
// Thread-safe (one mutex, as the reference's threading.Lock). `penalise` increments `denied`
// OUTSIDE the lock exactly as the reference does; here it is under the lock, which is only a
// difference under a data race the reference itself has.
class TokenBucket {
public:
    // Starts FULL, and stamps the clock at construction (the reference's `_t = time.monotonic()`).
    TokenBucket(double capacity, double refill_rate, Clock clock = steady_seconds);

    double available();                    // may be negative (debt)
    bool can_afford(double tokens);        // available() >= tokens
    double debit(double tokens);           // returns the resulting level
    // Re-point at a freshly read tier; keeps the current level, clamped to the new capacity.
    // An absent argument leaves that field alone. (The reference's `capacity or None` is the
    // caller's business: pass nullopt for "leave it".)
    void resize(OptD capacity = std::nullopt, OptD refill_rate = std::nullopt);
    double penalise(double tokens);        // denied += 1, then debit
    BucketDetail detail();

    double capacity();
    double refill_rate();
    double spent();
    long denied();

private:
    double refill_locked(double now);
    double capacity_, refill_rate_, level_, t_;
    Clock clock_;
    std::mutex mu_;
    double spent_ = 0.0;
    long denied_ = 0;
};

// client.fp: float -> Kalshi fixed-point dollar string, f"{x:.{places}f}". Correctly rounded
// (glibc printf and CPython's formatter agree on every finite double); nan/inf spelled as Python
// spells them ("nan", "inf", "-inf").
std::string fp(double x, int places = 4);

// Python float floor division `a // b` (CPython float_floor_div), for the batch-size rule.
double py_floordiv(double a, double b);

// client._max_batch: orders per batched create/cancel request. The smaller of the two TRUTHY
// figures (nonzero, present) // ORDER_TOKEN_COST, at least 1; BATCH_FALLBACK when neither is.
// Measured 2026-08-26: 30 on this account (= refill 300 // 10), NOT capacity 900 // 10.
long max_batch(OptD capacity, OptD refill_rate);

// client.order_body: one create-order body, for the single or the batched endpoint. Key order is
// the reference's dict order and reaches the wire through kp::json::dumps:
//   ticker, side, count, price, time_in_force, self_trade_prevention_type, post_only
//   [, client_order_id  -- only when non-empty, as `if client_order_id:`]
// `price` and `count` are fixed-point STRINGS (fp(price), "20.00") and go in verbatim.
kp::json::Value order_body(const std::string& ticker, const std::string& side,
                           const std::string& price, const std::string& count,
                           const std::string& client_order_id = "",
                           const std::string& time_in_force = "good_till_canceled",
                           bool post_only = true,
                           const std::string& self_trade_prevention_type = "maker");

// The pure half of client.write_budget(): the dict it builds from a GET /account/limits response,
//   {"usage_tier", "bucket_capacity", "refill_rate", "write_req_per_s", "max_batch"}
// with the reference's types (capacities as floats; write_req_per_s null when refill is 0).
// Throws where the reference's float() would raise (the caller then returns the fallback dict,
// {"error": ..., "max_batch": BATCH_FALLBACK}, exactly as write_budget's except clause does).
kp::json::Value write_budget_from_limits(const kp::json::Value& limits);

// An exception that knows the Python class name it stands for. `orders::apply` records a
// non-KalshiError failure as f"{type(e).__name__}: {str(e)[:80]}"; a C++ exception has no such
// name, so the transport throws this (e.g. {"TimeoutError", "timed out"}) where it wants the
// record to read like the reference's. Any other std::exception is recorded as "Exception".
class PyNamedError : public std::runtime_error {
public:
    PyNamedError(std::string type_name, const std::string& msg)
        : std::runtime_error(msg), py_type(std::move(type_name)) {}
    std::string py_type;
};

// type(e).__name__ for the above rule.
std::string py_type_name(const std::exception& e);

// str(s)[:n] -- the first n CODE POINTS of a UTF-8 string (Python slices code points, not bytes).
std::string utf8_prefix(const std::string& s, std::size_t n);

// Python float(x) for the JSON values the order path reads (`reduced_by`, `count_fp`, a limits
// figure): a number as itself, a bool as 0/1, a string through Python's float() grammar
// (surrounding whitespace, `_` between digits, inf/infinity/nan; hex is REJECTED, as Python does).
// Throws std::invalid_argument where Python raises ValueError/TypeError.
double py_float(const kp::json::Value& v);
double py_float_str(const std::string& s);

// Python truthiness of a JSON value: null/false/0/""/[]/{} are falsy.
bool py_truthy(const kp::json::Value& v);

}  // namespace kp::kalshi
