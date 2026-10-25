// The LIVE delta-adjusted theo: hold vol on the slow cadence, move spot on the fast one.
//
// Port of strategy/deltaadj.py. Between option snapshots a theo is carried forward by
//
//     theo_now = theo_at_snapshot + delta * (F_now - F_at_snapshot)
//
// with `delta` the dollar delta per index point from the edge manager. BOTH ENDS OF THE INCREMENT
// ARE THE SAME INSTRUMENT (a futures mid at the snapshot and a futures mid now), so the ~16.7pt
// futures-cash basis cancels. FAILING SOFT IS THE CONTRACT: no quote, a stale quote, an unknown
// delta or an adjustment past MAX_ADJ_DOLLARS all return the UNADJUSTED theo with a reason.
//
// FuturesTicker polls /futures/v1/quotes/{ticker} on its own thread. The REST call is INJECTED
// (`Fetch`), as are both clocks: `wall` is time.time() (the quote's age is measured against the
// vendor's nanosecond stamp), `mono` is time.monotonic() (the poll pacing). The massive I/O port
// supplies the real fetch; nothing here opens a connection.
#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

#include "kp/json/value.hpp"
#include "kp/massive/futures_pure.hpp"
#include "kp/pycompat/odict.hpp"

namespace kp::strategy::deltaadj {

using OptD = std::optional<double>;

// getattr(config, ..., default): none of the three is set in config, so these are what run.
inline constexpr double FUTURES_POLL_S = 0.5;
inline constexpr double MAX_QUOTE_AGE_S = 2.0;
inline constexpr double MAX_ADJ_DOLLARS = 0.5;

// results[0] of one /futures/v1/quotes page, as far as the ticker reads it.
struct FuturesRow {
    OptD bid_price, ask_price;
    std::optional<long long> timestamp;       // vendor ns; absent -> the reference's KeyError
};

// The reference formats a poll failure as f"{type(e).__name__}: {str(e)[:120]}". A fetch that
// wants a specific type name in `last_error` throws this; any other std::exception is reported
// as "Exception: <what>".
struct FetchError : std::runtime_error {
    std::string type_name;
    FetchError(std::string type, const std::string& msg)
        : std::runtime_error(msg), type_name(std::move(type)) {}
};

// One poll: the newest row, nullopt for an empty `results`, or throw.
using Fetch = std::function<std::optional<FuturesRow>(const std::string& ticker)>;
using Clock = std::function<double()>;
// front_month(product, day) -- normally massive::futures::front_month with the network volume.
using FrontMonth = std::function<std::optional<std::string>(const std::string& product,
                                                            const kp::massive::futures::Date& day)>;

double system_wall();     // time.time()
double system_mono();     // time.monotonic()

struct MidAge { OptD mid, age_s; };

struct TickerDetail {
    std::string product;
    std::optional<std::string> contract;
    bool ok = false;
    OptD mid, age_s;                          // age_s = round(age, 3)
    long polls = 0, errors = 0;
    std::optional<std::string> last_error;
    kp::json::Value to_json() const;
};

// Latest top-of-book mid for one product's front-month contract. Polls on its own thread.
class FuturesTicker {
public:
    FuturesTicker(std::string underlying, Fetch fetch, double poll_s = FUTURES_POLL_S,
                  Clock wall = system_wall, Clock mono = system_mono);
    ~FuturesTicker();
    FuturesTicker(const FuturesTicker&) = delete;
    FuturesTicker& operator=(const FuturesTicker&) = delete;

    // Resolve the front month and begin polling. Never throws: a failed or empty resolution
    // leaves the ticker not ok with `last_error` set, and starts no thread.
    FuturesTicker& start(const kp::massive::futures::Date& day, const FrontMonth& front_month);
    FuturesTicker& stop();
    bool ok() const;

    // One iteration of the poll loop, without the wait. Returns the wait the loop would then
    // do: max(0, poll_s - elapsed). Public so replay and tests can drive the ticker thread-free.
    double poll_once();

    // (mid, age_s) or (MISSING, age_s|MISSING). Age is on the VENDOR's nanosecond stamp; a
    // negative age (clock skew) or one past max_age_s refuses the mid but still reports the age.
    MidAge mid(double max_age_s = MAX_QUOTE_AGE_S) const;

    TickerDetail detail() const;

    const std::string& underlying() const { return underlying_; }
    const std::string& product() const { return product_; }
    const std::optional<std::string>& ticker() const { return ticker_; }
    void set_ticker(std::optional<std::string> t) { ticker_ = std::move(t); }   // tests / replay
    long polls() const { return polls_.load(); }
    long errors() const { return errors_.load(); }
    std::optional<std::string> last_error() const;

private:
    void run();
    void set_error(std::string e);

    std::string underlying_, product_;
    double poll_s_;
    Fetch fetch_;
    Clock wall_, mono_;
    std::optional<std::string> ticker_;
    mutable std::mutex mu_;                   // guards t_ns_/mid_ (the reference's _lock)
    std::optional<long long> t_ns_;
    OptD mid_;
    std::mutex stop_mu_;
    std::condition_variable stop_cv_;
    bool stop_ = false;
    std::thread thread_;
    std::atomic<bool> alive_{false};
    std::atomic<long> polls_{0}, errors_{0};
    mutable std::mutex err_mu_;
    std::optional<std::string> last_error_;
};

struct AdjDetail {
    double delta_adj = 0.0;
    OptD f_anchor, f_now, f_move;
    std::optional<std::string> reason;
    kp::json::Value to_json() const;
};

struct AdjusterDetail {
    long adjusted = 0, unadjusted = 0, refused_size = 0;
    OptD anchor;
    std::optional<TickerDetail> futures;
    kp::json::Value to_json() const;
};

// Anchors on a futures mid at each board snapshot and carries theos forward from it. Holds the
// ticker by pointer (it may be null: "no futures feed for this underlying" fails soft per board).
class DeltaAdjuster {
public:
    explicit DeltaAdjuster(const FuturesTicker* ticker, double max_adj = MAX_ADJ_DOLLARS);

    // Sample the anchor AS CLOSE AS POSSIBLE to taking the option snapshot.
    OptD anchor_now();

    // (theo', detail). `theo` unchanged, with a reason, whenever it cannot adjust.
    std::pair<OptD, AdjDetail> adjust(OptD theo, OptD delta);

    AdjusterDetail detail() const;

    double max_adj() const { return max_adj_; }
    OptD anchor() const { return anchor_; }
    OptD anchor_age_s() const { return anchor_age_s_; }

private:
    const FuturesTicker* ticker_;
    double max_adj_;
    OptD anchor_, anchor_age_s_;
    long adjusted_ = 0, unadjusted_ = 0, refused_size_ = 0;
};

}  // namespace kp::strategy::deltaadj
