#include "kp/strategy/orders.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <stdexcept>

#include "kp/config.hpp"
#include "kp/kalshi/error.hpp"
#include "kp/pycompat/round.hpp"
#include "kp/strategy/band.hpp"

namespace kp::strategy::orders {

using kp::json::Value;

bool is_reducing_reason(const std::string& reason) {
    return reason == "edge_negative" || reason == "no_longer_quoting";
}

// --- PySum ------------------------------------------------------------------------------------

void PySum::add(double x) {
    const double t = f_ + x;
    if (std::fabs(f_) >= std::fabs(x)) c_ += (f_ - t) + x;
    else c_ += (x - t) + f_;
    f_ = t;
}

double PySum::result() const {
    double f = f_;
    if (c_ != 0.0 && std::isfinite(c_)) f += c_;
    return f;
}

// --- formatting -------------------------------------------------------------------------------

std::string fmt_fixed(double x, int places) { return kp::kalshi::fp(x, places); }

std::string fmt_thousands2(double x) {
    if (std::isnan(x)) return "nan";
    if (std::isinf(x)) return x > 0 ? "inf" : "-inf";
    const std::string s = kp::kalshi::fp(x, 2);
    std::size_t i = (s[0] == '-') ? 1 : 0;
    const std::size_t dot = s.find('.');
    const std::string digits = s.substr(i, dot - i);
    std::string grouped;
    for (std::size_t k = 0; k < digits.size(); ++k) {
        if (k > 0 && (digits.size() - k) % 3 == 0) grouped.push_back(',');
        grouped.push_back(digits[k]);
    }
    return s.substr(0, i) + grouped + s.substr(dot);
}

namespace {
// str() of a Python int held in a double config constant (60 -> "60").
std::string int_str(double v) { return std::to_string((long long)v); }
}  // namespace

// --- Order --------------------------------------------------------------------------------------

double Order::count_f() const { return count.empty() ? 0.0 : kp::kalshi::py_float_str(count); }

Value Order::to_json() const {
    Value v;
    v.set("ticker", Value::make_string(ticker));
    v.set("side", Value::make_string(side));
    if (price) v.set("price", Value::make_number(*price));
    if (!count.empty()) v.set("count", Value::make_string(count));
    if (order_id) v.set("order_id", Value::make_string(*order_id));
    else if (order_id_null) v.set("order_id", Value());
    auto ids = [&] {
        if (client_order_id) v.set("client_order_id", Value::make_string(*client_order_id));
        if (dry_run) v.set("dry_run", Value::make_bool(*dry_run));
    };
    if (!coid_after_reason) ids();
    if (!reason.empty()) v.set("reason", Value::make_string(reason));
    if (!pair.empty()) v.set("pair", Value::make_string(pair));
    if (coid_after_reason) ids();
    return v;
}

double order_value(const std::string& side, OptD price, const std::string& count) {
    if (!price || count.empty()) return 0.0;
    const double p = *price;
    return (side == "bid" ? p : 1.0 - p) * kp::kalshi::py_float_str(count);
}

std::string pair_id(const std::string& ticker, const std::string& side, double price) {
    return ticker + "|" + side + "|" + fmt_fixed(price, 4);
}

std::vector<Order> best_first(const std::string& side, std::vector<Order> orders) {
    const bool bid = side == "bid";
    std::stable_sort(orders.begin(), orders.end(), [bid](const Order& a, const Order& b) {
        const double ka = bid ? -a.price.value() : a.price.value();
        const double kb = bid ? -b.price.value() : b.price.value();
        return ka < kb;
    });
    return orders;
}

// --- MarketState / RestingBook ----------------------------------------------------------------

void MarketState::arm_zombie(const std::string& s, double now) {
    if (s == "bid") { zombie_bid = now; zombie_bid_touch = best_bid; }
    else { zombie_ask = now; zombie_ask_touch = best_ask; }
}

double MarketState::contracts(const std::optional<std::string>& s) const {
    PySum sum;
    auto run = [&](const std::string& x) { for (const auto& o : side(x)) sum.add(o.count_f()); };
    if (s) run(*s);
    else { run("bid"); run("ask"); }
    return sum.result();
}

double MarketState::value(const std::optional<std::string>& s) const {
    PySum sum;
    auto run = [&](const std::string& x) {
        for (const auto& o : side(x)) sum.add(order_value(x, o.price, o.count));
    };
    if (s) run(*s);
    else { run("bid"); run("ask"); }
    return sum.result();
}

Value MarketState::as_record() const {
    Value v;
    v.set("ticker", Value::make_string(ticker));
    v.set("theo", theo ? Value::make_number(*theo) : Value());
    for (const char* s : {"bid", "ask"}) {
        Value arr = Value::make_array({});
        for (const auto& o : best_first(s, side(s))) {
            Value row = Value::make_array({});
            row.push(o.price ? Value::make_number(*o.price) : Value());
            row.push(o.count.empty() ? Value() : Value::make_string(o.count));
            arr.push(row);
        }
        v.set(s, arr);
    }
    return v;
}

MarketState& RestingBook::state(const std::string& ticker) {
    auto& p = m_[ticker];
    if (!p) p = std::make_shared<MarketState>(ticker);
    return *p;
}

const MarketState* RestingBook::find(const std::string& ticker) const {
    const auto* p = m_.find(ticker);
    return p ? p->get() : nullptr;
}

std::vector<std::string> RestingBook::resting_tickers() const {
    std::vector<std::string> out;
    for (const auto& [t, st] : m_)
        if (!st->bid.empty() || !st->ask.empty()) out.push_back(t);
    return out;
}

std::set<std::pair<std::string, std::string>> RestingBook::resting_sides() const {
    std::set<std::pair<std::string, std::string>> out;
    for (const auto& t : resting_tickers())
        for (const char* s : {"bid", "ask"})
            if (!m_.find(t)->get()->side(s).empty()) out.insert({t, s});
    return out;
}

std::vector<std::string> RestingBook::tickers() const {
    std::vector<std::string> out;
    for (const auto& kv : m_) out.push_back(kv.first);
    return out;
}

std::vector<Order> RestingBook::all_orders() const {
    std::vector<Order> out;
    for (const auto& kv : m_)
        for (const char* s : {"bid", "ask"})
            for (const auto& o : kv.second->side(s)) out.push_back(o);
    return out;
}

double RestingBook::contracts(const std::optional<std::string>& ticker,
                              const std::optional<std::string>& side) const {
    if (ticker) {
        const auto* st = find(*ticker);
        return st ? st->contracts(side) : 0.0;
    }
    PySum sum;
    for (const auto& kv : m_) sum.add(kv.second->contracts(side));
    return sum.result();
}

double RestingBook::value(const std::optional<std::string>& ticker,
                          const std::optional<std::string>& side) const {
    if (ticker) {
        const auto* st = find(*ticker);
        return st ? st->value(side) : 0.0;
    }
    PySum sum;
    for (const auto& kv : m_) sum.add(kv.second->value(side));
    return sum.result();
}

std::size_t RestingBook::size() const {
    std::size_t n = 0;
    for (const auto& kv : m_) n += kv.second->bid.size() + kv.second->ask.size();
    return n;
}

// --- the policy ---------------------------------------------------------------------------------

bool zombie_held(const std::string& side, OptD touch, OptD zombie_at, OptD zombie_touch, OptD now,
                 OptD hold_s) {
    if (!zombie_at || !now) return false;
    const double hold = hold_s ? *hold_s : kp::config::QUOTE_ZOMBIE_HOLD_S;
    if (hold <= 0 || (*now - *zombie_at) >= hold) return false;
    if (touch && zombie_touch) {
        const bool moved_in = (side == "bid") ? (*touch > *zombie_touch + 1e-9)
                                              : (*touch < *zombie_touch - 1e-9);
        if (moved_in) return false;
    }
    return true;
}

double tier_step(const std::vector<double>& want) {
    if (want.size() >= 2) {
        bool any = false;
        double lo = 0.0;
        for (std::size_t i = 0; i + 1 < want.size(); ++i) {
            const double g = std::fabs(want[i] - want[i + 1]);
            if (!(g > 1e-9)) continue;
            if (!any || g < lo) lo = g;
            any = true;
        }
        if (any) return lo;
    }
    return quoting::TICK;
}

namespace {
long edge_micros_opt(const std::string& side, OptD theo, double price) {
    // The reference calls `_edge_micros(side, None, px)` on the top-up path when a market was
    // never priced, which raises TypeError. Raise too, rather than invent an edge.
    if (!theo) throw std::invalid_argument("plan_side: theo is None");
    return quoting::edge_micros(side, *theo, price);
}
}  // namespace

SidePlan plan_side(const std::string& ticker, const std::string& side, OptD theo,
                   const std::vector<quoting::Rung>& rungs, const std::vector<Order>& live_in,
                   const std::vector<std::string>& counts, int front_ticks, int back_ticks,
                   OptD touch, OptD zombie_at, OptD zombie_touch, OptD now) {
    SidePlan out;
    const std::vector<Order> live = best_first(side, live_in);
    std::vector<double> want;
    for (const auto& r : rungs) want.push_back(r.price);
    const bool held_back = zombie_held(side, touch, zombie_at, zombie_touch, now);
    const bool bid = side == "bid";

    std::size_t rank = 0;
    for (const auto& o : live) {
        const double op = o.price.value();
        const long e = theo ? quoting::edge_micros(side, *theo, op) : -1;
        if (e <= 0) {
            Order c = o;
            c.reason = "edge_negative";
            out.cancels.push_back(std::move(c));
            continue;
        }
        const bool has_target = rank < want.size();
        const double target = has_target ? want[rank] : 0.0;
        rank += 1;
        if (!has_target) {
            Order k = o;
            k.reason = "no_target_for_slot";
            out.keeps.push_back(std::move(k));
            continue;
        }
        const double gain = bid ? (target - op) : (op - target);
        const double thresh = (double)(rank == 1 ? front_ticks : back_ticks) * quoting::TICK;
        if (gain >= thresh - 1e-9 && held_back) {
            Order k = o;
            k.reason = "zombie_hold";
            out.keeps.push_back(std::move(k));
            continue;
        }
        if (gain >= thresh - 1e-9) {
            const std::string pid = pair_id(ticker, side, op);
            Order c = o;
            c.reason = "improve_" + fmt_fixed(gain / quoting::TICK, 0) + "t";
            c.pair = pid;
            out.cancels.push_back(std::move(c));
            Order p;
            p.ticker = ticker;
            p.side = side;
            p.price = target;
            p.count = o.count;
            p.reason = "improved";
            p.pair = pid;
            out.places.push_back(std::move(p));
        } else {
            Order k = o;
            k.reason = (gain < thresh) ? "within_deadband" : "no_gain";
            out.keeps.push_back(std::move(k));
        }
    }

    // Top up to a full ladder, BEHIND the worst thing we are keeping.
    std::vector<double> held;
    for (const auto& k : out.keeps) held.push_back(k.price.value());
    for (const auto& p : out.places) held.push_back(p.price.value());
    const double step = tier_step(want);

    PySum bsum;
    for (const auto& r : rungs) bsum.add(kp::kalshi::py_float_str(r.count));
    const double budget = bsum.result();
    PySum ks, ps;
    for (const auto& k : out.keeps) ks.add(k.count_f());
    for (const auto& p : out.places) ps.add(p.count_f());
    double on_side = ks.result() + ps.result();

    while (held.size() < want.size()) {
        const std::size_t slot = held.size();
        const double remaining = budget - on_side;
        if (remaining <= 0) break;
        double px;
        if (held.empty()) {
            px = want[slot];
        } else {
            double back = held[0];
            for (std::size_t i = 1; i < held.size(); ++i)
                if (bid ? (held[i] < back) : (held[i] > back)) back = held[i];
            px = kp::pycompat::py_round_nd(bid ? back - step : back + step, 4);
        }
        if (!quoting::tradeable(px) || edge_micros_opt(side, theo, px) <= 0) break;
        bool dup = false;
        for (double h : held) if (std::fabs(px - h) < 1e-9) { dup = true; break; }
        if (dup) break;
        if (counts.empty()) throw std::out_of_range("plan_side: empty counts");
        const std::string& full = counts[std::min(slot, counts.size() - 1)];
        double n = kp::kalshi::py_float_str(full);
        std::string cnt;
        if (n <= remaining + 1e-9) {
            cnt = full;
        } else {
            n = remaining;
            cnt = fmt_fixed(n, 2);
        }
        Order p;
        p.ticker = ticker;
        p.side = side;
        p.price = px;
        p.count = cnt;
        p.reason = slot == 0 ? "cold_start" : "top_up";
        out.places.push_back(std::move(p));
        held.push_back(px);
        on_side += n;
    }
    return out;
}

int apply_fill(RestingBook& book, const Value& fill, double now) {
    if (!fill.is_object()) return 0;
    const Value* oidv = fill.find("order_id");
    if (oidv == nullptr || !kp::kalshi::py_truthy(*oidv)) return 0;
    double n;
    try {
        const Value* cf = fill.find("count_fp");
        n = (cf != nullptr && kp::kalshi::py_truthy(*cf)) ? kp::kalshi::py_float(*cf) : 0.0;
    } catch (const std::invalid_argument&) {
        return 0;
    }
    if (n <= 0) return 0;

    std::vector<std::string> tickers;
    const Value* mt = fill.find("market_ticker");
    if (mt != nullptr && kp::kalshi::py_truthy(*mt)) {
        // A non-string ticker keys a state in the reference that no order can live in; nothing
        // can match it, so it is a no-op here.
        if (!mt->is_string()) return 0;
        tickers.push_back(mt->str());
    } else {
        tickers = book.tickers();
    }
    // Only a STRING id can equal an order's id.
    if (!oidv->is_string()) {
        for (const auto& t : tickers) book.state(t);
        return 0;
    }
    const std::string& oid = oidv->str();
    for (const auto& t : tickers) {
        MarketState& st = book.state(t);
        for (const char* side : {"bid", "ask"}) {
            auto& rows = st.side(side);
            for (std::size_t i = 0; i < rows.size(); ++i) {
                Order& o = rows[i];
                if (!o.order_id || *o.order_id != oid) continue;
                const double left = o.count_f() - n;
                if (left > 1e-9) o.count = fmt_fixed(left, 2);
                else rows.erase(rows.begin() + (std::ptrdiff_t)i);
                st.mark_fill(side, now);
                return 1;
            }
        }
    }
    return 0;
}

Reconciled reconcile(RestingBook& book, const Desired& desired,
                     const std::map<std::string, std::vector<std::string>>* counts_by_ticker,
                     const std::vector<std::string>* counts, double now) {
    Reconciled out;
    std::set<SideKey> seen;
    for (const auto& [key, rungs] : desired) {
        const std::string& ticker = key.first;
        const std::string& side = key.second;
        MarketState& st = book.state(ticker);
        seen.insert(key);
        SidePlan sp = plan_side(ticker, side, st.theo, rungs, st.side(side),
                                band::resolve_counts(counts_by_ticker, counts, ticker),
                                FRONT_IMPROVE_TICKS, BACK_IMPROVE_TICKS, st.touch(side),
                                st.zombie(side), st.zombie_at_touch(side), now);
        for (auto& c : sp.cancels) out.to_cancel.push_back(std::move(c));
        for (auto& p : sp.places) out.to_place.push_back(std::move(p));
        for (auto& k : sp.keeps) out.kept.push_back(std::move(k));
    }
    for (const auto& ticker : book.tickers()) {
        MarketState& st = book.state(ticker);
        for (const char* side : {"bid", "ask"}) {
            if (seen.count({ticker, side})) continue;
            for (const auto& o : st.side(side)) {
                Order c = o;
                c.reason = "no_longer_quoting";
                out.to_cancel.push_back(std::move(c));
            }
        }
    }
    return out;
}

std::vector<Order> reducing_only(const std::vector<Order>& to_cancel) {
    std::vector<Order> out;
    for (const auto& c : to_cancel)
        if (is_reducing_reason(c.reason)) out.push_back(c);
    return out;
}

std::pair<bool, std::vector<std::string>> within_risk_limits(const RestingBook& book,
                                                             const std::vector<Order>& to_place,
                                                             const std::vector<Order>& to_cancel) {
    std::vector<std::string> problems;
    using SideMap = kp::pycompat::ODict<SideKey, double, SideKeyHash>;
    auto by_side = [](const std::vector<Order>& rows) {
        SideMap out;
        for (const auto& r : rows) {
            const SideKey key{r.ticker, r.side};
            const double* cur = out.find(key);
            const double base = cur ? *cur : 0.0;
            out.set(key, base + r.count_f());
        }
        return out;
    };
    const SideMap add_side = by_side(to_place), drop_side = by_side(to_cancel);
    std::vector<SideKey> keys;
    for (const auto& kv : add_side) keys.push_back(kv.first);
    for (const auto& kv : drop_side) if (!add_side.contains(kv.first)) keys.push_back(kv.first);
    for (const auto& key : keys) {
        const double* d = drop_side.find(key);
        const double* a = add_side.find(key);
        const double after = book.contracts(key.first, key.second) - (d ? *d : 0.0) + (a ? *a : 0.0);
        if (after > kp::config::MAX_RESTING_CONTRACTS_PER_MARKET_SIDE + 1e-9)
            problems.push_back(key.first + "/" + key.second + ": " + fmt_fixed(after, 0) + " > "
                               + int_str(kp::config::MAX_RESTING_CONTRACTS_PER_MARKET_SIDE));
    }

    using MktMap = kp::pycompat::ODict<std::string, double>;
    auto by_market = [](const std::vector<Order>& rows) {
        MktMap out;
        for (const auto& r : rows) {
            const double* cur = out.find(r.ticker);
            const double base = cur ? *cur : 0.0;
            out.set(r.ticker, base + order_value(r.side, r.price, r.count));
        }
        return out;
    };
    const MktMap add_mkt = by_market(to_place), drop_mkt = by_market(to_cancel);
    std::vector<std::string> tks;
    for (const auto& kv : add_mkt) tks.push_back(kv.first);
    for (const auto& kv : drop_mkt) if (!add_mkt.contains(kv.first)) tks.push_back(kv.first);
    for (const auto& t : tks) {
        const double* d = drop_mkt.find(t);
        const double* a = add_mkt.find(t);
        const double after = book.value(t) - (d ? *d : 0.0) + (a ? *a : 0.0);
        if (after > kp::config::MAX_RESTING_ORDER_VALUE_PER_MARKET + 1e-9)
            problems.push_back(t + ": $" + fmt_thousands2(after) + " > $"
                               + fmt_thousands2(kp::config::MAX_RESTING_ORDER_VALUE_PER_MARKET));
    }

    PySum cs, ps;
    for (const auto& c : to_cancel) cs.add(c.count_f());
    for (const auto& p : to_place) ps.add(p.count_f());
    const double total = book.contracts() - cs.result() + ps.result();
    if (total > kp::config::MAX_RESTING_CONTRACTS_TOTAL + 1e-9)
        problems.push_back("total " + fmt_fixed(total, 0) + " > "
                           + int_str(kp::config::MAX_RESTING_CONTRACTS_TOTAL));

    PySum cv, pv;
    for (const auto& c : to_cancel) cv.add(order_value(c.side, c.price, c.count));
    for (const auto& p : to_place) pv.add(order_value(p.side, p.price, p.count));
    const double value = book.value() - cv.result() + pv.result();
    if (value > kp::config::MAX_RESTING_ORDER_VALUE + 1e-9)
        problems.push_back("resting value $" + fmt_thousands2(value) + " > $"
                           + fmt_thousands2(kp::config::MAX_RESTING_ORDER_VALUE));
    return {problems.empty(), problems};
}

// --- apply ------------------------------------------------------------------------------------

Value ApplyResult::to_json() const {
    Value v;
    v.set("cancelled", Value::make_int(cancelled));
    v.set("placed", Value::make_int(placed));
    v.set("cancel_errors", Value::make_array(cancel_errors));
    v.set("place_errors", Value::make_array(place_errors));
    v.set("dry_run", Value::make_bool(dry_run));
    v.set("rate_limited", Value::make_int(rate_limited));
    v.set("unsent_cancels", Value::make_int(unsent_cancels));
    v.set("unsent_places", Value::make_int(unsent_places));
    v.set("orphaned_places", Value::make_int(orphaned_places));
    v.set("zombies_armed", Value::make_int(zombies_armed));
    v.set("cancel_gone", Value::make_int(cancel_gone));
    return v;
}

std::string uuid4() {
    thread_local std::mt19937_64 rng{[] {
        std::random_device rd;
        return ((std::uint64_t)rd() << 32) ^ (std::uint64_t)rd();
    }()};
    std::uint64_t hi = rng(), lo = rng();
    hi = (hi & 0xFFFFFFFFFFFF0FFFull) | 0x0000000000004000ull;   // version 4
    lo = (lo & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull;   // RFC 4122 variant
    char buf[40];
    std::snprintf(buf, sizeof buf, "%08llx-%04llx-%04llx-%04llx-%012llx",
                  (unsigned long long)(hi >> 32), (unsigned long long)((hi >> 16) & 0xFFFF),
                  (unsigned long long)(hi & 0xFFFF), (unsigned long long)(lo >> 48),
                  (unsigned long long)(lo & 0xFFFFFFFFFFFFull));
    return buf;
}

namespace {

enum class Sent { OK, DECLINED, REJECTED, FAILED };

const std::vector<Value>& as_list(const Value& v) {
    static const std::vector<Value> empty;
    if (v.is_null()) return empty;
    if (!v.is_array()) throw std::runtime_error("apply: batch response is not a list");
    return v.arr();
}

// res.get(k) on a per-item result, which must be a dict (the reference raises AttributeError).
const Value* get(const Value& res, const char* k) {
    if (!res.is_object()) throw std::runtime_error("apply: batch result item is not a dict");
    return res.find(k);
}

}  // namespace

ApplyResult apply(std::vector<Order> to_place, const std::vector<Order>& to_cancel,
                  RestingBook& book, kp::kalshi::Api* api, bool live, long max_batch,
                  kp::kalshi::TokenBucket* budget, double now, const IdSource& ids) {
    if (live && api == nullptr) throw std::invalid_argument("orders::apply: live without an Api");
    ApplyResult out;
    out.dry_run = !live;
    const std::size_t mb = (std::size_t)(max_batch > 0 ? max_batch : kp::kalshi::BATCH_FALLBACK);

    // _send: never raises. Returns the results and the status.
    auto send = [&](auto&& fn, std::size_t n, double cost, std::vector<Value>& errors,
                    Value& results) -> Sent {
        if (budget != nullptr && !budget->can_afford(cost)) return Sent::DECLINED;
        try {
            results = fn();
            return Sent::OK;
        } catch (const kp::kalshi::KalshiError& e) {
            Value err;
            err.set("batch_error", Value::make_string("HTTP " + std::to_string(e.status)));
            err.set("n", Value::make_int((long long)n));
            err.set("status", Value::make_int(e.status));
            err.set("body", Value::make_string(kp::kalshi::utf8_prefix(e.body, 400)));
            errors.push_back(std::move(err));
            if (e.status == 429) {
                out.rate_limited += 1;
                return Sent::REJECTED;
            }
            return Sent::FAILED;
        } catch (const std::exception& e) {
            Value err;
            err.set("batch_error", Value::make_string(kp::kalshi::py_type_name(e) + ": "
                                                      + kp::kalshi::utf8_prefix(e.what(), 80)));
            err.set("n", Value::make_int((long long)n));
            errors.push_back(std::move(err));
            return Sent::FAILED;
        }
    };

    std::vector<std::string> ids_all;
    for (const auto& c : to_cancel)
        if (c.has_order_id()) ids_all.push_back(*c.order_id);
    std::set<std::string> done_ids;

    if (live && !ids_all.empty()) {
        for (std::size_t i = 0; i < ids_all.size(); i += mb) {
            const std::vector<std::string> chunk(
                ids_all.begin() + (std::ptrdiff_t)i,
                ids_all.begin() + (std::ptrdiff_t)std::min(i + mb, ids_all.size()));
            Value resv;
            const Sent status = send([&] { return api->batch_cancel_orders(chunk); }, chunk.size(),
                                     (double)(kp::kalshi::CANCEL_TOKEN_COST * (long)chunk.size()),
                                     out.cancel_errors, resv);
            const std::vector<Value>& results = as_list(resv);
            for (std::size_t j = 0; j < chunk.size() && j < results.size(); ++j) {
                const std::string& oid = chunk[j];
                const Value& res = results[j];
                const Value* errv = get(res, "error");
                const bool err_truthy = errv != nullptr && kp::kalshi::py_truthy(*errv);
                bool gone = false;
                if (err_truthy && errv->is_object()) {
                    const Value* code = errv->find("code");
                    gone = code != nullptr && code->is_string() && code->str() == "not_found";
                }
                if (gone) {
                    done_ids.insert(oid);
                    out.cancel_gone += 1;
                    continue;
                }
                const Value* rb = get(res, "reduced_by");
                const double reduced =
                    (rb != nullptr && kp::kalshi::py_truthy(*rb)) ? kp::kalshi::py_float(*rb) : 0.0;
                if (err_truthy || !(reduced != 0.0)) {
                    Value e;
                    e.set("order_id", Value::make_string(oid));
                    for (const auto& m : res.obj()) e.set(m.first, m.second);
                    out.cancel_errors.push_back(std::move(e));
                } else {
                    const Value* rid = get(res, "order_id");
                    if (rid != nullptr && kp::kalshi::py_truthy(*rid)) {
                        // A non-string id is added in the reference and can equal no order's id.
                        if (rid->is_string()) done_ids.insert(rid->str());
                    } else {
                        done_ids.insert(oid);
                    }
                    out.cancelled += 1;
                }
            }
            if (!results.empty() && results.size() != chunk.size()) {
                Value e;
                e.set("batch_shape", Value::make_string(std::to_string(results.size())
                                                        + " results for "
                                                        + std::to_string(chunk.size())
                                                        + " cancels"));
                out.cancel_errors.push_back(std::move(e));
            }
            if (status == Sent::DECLINED || status == Sent::REJECTED) {
                out.unsent_cancels += (long)ids_all.size()
                                      - (long)(status == Sent::DECLINED ? i : i + chunk.size());
                break;
            }
        }
    }

    for (const auto& c : to_cancel) {
        if (live && !(c.order_id && done_ids.count(*c.order_id))) continue;
        MarketState& st = book.state(c.ticker);
        const double cp = c.price.value();
        std::vector<Order> keep;
        for (const auto& o : st.side(c.side))
            if (std::fabs(o.price.value() - cp) > 1e-9) keep.push_back(o);
        st.set_side(c.side, std::move(keep));
        if (c.reason == "edge_negative") {
            st.arm_zombie(c.side, now);
            out.zombies_armed += 1;
        }
    }
    if (!live) out.cancelled = (long)to_cancel.size();

    std::set<std::string> cancelled_pairs;
    for (const auto& c : to_cancel)
        if (!c.pair.empty() && (!live || (c.order_id && done_ids.count(*c.order_id))))
            cancelled_pairs.insert(c.pair);
    std::vector<Order> meta;
    for (auto& p : to_place) {
        if (!p.pair.empty() && !cancelled_pairs.count(p.pair)) {
            out.orphaned_places += 1;
            continue;
        }
        meta.push_back(std::move(p));
    }

    std::vector<Value> bodies;
    for (auto& p : meta) {
        p.client_order_id = ids();
        p.coid_after_reason = true;
        bodies.push_back(kp::kalshi::order_body(p.ticker, p.side, kp::kalshi::fp(p.price.value()),
                                                p.count, *p.client_order_id));
    }

    auto record = [&](const Order& p, std::optional<std::string> order_id, bool dry) {
        MarketState& st = book.state(p.ticker);
        Order row;
        row.ticker = p.ticker;
        row.side = p.side;
        row.price = p.price;
        row.count = p.count;
        row.order_id_null = !order_id.has_value();
        row.order_id = std::move(order_id);
        row.client_order_id = p.client_order_id;
        row.dry_run = dry;
        st.side(p.side).push_back(std::move(row));
    };

    if (live && !bodies.empty()) {
        for (std::size_t i = 0; i < bodies.size(); i += mb) {
            const std::size_t end = std::min(i + mb, bodies.size());
            const std::vector<Value> chunk(bodies.begin() + (std::ptrdiff_t)i,
                                           bodies.begin() + (std::ptrdiff_t)end);
            Value resv;
            const Sent status = send([&] { return api->batch_create_orders(chunk); }, chunk.size(),
                                     (double)(kp::kalshi::ORDER_TOKEN_COST * (long)chunk.size()),
                                     out.place_errors, resv);
            const std::vector<Value>& results = as_list(resv);
            for (std::size_t j = 0; j < chunk.size() && j < results.size(); ++j) {
                const Order& p = meta[i + j];
                const Value& res = results[j];
                const Value* errv = get(res, "error");
                if (errv != nullptr && kp::kalshi::py_truthy(*errv)) {
                    Value e = p.to_json();
                    e.set("error", *errv);
                    out.place_errors.push_back(std::move(e));
                    continue;
                }
                const Value* rid = get(res, "order_id");
                std::optional<std::string> oid;
                if (rid != nullptr && rid->is_string()) oid = rid->str();
                record(p, oid, false);
                out.placed += 1;
            }
            if (!results.empty() && results.size() != chunk.size()) {
                Value e;
                e.set("batch_shape", Value::make_string(std::to_string(results.size())
                                                        + " results for "
                                                        + std::to_string(chunk.size())
                                                        + " orders"));
                out.place_errors.push_back(std::move(e));
            }
            if (status == Sent::DECLINED || status == Sent::REJECTED) {
                out.unsent_places += (long)bodies.size()
                                     - (long)(status == Sent::DECLINED ? i : i + chunk.size());
                break;
            }
        }
    } else if (!bodies.empty()) {
        for (const auto& p : meta) record(p, std::nullopt, true);
        out.placed = (long)bodies.size();
    }
    return out;
}

}  // namespace kp::strategy::orders
