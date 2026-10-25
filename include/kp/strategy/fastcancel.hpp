// The fast guard: re-price the resting book from the futures, and pull what has gone bad.
//
// Port of strategy/fastcancel.py. IT CANCELS, IT NEVER PLACES: the slow loop (QUOTE_INTERVAL_S) owns vol,
// selection and placement; this owns the forward and the exit. A cancel costs 2 tokens and is a
// REDUCE (never deferred); re-quoting at 200 Hz would forfeit queue position on every rung.
//
// THE MOVE IS APPLIED AS A DELTA, NOT A LEVEL. The daily Pricer keeps the resolved INDEX spot and
// the HourlyPricer atmvol's PARITY forward; shifting both by the same increment preserves that
// distinction. Both are restored afterwards, including when pricing throws.
//
// SEAMS. FastGuard needs three things it does not own, each as a minimal interface here:
//   FuturesFeed    massive.streams.FuturesFeed.mid(u) -> (mid, age_s)
//   SpotTracker    pricing.basis.BasisTracker.spot(futures_mid) -> (fast_spot, detail); being
//                  ported separately as kp::pricing::basis::BasisTracker -- wire it with a
//                  three-line adapter that forwards `spot` and drops nothing the guard reads
//   ShiftablePricer  a pricer whose forward can be moved: DailyShift / HourlyShift below adapt
//                  pricer::Pricer (its `spot`) and hourly::HourlyPricer (its `fit.forward`)
#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "kp/json/value.hpp"
#include "kp/kalshi/api.hpp"
#include "kp/pricing/hourly.hpp"
#include "kp/pricing/pricer.hpp"
#include "kp/pricing/theo_core.hpp"
#include "kp/pycompat/odict.hpp"
#include "kp/strategy/orders.hpp"

namespace kp::strategy::fastcancel {

using OptD = std::optional<double>;
using kp::pricing::theo::Market;

// How often the guard wakes (config.FAST_REPRICE_S, absent from config -> this default) and the
// smallest move, in index points, worth re-pricing for (config.FAST_MIN_MOVE_POINTS -> this default).
inline constexpr double FAST_REPRICE_S = 0.01;
inline constexpr double MIN_MOVE_POINTS = 0.1;

// --- seams ----------------------------------------------------------------------------------

class FuturesFeed {
public:
    virtual ~FuturesFeed() = default;
    // (mid, age_s); mid absent when stale or unknown.
    virtual std::pair<OptD, OptD> mid(const std::string& underlying) = 0;
};

class SpotTracker {
public:
    virtual ~SpotTracker() = default;
    // (fast_spot, detail). fast_spot absent whenever it cannot be justified. The guard ignores
    // `detail`; it is carried so the reference's shape survives the adapter.
    virtual std::pair<OptD, kp::json::Value> spot(OptD futures_mid) = 0;
};

class ShiftablePricer {
public:
    virtual ~ShiftablePricer() = default;
    virtual OptD forward() const = 0;           // Pricer.spot / HourlyPricer.fit.forward
    virtual void set_forward(OptD f) = 0;
    virtual OptD market_fair(const Market& m) = 0;
    virtual kp::pricing::skew::SkewFit* curve() = 0;      // installed_skew(), never built here
};

// pricer::Pricer, by reference (the cycle's own object) or owning a private copy (the guard's).
class DailyShift : public ShiftablePricer {
public:
    explicit DailyShift(kp::pricing::pricer::Pricer& ref) : p_(&ref) {}
    static std::shared_ptr<DailyShift> copy_of(const kp::pricing::pricer::Pricer& p);
    OptD forward() const override { return p_->spot(); }
    void set_forward(OptD f) override { p_->set_spot(f); }
    OptD market_fair(const Market& m) override { return p_->market_fair(m).first; }
    kp::pricing::skew::SkewFit* curve() override { return p_->installed_skew(); }
private:
    std::shared_ptr<kp::pricing::pricer::Pricer> own_;
    kp::pricing::pricer::Pricer* p_;
};

class HourlyShift : public ShiftablePricer {
public:
    explicit HourlyShift(kp::pricing::hourly::HourlyPricer& ref) : p_(&ref) {}
    static std::shared_ptr<HourlyShift> copy_of(const kp::pricing::hourly::HourlyPricer& p);
    OptD forward() const override { return p_->fit().forward; }
    void set_forward(OptD f) override { p_->set_forward(f); }
    OptD market_fair(const Market& m) override { return p_->market_fair(m).first; }
    kp::pricing::skew::SkewFit* curve() override { return p_->installed_skew(); }
private:
    std::shared_ptr<kp::pricing::hourly::HourlyPricer> own_;
    kp::pricing::hourly::HourlyPricer* p_;
};

// {ticker: fair} for `items` with both forwards -- AND each non-flat smile curve's own forward --
// shifted by `move`; restored after, even on a throw.
// `items` is [(market, pricer)] -- each item names its own pricer, as in the reference, and is
// normally `daily` or `hourly`. A null item pricer throws (the reference's AttributeError on None).
// A market whose fair is absent is left out of the result.
using RepriceItem = std::pair<const Market*, ShiftablePricer*>;
kp::pycompat::ODict<std::string, double> reprice_shifted(ShiftablePricer* daily,
                                                         ShiftablePricer* hourly,
                                                         OptD base_daily_fwd, OptD base_hourly_fwd,
                                                         double move,
                                                         const std::vector<RepriceItem>& items);

// --- the guard --------------------------------------------------------------------------------

struct GuardDetail {
    bool ok = false;
    long wakes = 0, repriced = 0, cancels = 0, errors = 0;
    std::optional<std::string> last_error;
    std::vector<std::string> boards;       // sorted
    OptD reprice_ms_p50, reprice_ms_p95;
    double reprice_ms_max = 0.0;
    kp::json::Value to_json() const;
};

using OnCancel = std::function<void(const std::vector<orders::Order>&, const orders::ApplyResult&)>;

class FastGuard {
public:
    // `trackers` {underlying: tracker}; a missing underlying is skipped. `api` is only reached when
    // `live`, and then must be non-null. `apply_lock` is the lock the slow cycle's apply takes (and
    // every other book access -- see orders.hpp); null creates a private one. `max_batch`: see
    // DIVERGENCE below. `clock` is the monotonic clock for `publish`'s stamp.
    //
    // DIVERGENCE. The reference guard calls `orders.apply([], uniq, book, live)` with no max_batch,
    // so every guard cancel burst re-reads GET /account/limits (`k.write_budget()`) -- a network
    // round trip under the apply lock, on the dry path too. Here the caller passes the startup
    // figure once (run_04's `wbudget["max_batch"]`); chunking is otherwise identical.
    FastGuard(FuturesFeed& futures, std::map<std::string, SpotTracker*> trackers,
              orders::RestingBook& book, kp::kalshi::Api* api = nullptr, bool live = false,
              std::shared_ptr<std::recursive_mutex> apply_lock = nullptr,
              OptD interval_s = std::nullopt, OptD min_move = std::nullopt,
              OnCancel on_cancel = nullptr, long max_batch = kp::kalshi::BATCH_FALLBACK,
              kp::kalshi::Clock clock = kp::kalshi::steady_seconds);
    ~FastGuard();
    FastGuard(const FastGuard&) = delete;
    FastGuard& operator=(const FastGuard&) = delete;

    // Install this cycle's pricers and the spot they were priced at. The guard re-prices through
    // the pricers it is handed and MUTATES them (temporarily) from its own thread, so pass objects
    // the slow cycle does not also use -- DailyShift::copy_of / HourlyShift::copy_of. (The
    // reference shares the cycle's objects and races with it under the GIL.) Either may be null;
    // hourly markets are indexed only when `hourly` is non-null, and win over a daily duplicate.
    void publish(const std::string& underlying, std::shared_ptr<ShiftablePricer> daily,
                 std::shared_ptr<ShiftablePricer> hourly, const std::vector<Market>& daily_m,
                 const std::vector<Market>& hourly_m, OptD spot);
    void retire(const std::string& underlying);

    FastGuard& start();
    FastGuard& stop();          // joins (the reference joins with a 3 s timeout)
    bool ok() const;

    // ONE STEP of the loop, synchronous: for each published board with a daily pricer, a base spot
    // and a tracker, read the futures mid, ask the tracker for a fast spot, and if |spot - base| >=
    // min_move re-price the RESTING markets of that board and cancel every order whose edge at the
    // new fair is <= 0 (fair - price on the bid, price - fair on the ask; a None fair is not
    // evidence). Exceptions propagate (the thread loop catches and counts them).
    void tick();

    // De-duplicate on order_id (a None id is dropped), then apply([], uniq) under the apply lock.
    void cancel(const std::vector<orders::Order>& dead);

    GuardDetail detail() const;
    double interval_s() const { return interval_s_; }
    double min_move() const { return min_move_; }
    long wakes() const { return wakes_; }
    long repriced() const { return repriced_; }
    long cancels() const { return cancels_; }
    long errors() const { return errors_; }

private:
    struct BoardState {
        std::string underlying;
        std::shared_ptr<ShiftablePricer> daily, hourly;
        std::unordered_map<std::string, std::pair<Market, ShiftablePricer*>> by_ticker;
        OptD base_spot, base_daily_fwd, base_hourly_fwd;
        double stamp = 0.0;
    };
    std::vector<orders::Order> reprice_board(BoardState& st, double move);
    std::vector<orders::Order> judge_at(const Market& m, OptD fair);
    void run();

    FuturesFeed& futures_;
    std::map<std::string, SpotTracker*> trackers_;
    orders::RestingBook& book_;
    kp::kalshi::Api* api_;
    bool live_;
    std::shared_ptr<std::recursive_mutex> apply_lock_;
    double interval_s_, min_move_;
    OnCancel on_cancel_;
    long max_batch_;
    kp::kalshi::Clock clock_;

    mutable std::mutex lock_;          // boards_
    kp::pycompat::ODict<std::string, std::shared_ptr<BoardState>> boards_;

    std::atomic<long> wakes_{0}, repriced_{0}, cancels_{0}, errors_{0};
    mutable std::mutex stats_;         // last_error_, reprice_ms_, max_reprice_ms_
    std::optional<std::string> last_error_;
    std::vector<double> reprice_ms_;
    double max_reprice_ms_ = 0.0;

    std::mutex stop_mu_;
    std::condition_variable stop_cv_;
    bool stop_ = false;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

}  // namespace kp::strategy::fastcancel
