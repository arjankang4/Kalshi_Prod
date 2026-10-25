// run_04.build_desired and the book readers it needs (quoting.ladders / top_of_book).
//
// build_desired turns one cycle's fairs, edge records, lean rows, band shapes, positions and the
// resting book into {(ticker, side): [Rung]} plus a per-market NOTE saying why. The note is kept
// as an insertion-ordered json::Value with the reference's keys in the reference's order, because
// it is exactly what the journal rows are read from -- a struct here would be one more place for
// a key to be spelled differently.
//
// THE CHARGE, per side:  entry = max(0, max(0, floor + lean) + bonus + backoff)  micro-dollars.
// The lean is lean::edge_adjust(p_ref, p_rv, side); the backoff is lean::backoff_micros of the
// seconds since this (market, side) was last filled.
#pragma once
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "kp/json/value.hpp"
#include "kp/pricing/theo_core.hpp"
#include "kp/pycompat/odict.hpp"
#include "kp/strategy/band.hpp"
#include "kp/strategy/edgemgr.hpp"
#include "kp/strategy/leanchain.hpp"
#include "kp/strategy/orders.hpp"
#include "kp/strategy/quoting.hpp"

namespace kp::app {

using OptD = std::optional<double>;

// --- quoting.ladders / top_of_book -------------------------------------------------------
// (yes_levels, no_levels) from a Kalshi orderbook payload in any of its shapes: nested under
// `orderbook_fp` (or `orderbook`), with `yes_dollars`/`no_dollars` or bare `yes`/`no`. Levels are
// [price, size] ascending, so the BEST is the LAST entry.
struct TopOfBook { OptD best_bid, best_ask, bid_size, ask_size; };
TopOfBook top_of_book(const kp::json::Value* entry);

// --- one market's pricing detail, as the cycle's `fairs` dict carries it -----------------
// `fair` is what everything downstream quotes off; the rest is provenance for the journal.
struct FairDetail {
    OptD fair;
    std::optional<std::string> reason;          // why fair is None (the skip reason)
    OptD fair_vol, delta_adj, f_move, fair_at_price;
    std::optional<std::string> delta_adj_reason;
    bool has_fair_vol = false;                  // the delta-adjust block ran on this market
};
using Fairs = kp::pycompat::ODict<std::string, FairDetail>;

using Books = std::unordered_map<std::string, kp::json::Value>;
using EdgeLookup = std::function<kp::strategy::edgemgr::EdgeRecord(const std::string&)>;
using LeanRows = kp::pycompat::ODict<std::string, kp::strategy::leanchain::Row>;
using Bands = std::map<std::string, kp::strategy::band::Classified>;
using PositionMap = std::unordered_map<std::string, double>;
using Notes = kp::pycompat::ODict<std::string, kp::json::Value>;

struct DesiredResult {
    kp::strategy::orders::Desired desired;
    Notes notes;
};

// run_04.hourly_cutoff: (refuse, minutes_left). An HOURLY market -- one whose settlement is not
// 16:00 ET -- stops quoting HOURLY_MIN_MINUTES_TO_CLOSE before its own fix. Unknown settlement is
// not refused here.
std::pair<bool, OptD> hourly_cutoff(const kp::pricing::theo::Market& m, const kp::time::Instant& now);

// `now` is the wall clock the hourly cutoff measures against (the reference reads
// datetime.now(timezone.utc) when not given one).
DesiredResult build_desired(const std::vector<kp::pricing::theo::Market>& markets,
                            const Fairs& fairs, const Books* books, const EdgeLookup& em,
                            const LeanRows* lean_map, bool tiers_enabled, const Bands* bands,
                            const PositionMap* positions, kp::strategy::orders::RestingBook& book,
                            double mono_now, const kp::time::Instant& now);

}  // namespace kp::app
