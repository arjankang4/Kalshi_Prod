// run_04_quote_loop.cycle, step for step. The reference's comments on WHY each step sits where it
// does are summarised at each step; the full arguments live in the Python.
#include "kp/app/cycle.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <unordered_set>

#include "kp/app/run04.hpp"
#include "kp/config.hpp"
#include "kp/pricing/atmvol.hpp"
#include "kp/pricing/universe.hpp"
#include "kp/pycompat/repr.hpp"
#include "kp/pycompat/round.hpp"
#include "kp/strategy/band.hpp"
#include "kp/strategy/budget.hpp"
#include "kp/strategy/fastcancel.hpp"

namespace kp::app {

namespace json = kp::json;
namespace th = kp::pricing::theo;
namespace vt = kp::pricing::validate_theo;
namespace orders = kp::strategy::orders;
namespace budget = kp::strategy::budget;
namespace band = kp::strategy::band;
namespace fc = kp::strategy::fastcancel;
namespace em_ns = kp::strategy::edgemgr;

namespace {

json::Value num(OptD v) { return v ? json::Value::make_number(*v) : json::Value(); }
json::Value str(const std::optional<std::string>& s) {
    return s ? json::Value::make_string(*s) : json::Value();
}
json::Value str_or_none(const std::string& s) {
    return s.empty() ? json::Value() : json::Value::make_string(s);
}
json::Value i64(long long v) { return json::Value::make_int(v); }

// str(e)[:n] -- by code point, as Python slices a str.
std::string head(const std::string& s, std::size_t n) { return kp::kalshi::utf8_prefix(s, n); }
std::string exc_line(const std::exception& e, std::size_t n) {
    return kp::kalshi::py_type_name(e) + ": " + head(e.what(), n);
}

using kp::json::py_repr;

// Python round(x, n) for the record's display fields.
double r(double x, int n) { return kp::pycompat::py_round_nd(x, n); }

}  // namespace

// --- JSON for summaries whose ports carry none --------------------------------------------------
json::Value to_json(const em_ns::EdgeSummary& s) {
    json::Value o = json::Value::make_object({});
    if (!s.priced) {
        o.set("n", i64(0));
        o.set("source", json::Value::make_string("flat"));
        o.set("reason", str_or_none(s.reason));
        return o;
    }
    o.set("n", i64(s.n));
    o.set("n_priced", i64(s.n_priced));
    o.set("refused", i64(s.refused));
    o.set("edge_min", num(s.edge_min));
    o.set("edge_med", num(s.edge_med));
    o.set("edge_max", num(s.edge_max));
    o.set("enabled", json::Value::make_bool(s.enabled));
    return o;
}

json::Value to_json(const kp::strategy::risklimit::RiskSummary& s) {
    json::Value o = json::Value::make_object({});
    o.set("fills", i64(s.fills));
    o.set("skipped_fills", i64(s.skipped_fills));
    o.set("contracts", json::Value::make_number(s.contracts));
    o.set("cash", json::Value::make_number(s.cash));
    o.set("fees", json::Value::make_number(s.fees));
    o.set("pnl", num(s.pnl));
    o.set("pnl_basis", json::Value::make_string("account equity vs day open, marked to Kalshi mid"));
    o.set("equity", num(s.equity));
    o.set("opening_equity", num(s.opening_equity));
    o.set("unmarked", s.unmarked ? i64(*s.unmarked) : json::Value());
    if (s.mark_sources) {
        json::Value ms = json::Value::make_object({});
        for (const auto& [k, n] : *s.mark_sources) ms.set(k, i64(n));
        o.set("mark_sources", ms);
    } else {
        o.set("mark_sources", json::Value());
    }
    o.set("worst", json::Value::make_number(s.worst));
    o.set("limit", num(s.limit));
    o.set("halted", str_or_none(s.halted));
    return o;
}

std::vector<kp::strategy::risklimit::Fill> risk_fills(const std::vector<json::Value>& frames) {
    // _f(x): float(x), or absent on TypeError/ValueError. float(True) is 1.0 in Python.
    const auto f = [](const json::Value* v) -> OptD {
        if (v == nullptr || v->is_null()) return std::nullopt;
        if (v->is_bool()) return v->boolean() ? 1.0 : 0.0;
        if (v->is_number()) return v->num();
        if (v->is_string()) {
            try { return kp::kalshi::py_float_str(v->str()); } catch (...) { return std::nullopt; }
        }
        return std::nullopt;
    };
    std::vector<kp::strategy::risklimit::Fill> out;
    for (const auto& fr : frames) {
        kp::strategy::risklimit::Fill fl;
        fl.count_fp = f(fr.find("count_fp"));
        fl.yes_price_dollars = f(fr.find("yes_price_dollars"));
        fl.fee_cost = f(fr.find("fee_cost"));
        if (const json::Value* s = fr.find("outcome_side"); s && s->is_string()) fl.outcome_side = s->str();
        if (const json::Value* s = fr.find("side"); s && s->is_string()) fl.side = s->str();
        out.push_back(std::move(fl));
    }
    return out;
}

// --- Board ------------------------------------------------------------------------------------
Board::Board(std::string u, std::vector<std::string> d, std::vector<std::string> h)
    : underlying(std::move(u)), daily_series(std::move(d)), hourly_series(std::move(h)) {}

void Board::reset() {
    daily_m.clear();
    hourly_m.clear();
    priced.reset();
    sigma.reset();
    fast_spot.reset();
    fast_detail = json::Value();
    fast_reason.reset();
    move_scale.reset();
    em_map.clear();
    lean_map.clear();
    lean.reset();
    skip.clear();
}

Markets Board::markets() const {
    Markets m = daily_m;
    m.insert(m.end(), hourly_m.begin(), hourly_m.end());
    return m;
}

std::vector<std::string> Board::series() const {
    std::vector<std::string> s = daily_series;
    s.insert(s.end(), hourly_series.begin(), hourly_series.end());
    return s;
}

std::pair<Markets, Markets> Board::discover(CycleIO& io, const kp::time::Instant& now, double mono,
                                            double refresh_s, bool force) {
    const bool due = force || !disc || (mono - disc_t) >= refresh_s;
    if (due) {
        disc = io.discover(*this);
        disc_t = mono;
    }
    return {run04::still_open(disc->first, now.ns), run04::still_open(disc->second, now.ns)};
}

json::Value Board::detail() const {
    const auto* res = spot_res();
    json::Value o = json::Value::make_object({});
    o.set("underlying", json::Value::make_string(underlying));
    o.set("root", root.empty() ? json::Value() : json::Value::make_string(root));
    o.set("n_daily", i64((long long)daily_m.size()));
    o.set("n_hourly", i64((long long)hourly_m.size()));
    o.set("spot", res ? num(res->value) : json::Value());
    // THE LABEL NAMES THE PROVENANCE: the fast spot arrives in the index slot, so resolve stamps
    // it "index"; the record says "fast_futures".
    o.set("spot_source", res == nullptr ? json::Value()
                         : fast_spot ? json::Value::make_string("fast_futures")
                                     : str_or_none(res->source));
    o.set("spot_resolve_source", res ? str_or_none(res->source) : json::Value());
    o.set("sigma", num(sigma));
    o.set("skip", str_or_none(skip));
    o.set("fast_spot", num(fast_spot));
    if (trk) {
        std::lock_guard<std::mutex> g(*trk_mu);
        o.set("basis", trk->detail().to_json());
    } else {
        o.set("basis", json::Value());
    }
    o.set("fast_reason", str(fast_reason));
    o.set("move_scale", num(move_scale));
    o.set("edge", em ? to_json(em->summary()) : json::Value());
    o.set("delta_adj", da ? da->detail().to_json() : json::Value());
    o.set("skew", sk ? sk->detail().to_json() : json::Value());
    return o;
}

// --- the cycle --------------------------------------------------------------------------------
json::Value cycle(CycleState& state, CycleIO& io, std::vector<Board>& boards,
                  orders::RestingBook& book, const CycleArgs& args, kp::strategy::journal::Journal* jrnl,
                  CycleTrace* trace) {
    const double t0 = io.perf();
    const kp::time::Instant now = io.now();
    json::Value rec = json::Value::make_object({});

    // FILLS FIRST, ahead of every gate: a fill is the exchange telling us what we own, and a
    // Massive outage has no bearing on whether a Kalshi order filled. Cash is folded from the RAW
    // frames, so a fill for an order this process did not place still moves the loss limit.
    long n_fills = 0;
    if (io.has_mdfeed) {
        const auto drained = io.drain_fills();
        for (const auto& fl : drained) {
            std::lock_guard<std::recursive_mutex> g(*state.apply_lock);
            n_fills += orders::apply_fill(book, fl, io.mono());
        }
        if (state.risk) state.risk->on_fills(risk_fills(drained));
    }

    // THE HALT CHECK, ahead of the feed gate: a halt must work on a cycle we would skip anyway.
    if (state.risk) {
        const std::string why = state.risk->halt_reason();
        if (!why.empty()) {
            io.err("[HALT] " + why + " -- stopping and cancelling");
            if (state.stop) state.stop->store(true);
            rec.set("skipped", json::Value::make_string("halted"));
            rec.set("reason", json::Value::make_string(why));
            rec.set("fills", i64(n_fills));
            rec.set("risk", to_json(state.risk->summary()));
            return rec;
        }
    }

    // THE FEED GATE, before anything is read off it: a stale quote and a current quote are the same
    // dict, so health comes first and a dead or stale option feed skips the cycle.
    {
        const FeedHealth h = io.col_health();
        if (!h.alive) {
            const bool okc = io.col_reconnect();
            io.err("[feed] pump died (" + (h.pump_error ? *h.pump_error : std::string("None"))
                   + ") -- reconnect " + (okc ? "ok" : "FAILED") + " (#"
                   + std::to_string(io.col_reconnects()) + ")");
            rec.set("skipped", json::Value::make_string("feed_down"));
            rec.set("reconnected", json::Value::make_bool(okc));
            rec.set("fills", i64(n_fills));
            rec.set("feed", io.col_health().raw);
            return rec;
        }
        if (h.quote_age_s && *h.quote_age_s > kp::config::MASSIVE_MAX_QUOTE_AGE_S) {
            char b[160];
            std::snprintf(b, sizeof b, "[feed] no quote in %.1fs (> %ss) -- not quoting this cycle",
                          *h.quote_age_s, kp::pycompat::py_float_repr(kp::config::MASSIVE_MAX_QUOTE_AGE_S).c_str());
            io.err(b);
            rec.set("skipped", json::Value::make_string("feed_stale"));
            rec.set("quote_age_s", json::Value::make_number(r(*h.quote_age_s, 1)));
            rec.set("fills", i64(n_fills));
            rec.set("feed", h.raw);
            return rec;
        }
    }

    // The futures anchor, sampled HERE beside the option snapshot -- one end of the delta
    // increment. One anchor per underlying (SPX on ES, NDX on NQ), all at this instant.
    for (auto& b : boards)
        if (b.da) b.da->anchor_now();

    // THE BOOK COMES OFF THE WEBSOCKET when its coverage clears the floor, else REST.
    std::string book_src = "ws";
    OptD cov;
    json::Value books_json;
    if (io.has_mdfeed && io.feed_alive()) {
        const json::Value* c = nullptr;
        const json::Value fh = io.feed_health();
        c = fh.find("coverage");
        cov = (c && c->is_number()) ? OptD(c->num()) : OptD(0.0);
        if (*cov >= kp::config::WS_BOOK_MIN_COVERAGE)
            books_json = io.feed_books(state.tickers);
        else
            book_src = "rest_fallback";
    } else if (io.has_mdfeed) {
        book_src = "rest_feed_down";
    } else {
        book_src = "rest";
    }

    // The market list (cached), the option snapshot, the index and any REST book: the reference
    // runs these in a thread pool; they are independent reads and the order they complete in
    // does not reach any output.
    const double mono = io.mono();
    std::map<std::string, std::pair<Markets, Markets>> disc;
    for (auto& b : boards) disc[b.underlying] = b.discover(io, now, mono, run04::BOARD_REFRESH_S);
    const kp::massive::OptionSnapshot snap = io.col_snapshot();
    std::vector<std::string> us;
    for (const auto& b : boards) us.push_back(b.underlying);
    const IndexQuotes iq = io.index_quotes(us);
    for (auto& b : boards) {
        b.reset();
        b.daily_m = disc[b.underlying].first;
        b.hourly_m = disc[b.underlying].second;
    }
    if (book_src != "ws") books_json = io.rest_orderbooks(state.tickers);
    if (books_json.is_object())
        if (const json::Value* ob = books_json.find("orderbooks")) books_json = *ob;
    if (book_src != "ws") {
        std::string line = "[book] " + book_src;
        if (cov) line += " (ws coverage " + run04::pct0(*cov) + " < "
                         + run04::pct0(kp::config::WS_BOOK_MIN_COVERAGE) + ")";
        if (io.has_mdfeed) {
            const auto pe = io.feed_pump_error();
            if (pe && !pe->empty()) line += " (" + *pe + ")";
        }
        io.err(line);
    }
    Books books;
    if (books_json.is_object())
        for (const auto& [tk, e] : books_json.obj()) books[tk] = e;

    // PRICE EACH UNDERLYING SEPARATELY, THEN MERGE. A board that cannot price is dropped on its
    // own; the cycle only skips when every board is out.
    Fairs fairs;
    Markets markets;
    for (auto& b : boards) {
        if (b.daily_m.empty() && b.hourly_m.empty()) {
            b.skip = "no_markets";
            continue;
        }
        // THE FAST SPOT, handed in as a substitute INDEX QUOTE so parity, staleness and the
        // divergence alarm all still run and still get the last word.
        std::optional<kp::massive::index::IndexQuote> iq_b;
        if (auto it = iq.find(b.underlying); it != iq.end()) iq_b = it->second;
        OptD fast_spot;
        b.fast_reason = std::string("no_tracker");
        if (b.trk) {
            OptD fmid;
            if (io.has_futures) fmid = io.fut_mid(b.underlying).first;
            kp::pricing::basis::SpotResult sr;
            {
                std::lock_guard<std::mutex> g(*b.trk_mu);
                sr = b.trk->spot(fmid);
            }
            fast_spot = sr.spot;
            b.fast_detail = sr.detail.to_json();
            b.fast_reason = sr.detail.reason;
            if (fast_spot && iq_b) {
                kp::massive::index::IndexQuote q = *iq_b;
                q.value = fast_spot;
                q.ts_ns = io.time_ns();
                iq_b = q;
            }
        }
        b.fast_spot = fast_spot;
        const double sk_mono = io.mono();
        std::optional<vt::SkewObserver> obs;
        if (b.sk) obs = kp::strategy::skewstate::observer(*b.sk, sk_mono);
        const auto* cal = (b.em && b.em->calendar()) ? &*b.em->calendar() : nullptr;
        b.priced = vt::resolve_and_price_boards(b.daily_m, b.hourly_m, snap, b.root, b.expiry,
                                                b.strikes, now, cal, iq_b, obs ? &*obs : nullptr,
                                                run04::py_timestamp_ns(io.now()));
        const auto& res = b.priced->res;
        if (!res.value) {
            b.skip = "no_spot:" + res.reason;
            if (state.guard) state.guard->retire(b.underlying);
            io.err("[spot:" + b.underlying + "] " + (res.reason.empty() ? "None" : res.reason)
                   + " -- board not quoted this cycle");
            continue;
        }
        for (const auto& [tk, bf] : b.priced->fairs) {
            FairDetail fd;
            fd.fair = bf.fair;
            const std::string& why = bf.bracket().reason;
            if (!why.empty()) fd.reason = why;
            fairs[tk] = fd;
        }
        const Markets bm = b.markets();
        markets.insert(markets.end(), bm.begin(), bm.end());
    }
    std::vector<Board*> live_boards;
    for (auto& b : boards)
        if (b.skip.empty()) live_boards.push_back(&b);
    if (live_boards.empty()) {
        std::string why;
        for (const auto& b : boards) why += (why.empty() ? "" : ";") + b.underlying + "=" + b.skip;
        rec.set("skipped", json::Value::make_string("no_spot"));
        rec.set("fills", i64(n_fills));
        rec.set("reason", json::Value::make_string(why));
        return rec;
    }

    // Roll the parity band to follow spot: ONE re-centre over the union of both strips, from the
    // DAILY boards only (the seed in main must match). Rate-limited inside the collector.
    json::Value recentered;
    try {
        std::vector<std::string> want;
        for (auto* b : live_boards) {
            const auto w = kp::pricing::universe::desired_tickers(b->daily_m, b->strikes, b->root,
                                                                  b->expiry, b->spot_res()->value);
            want.insert(want.end(), w.tickers.begin(), w.tickers.end());
        }
        if (!want.empty()) {
            const auto rc = io.col_recenter(want);
            if (rc) {
                recentered = json::Value::make_array({});
                recentered.push(i64((long long)rc->first));
                recentered.push(i64((long long)rc->second));
            }
        }
    } catch (const std::exception& e) {
        io.err("[recenter] skipped (" + exc_line(e, 80) + ")");
    }

    // THE VOL, THE EDGE MANAGER, THE LEAN AND THE DELTA CARRY -- per underlying, one pass, so each
    // board's quotes_by_strike is computed once and shared by the three readers.
    for (auto* b : live_boards) {
        const auto quotes = kp::pricing::universe::quotes_by_strike(snap, b->root, b->expiry, b->strikes);
        const auto fit = kp::pricing::atmvol::fit(quotes, b->strikes);
        b->sigma = (b->em && b->em->calendar()) ? run04::sigma(&fit, *b->em->calendar(), now)
                                                : std::nullopt;
        b->move_scale.reset();
        if (kp::config::ADVERSE_GATE_ENABLED && io.has_futures)
            b->move_scale = io.fut_move_scale(b->underlying, kp::config::QUOTE_EXPOSURE_WINDOW_S,
                                              kp::config::ADVERSE_MOVE_SIGMAS);
        const Markets bm = b->markets();
        b->em_map = b->em->refresh(bm, b->spot_res()->value, b->sigma, now, io.mono(), false,
                                   b->move_scale);

        if (b->lc && b->lc->ok()) {
            try {
                const auto it = iq.find(b->underlying);
                b->lc->observe(it == iq.end() ? nullptr : &it->second);
                // The SAME curve the pricer installed -- a smiled implied chain against a flat
                // realized one would book the smile as a disagreement. `pricer.skew` builds the
                // lazy fit here when nothing was installed, exactly as the reference does.
                const kp::pricing::skew::SkewFit* sf = b->priced->daily.skew();
                auto ch = b->lc->chain(bm, quotes, b->strikes, now, io.mono(), b->spot_res()->value,
                                       std::nullopt, sf);
                b->lean_map = ch.rows;
                b->lean = std::move(ch);
            } catch (const std::exception& e) {
                io.err("[lean:" + b->underlying + "] " + exc_line(e, 70));
            }
        }

        // DELTA-ADJUST, between the edge manager and the ladder, and OFF when the fast spot is on:
        // both carry theo forward on the same futures move, and running both counts it twice.
        if (b->da && !b->fast_spot) {
            for (const auto& m : bm) {
                FairDetail* d = fairs.find(m.ticker);
                if (d == nullptr) continue;
                const em_ns::EdgeRecord* er = b->em_map.find(m.ticker);
                const auto [adj, det] = b->da->adjust(d->fair, er ? er->delta : std::nullopt);
                d->fair_vol = d->fair;
                d->has_fair_vol = true;
                d->fair = adj;
                d->delta_adj = det.delta_adj;
                d->delta_adj_reason = det.reason;
                d->f_move = det.f_move;
            }
        }
    }

    // MERGED VIEWS: capital and write tokens are shared, so from here the cycle is one board.
    em_ns::EdgeMap em_map;
    LeanRows lean_map;
    std::map<std::string, Board*> owner;
    for (auto* b : live_boards) {
        em_map.update(b->em_map);
        lean_map.update(b->lean_map);
        for (const auto& m : b->markets()) owner[m.ticker] = b;
    }
    // EdgeUnion.get: route by ticker; unknown -> the first board's well-formed no-R record.
    const EdgeLookup edges = [&](const std::string& tk) {
        auto it = owner.find(tk);
        if (it != owner.end()) return it->second->em->get(tk);
        return boards.empty() || !boards.front().em ? em_ns::EdgeRecord{} : boards.front().em->get(tk);
    };

    // HAND THIS CYCLE'S PRICERS TO THE FAST GUARD, after pricing and before any order is planned.
    // The guard gets its OWN copies: the reference shares the objects under the GIL.
    if (state.guard) {
        for (auto* b : live_boards) {
            if (b->fast_spot) {
                std::shared_ptr<fc::ShiftablePricer> hp;
                if (b->priced->hourly) hp = fc::HourlyShift::copy_of(*b->priced->hourly);
                state.guard->publish(b->underlying, fc::DailyShift::copy_of(b->priced->daily), hp,
                                     b->daily_m, b->hourly_m, b->fast_spot);
            } else {
                state.guard->retire(b->underlying);
            }
        }
    }

    // CAPITAL SELECTION -- the constraint that binds hardest. Incumbents get hysteresis.
    budget::Theos sel_theos;
    for (const auto& m : markets) {
        const FairDetail* d = fairs.find(m.ticker);
        sel_theos[m.ticker] = d ? d->fair : std::nullopt;
    }
    std::set<std::string> incumbents;
    {
        std::lock_guard<std::recursive_mutex> g(*state.apply_lock);
        for (const auto& t : book.tickers())
            if (book.contracts(t) > 0) incumbents.insert(t);
    }

    // THE BAND first, PER UNDERLYING (sigma-moneyness is measured against that board's forward).
    Bands bands;
    json::Value bsum = json::Value::make_object({});
    for (auto* b : live_boards) {
        std::map<std::string, OptD> em_w;
        for (const auto& [tk, er] : b->em_map) em_w[tk] = er.w;
        auto [bb, bs] = band::classify(b->markets(), b->spot_res()->value, em_w, incumbents);
        for (auto& [tk, c] : bb) bands[tk] = c;
        json::Value s = json::Value::make_object({});
        s.set("n", i64(bs.n));
        s.set("band", i64(bs.band));
        s.set("wing", i64(bs.wing));
        s.set("sigma", json::Value::make_number(bs.sigma));
        s.set("exit_mult", json::Value::make_number(bs.exit_mult));
        bsum.set(b->underlying, s);
    }
    std::map<std::string, std::vector<std::string>> counts_map;
    for (const auto& [tk, c] : bands) counts_map[tk] = c.counts;

    const budget::EdgeViews views = budget::edge_views(em_map);
    const budget::Selection sel = budget::select_markets(markets, &sel_theos, &views, &counts_map,
                                                         nullptr, incumbents);
    const std::set<std::string>& chosen = sel.chosen;

    // Positions off the socket; a market we hold but no longer choose gets no_longer_quoting
    // cancels from reconcile -- exposure-reducing, never deferred.
    json::Value pos_json = io.has_mdfeed ? io.feed_positions() : json::Value::make_object({});
    PositionMap positions;
    kp::strategy::risklimit::Positions risk_pos;
    kp::strategy::risktape::Positions tape_pos;
    long positions_held = 0;
    if (pos_json.is_object())
        for (const auto& [tk, v] : pos_json.obj()) {
            const double q = v.is_number() ? v.num() : 0.0;
            positions[tk] = q;
            risk_pos.emplace_back(tk, q);
            tape_pos[tk] = v.is_number() ? OptD(q) : std::nullopt;
            if (q != 0.0) ++positions_held;
        }

    // MARK THE BOOK. The loss limit reads ACCOUNT equity (REST balance + REST positions, polled by
    // AccountPoller) marked to the Kalshi MID, against the day's opening equity -- the old
    // fill-cash mark to our own theo read +$1,744.50 on a -$899.05 day. It is what the loss limit
    // reads on the NEXT cycle's halt check.
    if (state.risk) {
        const std::optional<kp::strategy::risklimit::AccountSnap> snap =
            io.acct_latest ? io.acct_latest() : std::nullopt;
        kp::strategy::risklimit::Tops tops;
        if (snap)
            for (const auto& [tk, q] : snap->positions) {
                (void)q;
                std::optional<json::Value> ent;
                if (const auto it = books.find(tk); it != books.end()) ent = it->second;
                if (!ent && io.has_mdfeed && io.feed_entry) ent = io.feed_entry(tk);
                const TopOfBook t = top_of_book(ent ? &*ent : nullptr);
                tops[tk] = {t.best_bid, t.best_ask};
            }
        std::map<std::string, OptD> fm;
        for (const auto& [tk, d] : fairs) fm[tk] = d.fair;
        state.risk->mark_account(snap, tops, &fm, io.mono());
    }
    (void)risk_pos;

    // THE RISK TABLE: positions, Greeks and both vol views exist for the same instant only here.
    if (state.risktape && state.risktape->due(io.mono())) {
        for (auto* b : live_boards) {
            const OptD sigma_rv = b->lean ? b->lean->first_sigma_rv() : std::nullopt;
            try {
                const auto* cal = b->em->calendar() ? &*b->em->calendar() : nullptr;
                state.risktape->snapshot(b->underlying, now, tape_pos, &b->em_map, b->markets(),
                                         &b->priced->daily, cal, b->sigma, sigma_rv,
                                         b->spot_res()->value, b->fast_spot, b->hourly_m, io.mono(),
                                         io.time_ns());
            } catch (const std::exception& e) {
                io.err("[risktape:" + b->underlying + "] " + exc_line(e, 80));
            }
        }
    }

    // RE-PRICE WHAT WE ARE ABOUT TO POST, off the spot as of NOW: hold the slow leg (vol), restamp
    // the fast one (spot). A full re-price, not delta * dF. Gated on the fast spot, not the guard.
    long n_repriced = 0;
    double reprice_ms = 0.0;
    if (io.has_futures && !args.no_reprice_at_send) {
        const double t_rp = io.perf();
        for (auto* b : live_boards) {
            std::unordered_set<std::string> hourly_tk;
            for (const auto& m : b->hourly_m) hourly_tk.insert(m.ticker);
            fc::DailyShift dshift(b->priced->daily);
            std::optional<fc::HourlyShift> hshift;
            if (b->priced->hourly) hshift.emplace(*b->priced->hourly);
            const Markets bm = b->markets();
            std::vector<fc::RepriceItem> mine;
            for (const auto& m : bm)
                if (chosen.count(m.ticker))
                    mine.emplace_back(&m, hourly_tk.count(m.ticker)
                                              ? (hshift ? static_cast<fc::ShiftablePricer*>(&*hshift) : nullptr)
                                              : static_cast<fc::ShiftablePricer*>(&dshift));
            if (mine.empty()) continue;
            const OptD fmid = io.fut_mid(b->underlying).first;
            OptD spot_now;
            if (b->trk) {
                std::lock_guard<std::mutex> g(*b->trk_mu);
                spot_now = b->trk->spot(fmid).spot;
            }
            if (!spot_now || !b->fast_spot) continue;
            const OptD base_h = b->priced->hourly ? b->priced->hourly->fit().forward : std::nullopt;
            const auto fresh = fc::reprice_shifted(&dshift, hshift ? &*hshift : nullptr,
                                                   b->priced->daily.spot(), base_h,
                                                   *spot_now - *b->fast_spot, mine);
            for (const auto& [tk, fair] : fresh) {
                FairDetail* d = fairs.find(tk);
                if (d == nullptr) continue;
                d->fair_at_price = d->fair;
                d->fair = fair;
                ++n_repriced;
            }
        }
        reprice_ms = (io.perf() - t_rp) * 1000.0;
    }

    Markets chosen_m;
    for (const auto& m : markets)
        if (chosen.count(m.ticker)) chosen_m.push_back(m);

    orders::Reconciled plan;
    DesiredResult dr;
    {
        // Every touch of the resting book is under the apply lock: the guard thread mutates it.
        std::lock_guard<std::recursive_mutex> g(*state.apply_lock);
        dr = build_desired(chosen_m, fairs, &books, edges, &lean_map, args.tiers, &bands,
                           &positions, book, io.mono(), io.now());
        // Stamp the theo each resting order is measured against, and the touch, for EVERY market
        // (an exiting market still needs a current theo), off the same books snapshot.
        for (const auto& m : markets) {
            const FairDetail* d = fairs.find(m.ticker);
            if (d && d->fair) book.state(m.ticker).theo = d->fair;
            const auto bit = books.find(m.ticker);
            const TopOfBook t = top_of_book(bit == books.end() ? nullptr : &bit->second);
            book.state(m.ticker).set_touch(t.best_bid, t.best_ask);
        }
        plan = orders::reconcile(book, dr.desired, &counts_map, nullptr, io.mono());
    }

    // WRITE BUDGET between the plan and the risk gate: admission only drops whole units, so what it
    // hands on is exposure-neutral or exposure-reducing.
    budget::Theos theos;
    budget::SideSet resting;
    {
        std::lock_guard<std::recursive_mutex> g(*state.apply_lock);
        for (const auto& t : book.tickers()) theos[t] = book.state(t).theo;
        resting = book.resting_sides();
    }
    for (const auto& m : markets) {
        const FairDetail* d = fairs.find(m.ticker);
        theos[m.ticker] = d ? d->fair : std::nullopt;
    }
    const double tokens = io.bucket ? io.bucket->available() : 0.0;
    auto adm = budget::admit(plan.to_place, plan.to_cancel, tokens, &theos, &views, std::nullopt,
                             &resting);
    const auto& bdet = adm.detail;
    if (bdet.n_deferred) {
        char b[256];
        std::snprintf(b, sizeof b, "[budget] %ld admitted / %ld deferred (%.0f tokens of work), %.0f left: ",
                      bdet.n_admitted, bdet.n_deferred, (double)bdet.deferred_cost, bdet.tokens_left);
        const json::Value dj = bdet.to_json();
        const json::Value* dbc = dj.find("deferred_by_class");
        io.err(std::string(b) + (dbc ? py_repr(*dbc) : std::string("{}")));
    }
    if (bdet.reduce_over_budget) {
        char b[200];
        std::snprintf(b, sizeof b, "[budget] ALARM exposure-reducing cancels alone cost %.0f tokens against %.0f available",
                      bdet.reduce_cost, bdet.tokens_in);
        io.err(b);
    }
    std::vector<orders::Order> place = std::move(adm.place), cancel = std::move(adm.cancel);

    // The cancels go in too: apply sends them FIRST, so what rests is held - cancelled + placed.
    std::pair<bool, std::vector<std::string>> risk_ok;
    {
        std::lock_guard<std::recursive_mutex> g(*state.apply_lock);
        risk_ok = orders::within_risk_limits(book, place, cancel);
    }
    if (!risk_ok.first) {
        // Drop the replacement cancels with the places they belonged to; only exposure-REDUCING
        // cancels survive a tripped limit.
        auto kept_cancels = orders::reducing_only(cancel);
        json::Value v3 = json::Value::make_array({});
        for (std::size_t i = 0; i < risk_ok.second.size() && i < 3; ++i)
            v3.push(json::Value::make_string(risk_ok.second[i]));
        io.err("[risk] BLOCKED " + std::to_string(place.size()) + " orders and "
               + std::to_string(cancel.size() - kept_cancels.size()) + " replacement cancels ("
               + std::to_string(kept_cancels.size()) + " reducing cancels still sent): " + py_repr(v3));
        place.clear();
        cancel = std::move(kept_cancels);
    }
    if (trace) { trace->place = place; trace->cancel = cancel; }

    // UNDER THE SAME LOCK THE GUARD TAKES -- only apply, never the whole plan.
    orders::ApplyResult res;
    {
        std::lock_guard<std::recursive_mutex> g(*state.apply_lock);
        res = orders::apply(place, cancel, book, args.live ? io.api : nullptr, args.live,
                            state.max_batch, io.bucket, io.mono(), io.ids);
    }

    // SAY WHAT THE EXCHANGE SAID, deduplicated by message.
    for (const auto* kind : {"place_errors", "cancel_errors"}) {
        const auto& errs = std::string(kind) == "place_errors" ? res.place_errors : res.cancel_errors;
        kp::pycompat::ODict<std::string, long> seen;
        for (const auto& e : errs) {
            const json::Value* msg = nullptr;
            for (const char* k : {"body", "error", "batch_error", "batch_shape"}) {
                const json::Value* v = e.find(k);
                if (v && kp::kalshi::py_truthy(*v)) { msg = v; break; }
            }
            // str(msg): a string as itself, anything else as its repr.
            std::string key = msg == nullptr ? "None" : msg->is_string() ? msg->str() : py_repr(*msg);
            key = head(key, 200);
            const json::Value* n = e.find("n");
            seen[key] += (n && n->is_number() && n->integer()) ? (long)n->integer() : 1L;
        }
        const std::string k(kind);
        for (const auto& [m, n] : seen)
            io.err("[" + k.substr(0, k.size() - 7) + "] x" + std::to_string(n) + " " + m);
    }

    // THE JOURNAL: one row per desired rung, stamped from the OWNING board.
    if (jrnl) {
        for (const auto& [key, rungs] : dr.desired) {
            const auto& [tk, side] = key;
            const json::Value* d = dr.notes.find(tk);
            static const json::Value EMPTY = json::Value::make_object({});
            const json::Value& n = d ? *d : EMPTY;
            const auto oit = owner.find(tk);
            if (oit == owner.end()) continue;
            const Board* ob = oit->second;
            const auto nv = [&](const std::string& k) {
                const json::Value* v = n.find(k);
                return v ? *v : json::Value();
            };
            const kp::strategy::leanchain::Row* rv = lean_map.find(tk);
            for (const auto& rg : rungs) {
                json::Value row = json::Value::make_object({});
                row.set("v", i64(2));
                row.set("ts_ns", i64(io.time_ns()));
                row.set("live", json::Value::make_bool(args.live));
                row.set("ticker", json::Value::make_string(tk));
                row.set("side", json::Value::make_string(side));
                row.set("rung", i64(rg.index));
                row.set("price", json::Value::make_number(rg.price));
                row.set("count", json::Value::make_string(rg.count));
                row.set("edge", num(rg.edge));
                row.set("rung_min_edge", rg.min_edge ? i64(*rg.min_edge) : json::Value());
                row.set("rule", json::Value::make_string(rg.reason));
                row.set("theo", nv("fair"));
                row.set("best_bid", nv("best_bid"));
                row.set("best_ask", nv("best_ask"));
                row.set("R", nv("edge_R"));
                row.set("required_edge", nv("edge_edge"));
                row.set("risk_u", nv("edge_risk_u"));
                row.set("edge_source", nv("edge_source"));
                row.set("theo_vol", nv("fair_vol"));
                row.set("p_rv", rv ? num(rv->p_rv) : json::Value());
                row.set("p_rv_shadow", rv ? num(rv->p_rv_shadow) : json::Value());
                row.set("p_impl_ref", rv ? num(rv->p_ref) : json::Value());
                row.set("sigma_rv", rv ? num(rv->sigma_rv) : json::Value());
                row.set("rv_horizon", rv ? str(rv->rv_horizon) : json::Value());
                row.set("lean_reason", rv ? str_or_none(rv->reason) : json::Value());
                row.set("lean_cents", nv(side + "_lean_cents"));
                row.set("min_edge", nv(side + "_min_edge"));
                row.set("bonus_edge", nv(side + "_bonus"));
                row.set("backoff_edge", nv(side + "_backoff"));
                row.set("delta_adj", nv("delta_adj"));
                row.set("delta_adj_reason", nv("delta_adj_reason"));
                row.set("f_move", nv("f_move"));
                row.set("underlying", json::Value::make_string(ob->underlying));
                row.set("spot", num(ob->spot_res()->value));
                row.set("spot_source", ob->fast_spot ? json::Value::make_string("fast_futures")
                                                     : str_or_none(ob->spot_res()->source));
                row.set("sigma", num(ob->sigma));
                row.set("skew_smoothed", json::Value::make_bool(ob->sk != nullptr));
                jrnl->write(row);
            }
        }
    }

    if (trace) {
        trace->planned = true;
        trace->fairs = fairs;
        trace->em_map = em_map;
        trace->lean_map = lean_map;
        trace->chosen = chosen;
        trace->desired = dr.desired;
        trace->notes = dr.notes;
        trace->apply = res.to_json();
    }

    // THE RECORD, keys in the reference's order.
    long resting_n = 0;
    double contracts = 0.0;
    {
        std::lock_guard<std::recursive_mutex> g(*state.apply_lock);
        resting_n = (long)book.all_orders().size();
        contracts = book.contracts();
    }
    rec.set("ms", json::Value::make_number(r((io.perf() - t0) * 1000.0, 1)));
    rec.set("n_markets", i64((long long)markets.size()));
    long nd = 0, nh = 0;
    for (const auto& b : boards) { nd += (long)b.daily_m.size(); nh += (long)b.hourly_m.size(); }
    rec.set("n_daily", i64(nd));
    rec.set("n_hourly", i64(nh));
    rec.set("n_chosen", i64((long long)chosen.size()));
    rec.set("n_desired", i64((long long)dr.desired.size()));
    rec.set("placed", i64(res.placed));
    rec.set("cancelled", i64(res.cancelled));
    rec.set("kept", i64((long long)plan.kept.size()));
    rec.set("resting", i64(resting_n));
    rec.set("contracts", json::Value::make_number(contracts));
    json::Value bj = json::Value::make_object({});
    for (const auto& b : boards) bj.set(b.underlying, b.detail());
    rec.set("boards", bj);
    rec.set("recentered", recentered);
    rec.set("select", sel.detail.to_json());
    rec.set("band", bsum);
    rec.set("positions_held", i64(positions_held));
    rec.set("book_src", json::Value::make_string(book_src));
    rec.set("ws_coverage", cov ? json::Value::make_number(r(*cov, 3)) : json::Value());
    rec.set("fills", i64(n_fills));
    rec.set("pnl", state.risk && state.risk->pnl() ? json::Value::make_number(r(*state.risk->pnl(), 2))
                                                   : json::Value());
    rec.set("budget", bdet.to_json());
    rec.set("bucket", io.bucket ? io.bucket->detail().to_json() : json::Value());
    rec.set("rate_limited", i64(res.rate_limited));
    rec.set("cancel_gone", i64(res.cancel_gone));
    rec.set("repriced_at_send", i64(n_repriced));
    rec.set("reprice_ms", json::Value::make_number(r(reprice_ms, 3)));
    rec.set("tape", io.tee_detail ? io.tee_detail() : json::Value());
    rec.set("spot_tape", io.spot_tape_detail ? io.spot_tape_detail() : json::Value());
    rec.set("guard", io.guard_detail ? io.guard_detail() : json::Value());
    json::Value feeds = json::Value::make_object({});
    feeds.set("futures", io.has_futures && io.fut_health ? io.fut_health() : json::Value());
    feeds.set("index", io.idx_health ? io.idx_health() : json::Value());
    rec.set("feeds", feeds);
    rec.set("unsent", i64(res.unsent_cancels + res.unsent_places));
    rec.set("orphaned_places", i64(res.orphaned_places));
    rec.set("errors", i64((long long)(res.place_errors.size() + res.cancel_errors.size())));
    return rec;
}

}  // namespace kp::app
