// One pass of the quote loop: run_04_quote_loop.cycle, and the per-underlying Board it walks.
//
// EVERYTHING THE CYCLE READS FROM OUTSIDE GOES THROUGH `CycleIO`. The clocks, the Kalshi book and
// fill feed, the Massive option collector, the index and futures feeds, market discovery and the
// REST fallbacks are all std::function members, so the same cycle runs three ways:
//   live    -- apps/quote_loop fills CycleIO from the real clients and sockets;
//   record  -- the same, wrapped so every value the cycle read is written down;
//   replay  -- CycleIO is filled from such a recording (or a scripted session), which is how the
//              C++ and the Python reference are driven over IDENTICAL inputs for the parity gate.
// Nothing in cycle() opens a socket, sleeps, or reads a clock except through `io`.
//
// THE ORDER OF OPERATIONS IS THE REFERENCE'S, and several of its orderings are load-bearing
// (each is explained where it happens in cycle.cpp): fills before every gate, the halt check
// before the feed gate, the futures anchor beside the option snapshot, delta-adjust between the
// edge manager and the ladder, the risk table after positions and Greeks exist, re-pricing just
// before the ladder, and the write budget between the plan and the risk gate.
#pragma once
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "kp/app/desired.hpp"
#include "kp/json/value.hpp"
#include "kp/kalshi/api.hpp"
#include "kp/kalshi/bucket.hpp"
#include "kp/massive/index.hpp"
#include "kp/massive/snapshot.hpp"
#include "kp/pricing/basis.hpp"
#include "kp/pricing/validate_theo.hpp"
#include "kp/strategy/deltaadj.hpp"
#include "kp/strategy/edgemgr.hpp"
#include "kp/strategy/fastcancel.hpp"
#include "kp/strategy/journal.hpp"
#include "kp/strategy/leanchain.hpp"
#include "kp/strategy/orders.hpp"
#include "kp/strategy/risklimit.hpp"
#include "kp/strategy/risktape.hpp"
#include "kp/strategy/skewstate.hpp"

namespace kp::app {

using Markets = std::vector<kp::pricing::theo::Market>;
using IndexQuotes = std::map<std::string, kp::massive::index::IndexQuote>;

// The option collector's health, as the cycle's feed gate reads it.
struct FeedHealth {
    bool alive = false;
    std::optional<std::string> pump_error;
    OptD quote_age_s;
    kp::json::Value raw;                 // the full health record, for the cycle record
};

struct Board;

struct CycleIO {
    // --- clocks -------------------------------------------------------------------------
    std::function<kp::time::Instant()> now;        // datetime.now(config.ET)
    std::function<double()> mono;                  // time.monotonic()
    std::function<long long()> time_ns;            // time.time_ns()
    std::function<double()> perf;                  // time.perf_counter()

    // --- the Kalshi market-data socket (kalshi.ws.MarketFeed); has_mdfeed=false = --no-ws
    bool has_mdfeed = false;
    std::function<std::vector<kp::json::Value>()> drain_fills;
    std::function<bool()> feed_alive;
    std::function<kp::json::Value()> feed_health;                    // {"coverage", ...}
    std::function<std::optional<std::string>()> feed_pump_error;
    std::function<kp::json::Value(const std::vector<std::string>&)> feed_books;
    std::function<kp::json::Value()> feed_positions;                 // {ticker: float}
    std::function<std::optional<kp::json::Value>(const std::string&)> feed_entry;   // one book

    // --- the account the loss limit marks (risklimit.AccountPoller.latest()); null = no poller
    std::function<std::optional<kp::strategy::risklimit::AccountSnap>()> acct_latest;

    // --- the Massive option collector ---------------------------------------------------
    std::function<FeedHealth()> col_health;
    std::function<bool()> col_reconnect;
    std::function<long()> col_reconnects;
    std::function<kp::massive::OptionSnapshot()> col_snapshot;
    // nullopt = refused (rate-limited); throws over the ticker cap.
    std::function<std::optional<std::pair<std::size_t, std::size_t>>(const std::vector<std::string>&)>
        col_recenter;

    // --- discovery and the REST fallbacks -------------------------------------------------
    // run_04.discover_boards for one board: (daily, hourly), UNfiltered by close_time.
    std::function<std::pair<Markets, Markets>(const Board&)> discover;
    // run_04._index_quotes: the index socket when it has anything, else REST, else {}.
    std::function<IndexQuotes(const std::vector<std::string>&)> index_quotes;
    // k.orderbooks(tickers, depth=10): {ticker: entry}.
    std::function<kp::json::Value(const std::vector<std::string>&)> rest_orderbooks;

    // --- the futures socket (massive.streams.FuturesFeed); has_futures=false = no fast spot
    bool has_futures = false;
    std::function<std::pair<OptD, OptD>(const std::string&)> fut_mid;              // (mid, age)
    std::function<OptD(const std::string&, double, double)> fut_move_scale;
    std::function<kp::json::Value()> fut_health, idx_health;

    // --- the exchange ------------------------------------------------------------------------
    kp::kalshi::TokenBucket* bucket = nullptr;
    kp::kalshi::Api* api = nullptr;                 // only reached when args.live
    kp::strategy::orders::IdSource ids = kp::strategy::orders::uuid4;   // client_order_id source

    // --- the tapes' own health, for the cycle record --------------------------------------
    std::function<kp::json::Value()> tee_detail, spot_tape_detail, guard_detail;

    // --- stderr -----------------------------------------------------------------------------
    std::function<void(const std::string&)> err;
};

// One underlying's worth of everything (run_04.Board): its strip, its models, its futures leg,
// and the per-cycle scratch that is rewritten every pass and read only within it.
struct Board {
    Board(std::string u, std::vector<std::string> daily, std::vector<std::string> hourly);

    std::string underlying;
    std::vector<std::string> daily_series, hourly_series;
    std::string root, expiry;
    std::vector<double> strikes;
    std::vector<std::string> tickers;

    std::unique_ptr<kp::strategy::edgemgr::EdgeManager> em;
    std::unique_ptr<kp::strategy::leanchain::LeanChain> lc;
    std::unique_ptr<kp::strategy::skewstate::SkewState> sk;
    std::unique_ptr<kp::strategy::deltaadj::FuturesTicker> ft;
    std::unique_ptr<kp::strategy::deltaadj::DeltaAdjuster> da;
    // The basis tracker is folded on the INDEX tick in its own thread and read by the cycle and
    // the fast guard: the reference leans on the GIL, here every touch takes trk_mu.
    std::unique_ptr<kp::pricing::basis::BasisTracker> trk;
    std::shared_ptr<std::mutex> trk_mu = std::make_shared<std::mutex>();

    // discovery cache
    std::optional<std::pair<Markets, Markets>> disc;
    double disc_t = 0.0;

    // per-cycle scratch
    Markets daily_m, hourly_m;
    std::optional<kp::pricing::validate_theo::PricedBoards> priced;
    OptD sigma, fast_spot, move_scale;
    kp::json::Value fast_detail;                   // null = {"reason": "no_tracker"} never set
    std::optional<std::string> fast_reason;
    kp::strategy::edgemgr::EdgeMap em_map;
    LeanRows lean_map;
    std::optional<kp::strategy::leanchain::Chain> lean;
    std::string skip;                              // empty = Python None

    void reset();
    Markets markets() const;                       // daily + hourly
    std::vector<std::string> series() const;
    const kp::pricing::spot::SpotResolution* spot_res() const {
        return priced ? &priced->res : nullptr;
    }
    // Board.discover: (daily, hourly) from the cache unless due a refresh, closed markets dropped.
    std::pair<Markets, Markets> discover(CycleIO& io, const kp::time::Instant& now, double mono,
                                         double refresh_s, bool force = false);
    kp::json::Value detail() const;
};

struct CycleArgs {
    bool live = false;
    bool tiers = true;
    bool no_reprice_at_send = false;
};

// What survives BETWEEN cycles and is shared by every board (run_04's `state` dict).
struct CycleState {
    std::vector<std::string> tickers;               // every market ticker at startup
    long max_batch = kp::kalshi::BATCH_FALLBACK;
    kp::strategy::risklimit::SessionRisk* risk = nullptr;
    kp::strategy::fastcancel::FastGuard* guard = nullptr;
    kp::strategy::risktape::RiskTape* risktape = nullptr;
    std::shared_ptr<std::recursive_mutex> apply_lock = std::make_shared<std::recursive_mutex>();
    std::atomic<bool>* stop = nullptr;              // run_04._STOP
};

// What the parity harness compares, beside the printed record. Filled on every path that gets
// far enough to produce it; `planned` is false on the early returns.
struct CycleTrace {
    bool planned = false;
    Fairs fairs;
    kp::strategy::edgemgr::EdgeMap em_map;
    LeanRows lean_map;
    std::set<std::string> chosen;
    kp::strategy::orders::Desired desired;
    Notes notes;
    std::vector<kp::strategy::orders::Order> place, cancel;     // what apply was handed
    kp::json::Value apply;                                       // ApplyResult.to_json()
};

// One pass over every board. Returns the cycle record the loop prints (run_04's dict, same keys).
kp::json::Value cycle(CycleState& state, CycleIO& io, std::vector<Board>& boards,
                      kp::strategy::orders::RestingBook& book, const CycleArgs& args,
                      kp::strategy::journal::Journal* jrnl, CycleTrace* trace = nullptr);

// risklimit fill frames: the reference reads the raw dicts with _f(); this is that conversion.
std::vector<kp::strategy::risklimit::Fill> risk_fills(const std::vector<kp::json::Value>& frames);

// JSON renderings the cycle record uses for types whose ports carry no to_json().
kp::json::Value to_json(const kp::strategy::edgemgr::EdgeSummary& s);
kp::json::Value to_json(const kp::strategy::risklimit::RiskSummary& s);

}  // namespace kp::app
