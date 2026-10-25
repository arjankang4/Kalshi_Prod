// The resting-order manager: per (market, side) ladder state, and the smallest change that is right.
//
// Port of strategy/orders.py, in full. The planning half (plan_side, reconcile, zombie_held,
// within_risk_limits, reducing_only) is pure; `apply` SENDS, and only ever through
// kp::kalshi::Api -- the dry path (live=false) never touches it, and never even dereferences it.
//
// THE POLICY, in one paragraph (the reference's module docstring has the full argument). An order's
// identity on a side is its PRICE, and Kalshi is price-time, so the default is to leave an order
// alone. Exactly three reasons touch one: (1) negative edge -> cancel, unconditionally; (2) our side
// got better -> improve, FRONT_IMPROVE_TICKS for the front rung and BACK_IMPROVE_TICKS for the
// back ones, held for QUOTE_ZOMBIE_HOLD_S after a negative-edge cancel unless the touch has moved
// in; (3) short a level
// -> top up BEHIND the worst survivor, trimmed so the side never holds more than one ladder.
//
// REPRESENTATION. The reference passes dicts; here an order row is `Order`, with every key the
// order path ever writes. Where the reference distinguishes a missing key from a falsy one, the
// field is optional and its truthiness is spelled out below. Dict insertion order reaches the wire
// (batch chunking) and the per-market sums, so the book is an insertion-ordered ODict, sides are
// vectors, and every float sum goes through `PySum` -- CPython 3.12's `sum()` over floats is
// NEUMAIER-COMPENSATED, not a left fold, and it differs from `+=` in the last bits.
//
// THREADS. The book is not internally synchronised. The fast guard (strategy/fastcancel) reads it
// from its own thread; every access from either thread -- including reconcile, apply_fill and the
// theo/touch stamping -- must hold the shared apply lock. (The reference relies on the GIL here.)
#pragma once
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "kp/json/value.hpp"
#include "kp/kalshi/api.hpp"
#include "kp/kalshi/bucket.hpp"
#include "kp/pycompat/odict.hpp"
#include "kp/strategy/quoting.hpp"

namespace kp::strategy::orders {

using OptD = std::optional<double>;

inline constexpr const char* PLACE = "place";
inline constexpr const char* CANCEL = "cancel";
inline constexpr const char* KEEP = "keep";

// Ticks of improvement required before a resting order is worth replacing -- the requote deadband.
inline constexpr int FRONT_IMPROVE_TICKS = 2;
inline constexpr int BACK_IMPROVE_TICKS = 3;

// Cancels that stand on their own; every other cancel exists only so a matching place can take its
// slot. (REDUCING_CANCEL_REASONS)
bool is_reducing_reason(const std::string& reason);
inline const std::set<std::string>& reducing_cancel_reasons() {
    static const std::set<std::string> s{"edge_negative", "no_longer_quoting"};
    return s;
}

// --- CPython 3.12 sum() over floats ---------------------------------------------------------
// Neumaier's improved Kahan-Babuska summation, exactly as builtin_sum_impl runs it: the compensation
// is added at the end only when it is nonzero and finite. An empty sum is 0.0 (the reference's int 0).
class PySum {
public:
    void add(double x);
    double result() const;
private:
    double f_ = 0.0, c_ = 0.0;
};

// --- one order row --------------------------------------------------------------------------
//
// Keys, and the truthiness the reference gives them:
//   price            absent only on hand-built risk-limit plans; plan_side/apply require it
//   count            fixed-point STRING ("20.00"), verbatim onto the wire; "" = absent (-> 0)
//   order_id         nullopt = missing OR None; "" is present-but-falsy (never sent, never matched).
//                    `order_id_null` records "present, and None" (a dry-run or id-less book row),
//                    which only matters to to_json
//   client_order_id  set by apply on every place it builds a body for
//   dry_run          set on book rows recorded by apply
//   reason / pair    "" = absent
struct Order {
    std::string ticker, side;
    OptD price;
    std::string count;
    std::optional<std::string> order_id;
    bool order_id_null = false;
    std::optional<std::string> client_order_id;
    std::optional<bool> dry_run;
    std::string reason;
    std::string pair;

    bool has_order_id() const { return order_id.has_value() && !order_id->empty(); }   // truthy
    double count_f() const;   // float(o.get("count") or 0)

    // Where client_order_id/dry_run sit in the reference's dict. A book row (apply._record, or any
    // row carrying an id before it is planned) has them BEFORE reason/pair, which plan_side appends
    // with {**o, "reason": ...}; a place gets client_order_id stamped by apply AFTER its
    // reason/pair. apply sets this on the places it stamps.
    bool coid_after_reason = false;

    // The dict as JSON, keys in the reference's order: ticker, side, price, count, order_id, then
    // client_order_id, dry_run, reason, pair -- or reason, pair, client_order_id, dry_run when
    // coid_after_reason. Absent fields are omitted (order_id_null writes "order_id": null).
    kp::json::Value to_json() const;
};

// Dollars of collateral one resting order ties up: p on a bid, 1-p on an ask (a resting offer is
// a NO buy). 0.0 when price or count is absent.
double order_value(const std::string& side, OptD price, const std::string& count);

// f"{ticker}|{side}|{float(price):.4f}" -- links a replacement cancel to its place.
std::string pair_id(const std::string& ticker, const std::string& side, double price);

// sorted(orders, key=-price on the bid / price on the ask), stable.
std::vector<Order> best_first(const std::string& side, std::vector<Order> orders);

// --- per-market state ----------------------------------------------------------------------
struct MarketState {
    explicit MarketState(std::string t) : ticker(std::move(t)) {}
    std::string ticker;
    OptD theo;
    std::vector<Order> bid, ask;
    std::optional<long long> updated_ns;
    double position = 0.0;             // signed net contracts; flat (not unknown) on a cold start
    OptD fill_bid, fill_ask;           // monotonic seconds of the side's last fill (the backoff clock)
    OptD best_bid, best_ask;           // the touch the ladder was last priced against
    OptD zombie_bid, zombie_ask;       // monotonic seconds of the last negative-edge cancel
    OptD zombie_bid_touch, zombie_ask_touch;

    // "bid" -> bid; ANY other string -> ask, as the reference's `if s == "bid" else`.
    std::vector<Order>& side(const std::string& s) { return s == "bid" ? bid : ask; }
    const std::vector<Order>& side(const std::string& s) const { return s == "bid" ? bid : ask; }
    OptD touch(const std::string& s) const { return s == "bid" ? best_bid : best_ask; }
    void set_touch(OptD b, OptD a) { best_bid = b; best_ask = a; }
    OptD last_fill(const std::string& s) const { return s == "bid" ? fill_bid : fill_ask; }
    void mark_fill(const std::string& s, double now) { (s == "bid" ? fill_bid : fill_ask) = now; }
    OptD zombie(const std::string& s) const { return s == "bid" ? zombie_bid : zombie_ask; }
    OptD zombie_at_touch(const std::string& s) const {
        return s == "bid" ? zombie_bid_touch : zombie_ask_touch;
    }
    void arm_zombie(const std::string& s, double now);
    void set_side(const std::string& s, std::vector<Order> orders) { side(s) = std::move(orders); }

    double contracts(const std::optional<std::string>& s = std::nullopt) const;
    double value(const std::optional<std::string>& s = std::nullopt) const;

    // as_record(): {"ticker", "theo", "bid": [[price, count]...], "ask": [...]}, best-first.
    kp::json::Value as_record() const;
};

// Every market's ladder state, in first-touched order.
class RestingBook {
public:
    // Creates the state on first use -- the reference does, and `tickers()` therefore lists every
    // market ever touched (reconcile, apply, apply_fill and the cycle's stamping all touch).
    // The reference is stable across later insertions (states are heap objects).
    MarketState& state(const std::string& ticker);
    const MarketState* find(const std::string& ticker) const;

    std::vector<std::string> resting_tickers() const;     // markets actually holding an order
    std::set<std::pair<std::string, std::string>> resting_sides() const;
    std::vector<std::string> tickers() const;
    std::vector<Order> all_orders() const;
    double contracts(const std::optional<std::string>& ticker = std::nullopt,
                     const std::optional<std::string>& side = std::nullopt) const;
    double value(const std::optional<std::string>& ticker = std::nullopt,
                 const std::optional<std::string>& side = std::nullopt) const;
    std::size_t size() const;                             // len(book): number of resting orders

private:
    kp::pycompat::ODict<std::string, std::shared_ptr<MarketState>> m_;
};

// True when an IMPROVE on this side must wait because we just left a level here. The exception is
// that the touch has MOVED IN since the clock was armed. False whenever it cannot tell.
// `hold_s` absent = config::QUOTE_ZOMBIE_HOLD_S.
bool zombie_held(const std::string& side, OptD touch, OptD zombie_at, OptD zombie_touch, OptD now,
                 OptD hold_s = std::nullopt);

struct SidePlan {
    std::vector<Order> cancels, places, keeps;
};

// (cancels, places, keeps) for one (market, side). `rungs` is the freshly decided ladder
// (best-first), `live` what is already out, `counts` the slot sizes. The zombie inputs are
// arguments so this stays pure; omitting them disables the hold.
SidePlan plan_side(const std::string& ticker, const std::string& side, OptD theo,
                   const std::vector<quoting::Rung>& rungs, const std::vector<Order>& live,
                   const std::vector<std::string>& counts,
                   int front_ticks = FRONT_IMPROVE_TICKS, int back_ticks = BACK_IMPROVE_TICKS,
                   OptD touch = std::nullopt, OptD zombie_at = std::nullopt,
                   OptD zombie_touch = std::nullopt, OptD now = std::nullopt);

// Spacing to use when adding behind the survivors: the ladder's own smallest gap, else one tick.
double tier_step(const std::vector<double>& want);

// Reduce the resting order a WS `fill` frame names; 1 if matched, 0 if not. Matched by order_id.
// A partial fill re-spells the remainder as "%.2f"; a full one removes the row. Arms the side's
// backoff clock at `now` (monotonic seconds) on any match. NOTE: like the reference, it creates a
// MarketState for the frame's `market_ticker` even when nothing matches.
int apply_fill(RestingBook& book, const kp::json::Value& fill, double now);

using SideKey = std::pair<std::string, std::string>;   // (ticker, side)
struct SideKeyHash {
    std::size_t operator()(const SideKey& k) const {
        return std::hash<std::string>()(k.first) * 31u ^ std::hash<std::string>()(k.second);
    }
};
// {(ticker, side): [Rung]} in insertion order -- run_04.build_desired's output.
using Desired = kp::pycompat::ODict<SideKey, std::vector<quoting::Rung>, SideKeyHash>;

struct Reconciled {
    std::vector<Order> to_place, to_cancel, kept;
};

// (to_place, to_cancel, kept) across the board. `counts_by_ticker` / `counts` are the two shapes
// band::resolve_counts accepts (a per-market map, or one tuple; both null = QUOTE_TIER_COUNTS).
// `now` is ONE monotonic reading for the whole board (the zombie hold's clock).
Reconciled reconcile(RestingBook& book, const Desired& desired,
                     const std::map<std::string, std::vector<std::string>>* counts_by_ticker,
                     const std::vector<std::string>* counts, double now);

// The subset of a cancel plan safe to send when the places are BLOCKED.
std::vector<Order> reducing_only(const std::vector<Order>& to_cancel);

// (ok, violations), netting the cancels. The violation strings are the reference's f-strings
// byte for byte. ORDER: the reference iterates a Python SET of keys for the per-side and
// per-market checks, so its order is hash-seeded and not reproducible; here it is first-seen
// order (places, then cancel-only keys). Only `violations[:3]` is ever printed.
std::pair<bool, std::vector<std::string>> within_risk_limits(
    const RestingBook& book, const std::vector<Order>& to_place,
    const std::vector<Order>& to_cancel = {});

// --- apply --------------------------------------------------------------------------------
struct ApplyResult {
    long cancelled = 0, placed = 0;
    std::vector<kp::json::Value> cancel_errors, place_errors;
    bool dry_run = true;
    long rate_limited = 0, unsent_cancels = 0, unsent_places = 0, orphaned_places = 0;
    long zombies_armed = 0, cancel_gone = 0;
    // The reference's dict, keys in its order: cancelled, placed, cancel_errors, place_errors,
    // dry_run, rate_limited, unsent_cancels, unsent_places, orphaned_places, zombies_armed,
    // cancel_gone.
    kp::json::Value to_json() const;
};

// Client order ids. The reference uses str(uuid.uuid4()); injectable so replays can pin them.
using IdSource = std::function<std::string()>;
std::string uuid4();          // random RFC 4122 v4, lowercase 8-4-4-4-12

// Send the plan: cancels first (chunked at max_batch), then places. Returns the reference's result.
//
//   api        the ONLY exit to the exchange. Never touched when live=false (may be null then);
//              live=true with a null api throws std::invalid_argument before anything happens.
//   max_batch  orders per batch request. The reference falls back to a LIVE READ of
//              /account/limits when this is None/0 (`k.write_budget()`, on the dry path too);
//              here <= 0 means kalshi::BATCH_FALLBACK and nothing is read. run_04 always passes
//              its startup figure.
//   budget     the write bucket, ASKED (can_afford) before each chunk and never charged -- the
//              transport charges. Null = unmetered (the kill path). A chunk it cannot afford is
//              DECLINED: left unsent, not an error. A 429 is REJECTED: recorded, and the loop
//              stops (never retried, so never double-sent). Any other failure is recorded and the
//              remaining chunks still go.
//   now        monotonic seconds, stamped on zombie holds armed by landed edge_negative cancels.
//
// A place whose paired cancel did not land is dropped (`orphaned_places`) -- it would double the
// rung. A cancel answered `not_found` leaves the local book (`cancel_gone`) without counting as
// `cancelled`. Only confirmed cancels (reduced_by > 0, or not_found) leave the local book.
// `to_place` is taken by value: the reference stamps client_order_id onto the caller's dicts, and
// nothing reads them back.
ApplyResult apply(std::vector<Order> to_place, const std::vector<Order>& to_cancel,
                  RestingBook& book, kp::kalshi::Api* api, bool live, long max_batch,
                  kp::kalshi::TokenBucket* budget, double now, const IdSource& ids = uuid4);

// Python-format helpers, exposed for the tests: f"{x:.{p}f}" and f"{x:,.2f}".
std::string fmt_fixed(double x, int places);
std::string fmt_thousands2(double x);

}  // namespace kp::strategy::orders
