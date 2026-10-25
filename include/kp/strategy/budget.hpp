// Write-budget admission and capital selection. Port of strategy/budget.py, in full.
//
// TWO RESOURCES, TWO FUNCTIONS, one ranking number:
//   admit           rations WRITES within one cycle. Exposure-reducing cancels (edge_negative,
//                   no_longer_quoting) are NEVER deferred, even into debt; a replacement cancel is
//                   admitted only WITH its paired place (the PAIR is the unit). Everything else is
//                   greedy in priority order REDUCE < BACKOFF < IMPROVE < COLD < TOPUP, then urgency
//                   descending, then ticker, then side -- a stable sort, never a knapsack.
//   select_markets  rations CAPITAL across the session: two passes (band markets get
//                   BAND_CAPITAL_FRACTION of the caps, wings the rest), ranked by `risk_u` (R's
//                   percentile within its own underlying) with R as the fallback, and incumbents
//                   retained while their rank holds at SELECT_HYSTERESIS of the marginal entrant's.
//
// Pure: no network, no clock, no module state.
#pragma once
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "kp/json/value.hpp"
#include "kp/pricing/theo_core.hpp"
#include "kp/pycompat/odict.hpp"
#include "kp/strategy/edgemgr.hpp"
#include "kp/strategy/orders.hpp"

namespace kp::strategy::budget {

using OptD = std::optional<double>;
using orders::Order;

enum Cls : int { REDUCE = 0, BACKOFF = 1, IMPROVE = 2, COLD = 3, TOPUP = 4 };
const char* class_name(int cls);   // "reduce" | "backoff" | "improve" | "cold" | "topup"

// What budget reads off an edge-manager record. The reference reads `risk_u`, `R` and `quotable`
// off a dict with `.get`, so each may be missing; `edge_views` adapts the live EdgeMap (where
// risk_u is always present). A ticker absent from the map reads as {} (quotable, no R).
struct EdgeView {
    OptD risk_u, R;
    std::optional<bool> quotable;     // absent = True, as `d.get("quotable", True)`
};
using EdgeViews = std::unordered_map<std::string, EdgeView>;
EdgeViews edge_views(const edgemgr::EdgeMap& em);

// {ticker: fair or None}. A ticker absent from the map is None.
using Theos = std::unordered_map<std::string, OptD>;
using SideSet = std::set<std::pair<std::string, std::string>>;

// One admissible unit: a solo cancel, a solo place, or a linked cancel+place pair.
struct Item {
    int cls = TOPUP;
    std::vector<Order> cancels, places;
    long cost = 0;              // 2 per cancel + 10 per place
    double urgency = 0.0;
    std::string ticker, side;   // off the first cancel, else the first place
    kp::json::Value record() const;   // {"cls","ticker","side","cost","urgency"(4dp),"n_cancel","n_place"}
};

// Group a raw plan into units, unsorted. `resting` null = "the caller did not say" (every solo
// place is a TOPUP); an EMPTY set means nothing rests anywhere (every solo place is COLD).
std::vector<Item> build_items(const std::vector<Order>& to_place,
                              const std::vector<Order>& to_cancel, const Theos* theos = nullptr,
                              const EdgeViews* em = nullptr, const SideSet* resting = nullptr);

// Item.key() order, stable.
void sort_items(std::vector<Item>& items);

struct AdmitDetail {
    double tokens_in = 0, tokens_left = 0, reserve = 0, cost = 0, reduce_cost = 0;
    bool reduce_over_budget = false;
    long n_admitted = 0, n_deferred = 0;
    long deferred_cost = 0;                     // a sum of int costs: an int, as in the reference
    long by_class[5] = {0, 0, 0, 0, 0};
    long deferred_by_class[5] = {0, 0, 0, 0, 0};
    std::vector<kp::json::Value> deferred_sample;   // record() of the first five deferred
    // Keys in the reference's order. `reserve` is written as a float (the config default is one).
    kp::json::Value to_json() const;
};

struct Admitted {
    std::vector<Order> place, cancel;
    AdmitDetail detail;
};

// `tokens` is write_bucket.available() now; `reserve` absent = config::WRITE_RESERVE_TOKENS.
Admitted admit(const std::vector<Order>& to_place, const std::vector<Order>& to_cancel,
               double tokens, const Theos* theos = nullptr, const EdgeViews* em = nullptr,
               OptD reserve = std::nullopt, const SideSet* resting = nullptr);

// --- capital selection ------------------------------------------------------------------------

// Collateral one market's ladder ties up: sum over sides x counts of orders::order_value.
double ladder_value(double fair, const std::vector<std::string>& counts,
                    const std::vector<std::string>& sides = {"bid", "ask"});

// The operator-named moneyness floor, as a reason string, or nullopt.
std::optional<std::string> moneyness_skip(const std::string& strike_type, OptD fair);

struct PassDetail {
    long candidates = 0, chosen = 0, retained = 0, unaffordable = 0;
    double value = 0, budget = 0, cut_R = 0, keep_R = 0;   // already rounded (2, 2, 6, 6)
};

struct SelectDetail {
    long universe = 0, candidates = 0, chosen = 0;
    PassDetail band, wing;
    long retained = 0, added = 0, dropped_incumbents = 0;
    double value = 0, max_value = 0, contracts = 0, max_contracts = 0;
    bool max_contracts_is_int = true;   // the config default is a Python int (5000)
    kp::pycompat::ODict<std::string, long> skipped;   // reason -> count, first-seen order
    long unaffordable = 0;
    kp::json::Value to_json() const;
};

struct Selection {
    std::set<std::string> chosen;
    SelectDetail detail;
};

// `counts_by_ticker` / `counts`: the two shapes band::resolve_counts takes (both null =
// QUOTE_TIER_COUNTS). A market is a BAND candidate when its counts have more than one rung.
Selection select_markets(const std::vector<kp::pricing::theo::Market>& markets, const Theos* theos,
                         const EdgeViews* em,
                         const std::map<std::string, std::vector<std::string>>* counts_by_ticker,
                         const std::vector<std::string>* counts,
                         const std::set<std::string>& incumbents = {},
                         OptD max_value = std::nullopt, OptD max_contracts = std::nullopt,
                         OptD hysteresis = std::nullopt, OptD band_fraction = std::nullopt);

}  // namespace kp::strategy::budget
