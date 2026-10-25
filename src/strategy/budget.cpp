#include "kp/strategy/budget.hpp"

#include <algorithm>
#include <cmath>

#include "kp/config.hpp"
#include "kp/kalshi/bucket.hpp"
#include "kp/pycompat/repr.hpp"
#include "kp/pycompat/round.hpp"
#include "kp/strategy/band.hpp"
#include "kp/strategy/quoting.hpp"

namespace kp::strategy::budget {

using kp::json::Value;
using kp::pycompat::py_round_nd;
using orders::PySum;

const char* class_name(int cls) {
    switch (cls) {
        case REDUCE: return "reduce";
        case BACKOFF: return "backoff";
        case IMPROVE: return "improve";
        case COLD: return "cold";
        case TOPUP: return "topup";
    }
    return "?";
}

EdgeViews edge_views(const edgemgr::EdgeMap& em) {
    EdgeViews out;
    for (const auto& [t, r] : em) out[t] = EdgeView{r.risk_u, r.R, r.quotable};
    return out;
}

namespace {

const EdgeView* view_of(const EdgeViews* em, const std::string& t) {
    if (em == nullptr) return nullptr;
    const auto it = em->find(t);
    return it == em->end() ? nullptr : &it->second;
}

OptD theo_of(const Theos* theos, const std::string& t) {
    if (theos == nullptr) return std::nullopt;
    const auto it = theos->find(t);
    return it == theos->end() ? std::nullopt : it->second;
}

double reduce_urgency(const Order& c, const Theos* theos) {
    const double n = c.count_f();
    const OptD theo = theo_of(theos, c.ticker);
    if (!theo || !c.price) return n;
    const long em = quoting::edge_micros(c.side, *theo, *c.price);
    const long neg = em < 0 ? em : 0;                      // min(0, em)
    return (double)(neg < 0 ? -neg : neg) / (double)quoting::MICROS * n;
}

double risk_urgency(const std::vector<const Order*>& rows, const EdgeViews* em) {
    const EdgeView* d = view_of(em, rows[0]->ticker);
    if (d != nullptr && d->risk_u) return *d->risk_u;
    if (d != nullptr && d->R) return *d->R;
    PySum s;
    for (const Order* x : rows) s.add(x->count_f());
    return s.result() / 100.0;
}

Item make_item(int cls, std::vector<Order> cancels, std::vector<Order> places, double urgency) {
    Item it;
    it.cls = cls;
    it.cancels = std::move(cancels);
    it.places = std::move(places);
    it.cost = (long)it.cancels.size() * kp::kalshi::CANCEL_TOKEN_COST
              + (long)it.places.size() * kp::kalshi::ORDER_TOKEN_COST;
    it.urgency = urgency;
    const Order& row = it.cancels.empty() ? it.places.at(0) : it.cancels[0];
    it.ticker = row.ticker;
    it.side = row.side;
    return it;
}

}  // namespace

Value Item::record() const {
    Value v;
    v.set("cls", Value::make_string(class_name(cls)));
    v.set("ticker", Value::make_string(ticker));
    v.set("side", Value::make_string(side));
    v.set("cost", Value::make_int(cost));
    v.set("urgency", Value::make_number(py_round_nd(urgency, 4)));
    v.set("n_cancel", Value::make_int((long long)cancels.size()));
    v.set("n_place", Value::make_int((long long)places.size()));
    return v;
}

std::vector<Item> build_items(const std::vector<Order>& to_place,
                              const std::vector<Order>& to_cancel, const Theos* theos,
                              const EdgeViews* em, const SideSet* resting) {
    kp::pycompat::ODict<std::string, std::vector<Order>> by_pair;
    std::vector<Order> solo;
    for (const auto& p : to_place) {
        if (!p.pair.empty()) by_pair[p.pair].push_back(p);
        else solo.push_back(p);
    }
    std::vector<Item> items;
    for (const auto& c : to_cancel) {
        if (orders::is_reducing_reason(c.reason)) {
            items.push_back(make_item(REDUCE, {c}, {}, reduce_urgency(c, theos)));
            continue;
        }
        const std::vector<Order>* mates = c.pair.empty() ? nullptr : by_pair.find(c.pair);
        if (mates == nullptr || mates->empty()) continue;
        std::vector<const Order*> rows{&c};
        for (const auto& m : *mates) rows.push_back(&m);
        const double u = risk_urgency(rows, em);
        items.push_back(make_item(IMPROVE, {c}, *mates, u));
    }
    const bool known = resting != nullptr;
    for (const auto& p : solo) {
        const bool cold = known && !resting->count({p.ticker, p.side});
        items.push_back(make_item(cold ? COLD : TOPUP, {}, {p}, risk_urgency({&p}, em)));
    }
    return items;
}

void sort_items(std::vector<Item>& items) {
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        if (a.cls != b.cls) return a.cls < b.cls;
        const double ua = -a.urgency, ub = -b.urgency;
        if (ua < ub) return true;
        if (ub < ua) return false;
        if (a.ticker != b.ticker) return a.ticker < b.ticker;
        return a.side < b.side;
    });
}

Value AdmitDetail::to_json() const {
    Value v;
    v.set("tokens_in", Value::make_number(tokens_in));
    v.set("tokens_left", Value::make_number(tokens_left));
    v.set("reserve", Value::make_number(reserve));
    v.set("cost", Value::make_number(cost));
    v.set("reduce_cost", Value::make_number(reduce_cost));
    v.set("reduce_over_budget", Value::make_bool(reduce_over_budget));
    v.set("n_admitted", Value::make_int(n_admitted));
    v.set("n_deferred", Value::make_int(n_deferred));
    v.set("deferred_cost", Value::make_int(deferred_cost));
    Value bc, dc;
    for (int c = REDUCE; c <= TOPUP; ++c) {
        bc.set(class_name(c), Value::make_int(by_class[c]));
        dc.set(class_name(c), Value::make_int(deferred_by_class[c]));
    }
    v.set("by_class", bc);
    v.set("deferred_by_class", dc);
    v.set("deferred_sample", Value::make_array(deferred_sample));
    return v;
}

Admitted admit(const std::vector<Order>& to_place, const std::vector<Order>& to_cancel,
               double tokens, const Theos* theos, const EdgeViews* em, OptD reserve_opt,
               const SideSet* resting) {
    const double reserve = reserve_opt ? *reserve_opt : kp::config::WRITE_RESERVE_TOKENS;
    std::vector<Item> items = build_items(to_place, to_cancel, theos, em, resting);
    sort_items(items);

    Admitted out;
    std::vector<const Item*> admitted, deferred;
    double budget = tokens;
    double spent_reduce = 0.0;
    for (const auto& it : items) {
        if (it.cls == REDUCE) {
            for (const auto& c : it.cancels) out.cancel.push_back(c);
            admitted.push_back(&it);
            budget -= (double)it.cost;
            spent_reduce += (double)it.cost;
            continue;
        }
        if ((double)it.cost > budget - reserve) {
            deferred.push_back(&it);
            continue;
        }
        budget -= (double)it.cost;
        for (const auto& c : it.cancels) out.cancel.push_back(c);
        for (const auto& p : it.places) out.place.push_back(p);
        admitted.push_back(&it);
    }

    AdmitDetail& d = out.detail;
    d.tokens_in = py_round_nd(tokens, 1);
    d.tokens_left = py_round_nd(budget, 1);
    d.reserve = reserve;
    d.cost = py_round_nd(tokens - budget, 1);
    d.reduce_cost = py_round_nd(spent_reduce, 1);
    d.reduce_over_budget = spent_reduce > tokens;
    d.n_admitted = (long)admitted.size();
    d.n_deferred = (long)deferred.size();
    for (const Item* i : deferred) d.deferred_cost += i->cost;
    for (const Item* i : admitted) d.by_class[i->cls] += 1;
    for (const Item* i : deferred) d.deferred_by_class[i->cls] += 1;
    for (std::size_t k = 0; k < deferred.size() && k < 5; ++k)
        d.deferred_sample.push_back(deferred[k]->record());
    return out;
}

// --- capital selection ------------------------------------------------------------------------

double ladder_value(double fair, const std::vector<std::string>& counts_in,
                    const std::vector<std::string>& sides) {
    std::vector<std::string> counts = counts_in;
    if (counts.empty())
        for (const auto& c : kp::config::QUOTE_TIER_COUNTS) counts.emplace_back(c);
    PySum s;
    for (const auto& side : sides)
        for (const auto& c : counts) s.add(orders::order_value(side, fair, c));
    return s.result();
}

std::optional<std::string> moneyness_skip(const std::string& strike_type, OptD fair) {
    if (!fair) return std::string("no_fair");
    if (strike_type == "between") {
        if (*fair <= kp::config::SELECT_FLOOR_BRACKET)
            return "bracket_fair<=" + kp::pycompat::py_float_repr(kp::config::SELECT_FLOOR_BRACKET);
        return std::nullopt;
    }
    if (strike_type == "greater" || strike_type == "greater_or_equal" || strike_type == "less"
        || strike_type == "less_or_equal") {
        const double lo = kp::config::SELECT_FLOOR_ONE_SIDED;
        if (*fair <= lo || *fair >= 1.0 - lo)
            return "one_sided_fair_outside_" + kp::pycompat::py_float_repr(lo) + "-"
                   + kp::pycompat::py_float_repr(1 - lo);
        return std::nullopt;
    }
    return std::nullopt;
}

namespace {

struct Cand { double r; std::string t; double v, c; };

struct Filled {
    std::vector<std::string> chosen;
    double value = 0.0, contracts = 0.0, cut_r = 0.0, keep_r = 0.0;
    long n_retained = 0;
};

Filled fill(const std::vector<Cand>& cands, const std::set<std::string>& incumbents,
            double max_value, double max_contracts, double hysteresis) {
    Filled f;
    double cut_r = 0.0, val = 0.0, n = 0.0;
    for (const auto& x : cands) {
        if (val + x.v > max_value || n + x.c > max_contracts) break;
        val += x.v;
        n += x.c;
        cut_r = x.r;
    }
    const double keep_r = cut_r * hysteresis;
    auto take = [&](const Cand& x) {
        if (f.value + x.v > max_value || f.contracts + x.c > max_contracts) return false;
        f.chosen.push_back(x.t);
        f.value += x.v;
        f.contracts += x.c;
        return true;
    };
    for (const auto& x : cands)
        if (incumbents.count(x.t) && x.r >= keep_r) take(x);
    f.n_retained = (long)f.chosen.size();
    const std::set<std::string> held(f.chosen.begin(), f.chosen.end());
    for (const auto& x : cands) {
        if (held.count(x.t)) continue;
        if (!take(x)) break;
    }
    f.cut_r = cut_r;
    f.keep_r = keep_r;
    return f;
}

Value pass_json(const PassDetail& p) {
    Value v;
    v.set("candidates", Value::make_int(p.candidates));
    v.set("chosen", Value::make_int(p.chosen));
    v.set("retained", Value::make_int(p.retained));
    v.set("value", Value::make_number(p.value));
    v.set("budget", Value::make_number(p.budget));
    v.set("cut_R", Value::make_number(p.cut_R));
    v.set("keep_R", Value::make_number(p.keep_R));
    v.set("unaffordable", Value::make_int(p.unaffordable));
    return v;
}

}  // namespace

Value SelectDetail::to_json() const {
    Value v;
    v.set("universe", Value::make_int(universe));
    v.set("candidates", Value::make_int(candidates));
    v.set("chosen", Value::make_int(chosen));
    v.set("band", pass_json(band));
    v.set("wing", pass_json(wing));
    v.set("retained", Value::make_int(retained));
    v.set("added", Value::make_int(added));
    v.set("dropped_incumbents", Value::make_int(dropped_incumbents));
    v.set("value", Value::make_number(value));
    v.set("max_value", Value::make_number(max_value));
    v.set("contracts", Value::make_number(contracts));
    v.set("max_contracts", max_contracts_is_int ? Value::make_int((long long)max_contracts)
                                                : Value::make_number(max_contracts));
    Value sk = Value::make_object({});
    for (const auto& [k, n] : skipped) sk.set(k, Value::make_int(n));
    v.set("skipped", sk);
    v.set("unaffordable", Value::make_int(unaffordable));
    return v;
}

Selection select_markets(const std::vector<kp::pricing::theo::Market>& markets, const Theos* theos,
                         const EdgeViews* em,
                         const std::map<std::string, std::vector<std::string>>* counts_by_ticker,
                         const std::vector<std::string>* counts,
                         const std::set<std::string>& incumbents, OptD max_value_opt,
                         OptD max_contracts_opt, OptD hysteresis_opt, OptD band_fraction_opt) {
    const double max_value = max_value_opt ? *max_value_opt : kp::config::MAX_RESTING_ORDER_VALUE;
    const double max_contracts =
        max_contracts_opt ? *max_contracts_opt : kp::config::MAX_RESTING_CONTRACTS_TOTAL;
    const double hysteresis = hysteresis_opt ? *hysteresis_opt : kp::config::SELECT_HYSTERESIS;
    const double band_fraction =
        band_fraction_opt ? *band_fraction_opt : kp::config::BAND_CAPITAL_FRACTION;

    Selection out;
    SelectDetail& d = out.detail;
    std::vector<Cand> band_c, wing_c;
    auto skip = [&](const std::string& why) {
        const long* cur = d.skipped.find(why);
        d.skipped.set(why, (cur ? *cur : 0) + 1);
    };
    for (const auto& m : markets) {
        const std::string& t = m.ticker;
        const OptD fair = theo_of(theos, t);
        const auto why = moneyness_skip(m.strike_type, fair);
        if (why) { skip(*why); continue; }
        const EdgeView* ev = view_of(em, t);
        if (ev != nullptr && ev->quotable && !*ev->quotable) { skip("not_quotable"); continue; }
        if (ev == nullptr || !ev->R) { skip("no_R"); continue; }
        const double rank = ev->risk_u ? *ev->risk_u : *ev->R;
        const std::vector<std::string> cn = band::resolve_counts(counts_by_ticker, counts, t);
        PySum cs;
        for (const auto& c : cn) cs.add(kp::kalshi::py_float_str(c));
        Cand row{rank, t, ladder_value(*fair, cn), 2.0 * cs.result()};
        (cn.size() > 1 ? band_c : wing_c).push_back(std::move(row));
    }
    auto by_rank = [](const Cand& a, const Cand& b) {
        const double ka = -a.r, kb = -b.r;
        if (ka < kb) return true;
        if (kb < ka) return false;
        return a.t < b.t;
    };
    std::stable_sort(band_c.begin(), band_c.end(), by_rank);
    std::stable_sort(wing_c.begin(), wing_c.end(), by_rank);

    const double bv = !wing_c.empty() ? max_value * band_fraction : max_value;
    const double bc = !wing_c.empty() ? max_contracts * band_fraction : max_contracts;
    const Filled b = fill(band_c, incumbents, bv, bc, hysteresis);
    const Filled w = fill(wing_c, incumbents, max_value - b.value, max_contracts - b.contracts,
                          hysteresis);

    for (const auto& t : b.chosen) out.chosen.insert(t);
    for (const auto& t : w.chosen) out.chosen.insert(t);

    d.universe = (long)markets.size();
    d.candidates = (long)(band_c.size() + wing_c.size());
    d.chosen = (long)out.chosen.size();
    auto pass = [&](const Filled& f, const std::vector<Cand>& c, double budget) {
        PassDetail p;
        p.candidates = (long)c.size();
        p.chosen = (long)f.chosen.size();
        p.retained = f.n_retained;
        p.value = py_round_nd(f.value, 2);
        p.budget = py_round_nd(budget, 2);
        p.cut_R = py_round_nd(f.cut_r, 6);
        p.keep_R = py_round_nd(f.keep_r, 6);
        const long un = (long)c.size() - (long)f.chosen.size();
        p.unaffordable = un > 0 ? un : 0;
        return p;
    };
    d.band = pass(b, band_c, bv);
    d.wing = pass(w, wing_c, max_value - b.value);
    d.retained = b.n_retained + w.n_retained;
    d.added = d.chosen - d.retained;
    long dropped = 0;
    for (const auto& t : incumbents) if (!out.chosen.count(t)) ++dropped;
    d.dropped_incumbents = dropped;
    d.value = py_round_nd(b.value + w.value, 2);
    d.max_value = max_value;
    d.contracts = b.contracts + w.contracts;
    d.max_contracts = max_contracts;
    d.max_contracts_is_int = !max_contracts_opt.has_value();
    const long un = d.candidates - d.chosen;
    d.unaffordable = un > 0 ? un : 0;
    return out;
}

}  // namespace kp::strategy::budget
