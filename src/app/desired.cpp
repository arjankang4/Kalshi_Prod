#include "kp/app/desired.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "kp/config.hpp"
#include "kp/kalshi/bucket.hpp"
#include "kp/pricing/lean.hpp"
#include "kp/pycompat/round.hpp"

namespace kp::app {

namespace json = kp::json;
namespace th = kp::pricing::theo;
namespace orders = kp::strategy::orders;
namespace quoting = kp::strategy::quoting;

namespace {

// Python `dict.get(k, fallback) or []` over a json object: absent AND falsy both fall back.
const json::Value* truthy_list(const json::Value& inner, const char* k1, const char* k2) {
    const json::Value* v = inner.find(k1);
    if (v == nullptr) v = inner.find(k2);
    if (v == nullptr || !v->is_array() || v->arr().empty()) return nullptr;
    return v;
}

json::Value opt_num(OptD v) { return v ? json::Value::make_number(*v) : json::Value(); }
json::Value opt_int(std::optional<long> v) { return v ? json::Value::make_int(*v) : json::Value(); }
json::Value opt_str(const std::optional<std::string>& v) {
    return v ? json::Value::make_string(*v) : json::Value();
}
json::Value str_or_none(const std::string& s) {
    return s.empty() ? json::Value() : json::Value::make_string(s);
}

}  // namespace

TopOfBook top_of_book(const json::Value* entry) {
    TopOfBook t;
    // `if not book_entry: return [], []` -- None and an empty dict are both empty.
    if (entry == nullptr || entry->is_null() || (entry->is_object() && entry->obj().empty()))
        return t;
    const json::Value* inner = entry;
    for (const char* k : {"orderbook_fp", "orderbook"}) {
        const json::Value* v = entry->find(k);
        if (v != nullptr && kp::kalshi::py_truthy(*v)) { inner = v; break; }
    }
    const json::Value* yes = truthy_list(*inner, "yes_dollars", "yes");
    const json::Value* no = truthy_list(*inner, "no_dollars", "no");
    if (yes) {
        const auto& lv = yes->arr().back().arr();
        t.best_bid = kp::kalshi::py_float(lv.at(0));
        t.bid_size = kp::kalshi::py_float(lv.at(1));
    }
    if (no) {
        const auto& lv = no->arr().back().arr();
        t.best_ask = kp::pycompat::py_round_nd(1.0 - kp::kalshi::py_float(lv.at(0)), 4);
        t.ask_size = kp::kalshi::py_float(lv.at(1));
    }
    return t;
}

std::pair<bool, OptD> hourly_cutoff(const th::Market& m, const kp::time::Instant& now) {
    const auto settle = kp::strategy::edgemgr::settlement_of(m);
    if (!settle) return {false, std::nullopt};
    const auto et = kp::time::to_et(*settle);
    if (et.hour == 16 && et.minute == 0) return {false, std::nullopt};
    const double left = kp::time::total_seconds(now, *settle) / 60.0;
    return {left < kp::config::HOURLY_MIN_MINUTES_TO_CLOSE, left};
}

DesiredResult build_desired(const std::vector<th::Market>& markets, const Fairs& fairs,
                            const Books* books, const EdgeLookup& em, const LeanRows* lean_map,
                            bool tiers_enabled, const Bands* bands, const PositionMap* positions,
                            orders::RestingBook& book, double mono_now, const kp::time::Instant& now) {
    DesiredResult out;
    static const FairDetail NO_DETAIL{};
    std::vector<std::string> default_counts;
    for (auto c : kp::config::QUOTE_TIER_COUNTS) default_counts.emplace_back(c);

    for (const auto& m : markets) {
        const std::string& tk = m.ticker;
        const FairDetail* dp = fairs.find(tk);
        const FairDetail& detail = dp ? *dp : NO_DETAIL;
        const OptD fair = detail.fair;
        const kp::strategy::edgemgr::EdgeRecord e = em(tk);
        const json::Value* entry = nullptr;
        if (books) {
            auto it = books->find(tk);
            if (it != books->end()) entry = &it->second;
        }
        const TopOfBook tob = top_of_book(entry);
        const kp::strategy::band::Classified* bd = nullptr;
        if (bands) {
            auto it = bands->find(tk);
            if (it != bands->end()) bd = &it->second;
        }
        const std::vector<std::string>& counts =
            (bd && !bd->counts.empty()) ? bd->counts : default_counts;

        json::Value note;
        note.set("fair", opt_num(fair));
        note.set("best_bid", opt_num(tob.best_bid));
        note.set("best_ask", opt_num(tob.best_ask));
        note.set("inband", bd ? json::Value::make_bool(bd->inband) : json::Value());
        note.set("sigmas", bd && bd->detail.has_detail ? opt_num(bd->detail.sigmas) : json::Value());
        note.set("rungs_wanted", json::Value::make_int((long long)counts.size()));
        note.set("edge_R", opt_num(e.R));
        note.set("edge_edge", opt_num(e.edge));
        note.set("edge_risk_u", json::Value::make_number(e.risk_u));
        note.set("edge_source", str_or_none(e.source));
        note.set("edge_reason", str_or_none(e.reason));
        note.set("fair_vol", opt_num(detail.fair_vol));
        note.set("delta_adj", opt_num(detail.delta_adj));
        note.set("delta_adj_reason", opt_str(detail.delta_adj_reason));
        note.set("f_move", opt_num(detail.f_move));

        if (!fair) {
            note.set("skip", json::Value::make_string(
                                 detail.reason && !detail.reason->empty() ? *detail.reason : "no_fair"));
            out.notes[tk] = std::move(note);
            continue;
        }
        if (!e.quotable) {
            note.set("skip", json::Value::make_string(!e.reason.empty() ? e.reason : "not_quotable"));
            out.notes[tk] = std::move(note);
            continue;
        }
        if (const auto [cut, left] = hourly_cutoff(m, now); cut) {
            char b[64];
            std::snprintf(b, sizeof b, "hourly_within_%gmin", kp::config::HOURLY_MIN_MINUTES_TO_CLOSE);
            note.set("skip", json::Value::make_string(b));
            note.set("minutes_to_close", opt_num(left));
            out.notes[tk] = std::move(note);
            continue;
        }

        // THE INVENTORY GATE: at the limit, stop quoting the side that would add; keep quoting the
        // side that flattens.
        double pos = 0.0;
        if (positions) {
            auto it = positions->find(tk);
            if (it != positions->end()) pos = it->second;
        }
        const OptD limit = bd ? OptD(bd->position_limit) : std::nullopt;
        std::vector<std::string> blocked;          // sorted(): "ask" < "bid"
        if (limit && *limit != 0.0) {
            if (pos <= -*limit + 1e-9) blocked.push_back("ask");
            if (pos >= *limit - 1e-9) blocked.push_back("bid");
        }
        note.set("position", json::Value::make_number(pos + 0.0));
        note.set("position_limit", opt_num(limit));
        if (!blocked.empty()) {
            json::Value arr;
            for (const auto& s : blocked) arr.push(json::Value::make_string(s));
            note.set("position_blocked", arr);
        }

        const kp::strategy::leanchain::Row* rv = lean_map ? lean_map->find(tk) : nullptr;
        for (const std::string side : {"bid", "ask"}) {
            if (std::find(blocked.begin(), blocked.end(), side) != blocked.end()) {
                note.set(side + "_rungs", json::Value::make_int(0));
                continue;
            }
            const auto [adj, ldet] = kp::pricing::lean::edge_adjust(
                rv ? rv->p_ref : std::nullopt, rv ? rv->p_rv : std::nullopt, side);
            const long floor_m = e.floor_micros.value_or(0);
            const long bonus_m = e.bonus_micros.value_or(0);
            const OptD ft = book.state(tk).last_fill(side);
            const long back_m = kp::pricing::lean::backoff_micros(
                ft ? OptD(mono_now - *ft) : std::nullopt);
            const long min_m = std::max(0L, floor_m + adj);
            const long entry_m = std::max(0L, min_m + bonus_m + back_m);
            const auto rungs = quoting::decide_ladder(side, fair, tob.best_bid, tob.best_ask, counts,
                                                      quoting::NONE, entry_m, e.scale_pos,
                                                      tiers_enabled);
            if (!rungs.empty()) out.desired[{tk, side}] = rungs;
            note.set(side + "_rungs", json::Value::make_int((long long)rungs.size()));
            note.set(side + "_lean", str_or_none(ldet.lean_reason));
            note.set(side + "_lean_cents", json::Value::make_number(ldet.lean_cents));
            note.set(side + "_min_edge", json::Value::make_int(entry_m));
            note.set(side + "_bonus", json::Value::make_int(bonus_m));
            note.set(side + "_backoff", json::Value::make_int(back_m));
        }
        out.notes[tk] = std::move(note);
    }
    return out;
}

}  // namespace kp::app
