// Drive the C++ cycle over a scripted session and write its trace -- the C++ half of the parity
// gate. The Python half is tools/replay/py_replay.py; tools/replay/compare.py diffs the two.
//
//     replay SESSION.json TRACE.jsonl [KALSHI_ROOT]
//
// Every CycleIO member is filled from the session, exactly as py_replay.py patches the reference:
// the same clocks, the same board, the same books and fills, the same fake exchange. Nothing here
// opens a socket; the journal and risk tape go to a temporary directory.
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <map>
#include <unistd.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "kp/app/cycle.hpp"
#include "kp/config.hpp"
#include "kp/kalshi/bucket.hpp"
#include "kp/pycompat/odict.hpp"

namespace json = kp::json;
namespace app = kp::app;
namespace th = kp::pricing::theo;
using OptD = std::optional<double>;

namespace {

struct Clock { long long ns = 0; double mono = 0.0; };
Clock CLK;

OptD onum(const json::Value* v) { return v && v->is_number() ? OptD(v->num()) : std::nullopt; }

kp::massive::index::IndexQuote iq_of(const std::string& u, const json::Value& d) {
    kp::massive::index::IndexQuote q;
    q.underlying = u;
    q.ticker = d.find("ticker")->str();
    q.value = onum(d.find("value"));
    if (const auto* t = d.find("ts_ns"); t && t->is_number()) q.ts_ns = t->integer();
    if (const auto* s = d.find("market_status"); s && s->is_string()) q.market_status = s->str();
    if (const auto* s = d.find("name"); s && s->is_string()) q.name = s->str();
    return q;
}

std::string read_file(const std::string& p) {
    std::ifstream in(p);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::vector<std::string> lines_of(const std::string& p) {
    std::vector<std::string> out;
    std::ifstream in(p);
    std::string l;
    while (std::getline(in, l)) out.push_back(l);
    return out;
}

// The same fake exchange py_replay.py installs as k.batch_create_orders / batch_cancel_orders.
class FakeExchange : public kp::kalshi::Api {
public:
    const json::Value** cyc = nullptr;
    std::string mode() const {
        const json::Value* a = (*cyc)->find("api");
        return a && a->is_string() ? a->str() : "ok";
    }
    json::Value batch_create_orders(const std::vector<json::Value>& orders) override {
        if (orders.empty()) return json::Value::make_array({});
        const std::string m = mode();
        if (m == "http429") throw kp::kalshi::KalshiError(429, R"({"error":{"code":"too_many_requests"}})");
        json::Value res = json::Value::make_array({});
        for (std::size_t i = 0; i < orders.size(); ++i) {
            json::Value r = json::Value::make_object({});
            if (m == "partial" && i % 3 == 2) {
                json::Value e = json::Value::make_object({});
                e.set("code", json::Value::make_string("insufficient_balance"));
                e.set("message", json::Value::make_string("no"));
                r.set("error", e);
            } else {
                r.set("order_id", json::Value::make_string("oid-" + orders[i].find("client_order_id")->str()));
            }
            res.push(r);
        }
        return res;
    }
    json::Value batch_cancel_orders(const std::vector<std::string>& ids) override {
        if (ids.empty()) return json::Value::make_array({});
        const std::string m = mode();
        if (m == "http429") throw kp::kalshi::KalshiError(429, R"({"error":{"code":"too_many_requests"}})");
        json::Value res = json::Value::make_array({});
        for (std::size_t i = 0; i < ids.size(); ++i) {
            json::Value r = json::Value::make_object({});
            r.set("order_id", json::Value::make_string(ids[i]));
            if (m == "not_found" && i % 2 == 0) {
                json::Value e = json::Value::make_object({});
                e.set("code", json::Value::make_string("not_found"));
                r.set("error", e);
            } else {
                r.set("reduced_by", json::Value::make_string("20.00"));
            }
            res.push(r);
        }
        return res;
    }
};

json::Value desired_json(const kp::strategy::orders::Desired& d) {
    json::Value des = json::Value::make_array({});
    for (const auto& [key, rungs] : d) {
        json::Value rs = json::Value::make_array({});
        for (const auto& r : rungs) {
            json::Value rr = json::Value::make_array({});
            rr.push(json::Value::make_int(r.index));
            rr.push(json::Value::make_number(r.price));
            rr.push(json::Value::make_string(r.count));
            rr.push(r.edge ? json::Value::make_number(*r.edge) : json::Value());
            rr.push(json::Value::make_string(r.reason));
            rr.push(r.min_edge ? json::Value::make_int(*r.min_edge) : json::Value());
            rs.push(rr);
        }
        json::Value row = json::Value::make_array({});
        row.push(json::Value::make_string(key.first));
        row.push(json::Value::make_string(key.second));
        row.push(rs);
        des.push(row);
    }
    return des;
}

json::Value orders_json(const std::vector<kp::strategy::orders::Order>& os) {
    json::Value a = json::Value::make_array({});
    for (const auto& o : os) a.push(o.to_json());
    return a;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: replay SESSION.json TRACE.jsonl [KALSHI_ROOT]\n");
        return 2;
    }
    kp::time::init();
    const std::string root = argc > 3 ? argv[3] : "";
    const json::Value sess = json::parse(read_file(argv[1]));
    std::ofstream out(argv[2]);
    const auto tmp = std::filesystem::temp_directory_path() / ("cxxreplay-" + std::to_string(::getpid()));
    std::filesystem::create_directories(tmp);

    const json::Value* cur = nullptr;
    const auto& C = [&]() -> const json::Value& { return *cur; };

    kp::kalshi::TokenBucket bucket(sess.find("bucket")->find("capacity")->num(),
                                   sess.find("bucket")->find("refill")->num(), [] { return CLK.mono; });
    FakeExchange exch;
    exch.cyc = &cur;
    long ids = 0;

    // discovery lists, by (kind, series)
    std::map<std::pair<char, std::string>, app::Markets> by_series;
    for (const auto& [u, d] : sess.find("discover")->obj()) {
        for (const char* kind : {"daily", "hourly"})
            for (const auto& m : d.find(kind)->arr()) {
                const std::string ev = m.find("event_ticker")->str();
                by_series[{kind[0], ev.substr(0, ev.find('-'))}].push_back(th::market_from_json(m));
            }
    }

    const bool live = sess.find("live")->boolean();
    const bool has_fut = sess.find("has_futures")->boolean();
    std::vector<app::Board> boards;
    for (const auto& bj : sess.find("boards")->arr()) {
        std::vector<std::string> d, h;
        for (const auto& s : bj.find("daily_series")->arr()) d.push_back(s.str());
        for (const auto& s : bj.find("hourly_series")->arr()) h.push_back(s.str());
        boards.emplace_back(bj.find("underlying")->str(), d, h);
        auto& b = boards.back();
        b.root = bj.find("root")->str();
        b.expiry = bj.find("expiry")->str();
        for (const auto& k : bj.find("strikes")->arr()) b.strikes.push_back(k.num());
        for (const auto& t : bj.find("tickers")->arr()) b.tickers.push_back(t.str());
        b.em = std::make_unique<kp::strategy::edgemgr::EdgeManager>(b.underlying, std::nullopt,
                                                                    std::nullopt, true, root);
        if (sess.find("smooth_skew")->boolean())
            b.sk = std::make_unique<kp::strategy::skewstate::SkewState>(b.underlying);
        if (sess.find("delta_adj")->boolean()) {
            const std::string u = b.underlying;
            b.ft = std::make_unique<kp::strategy::deltaadj::FuturesTicker>(
                u,
                [&cur, u](const std::string&) -> std::optional<kp::strategy::deltaadj::FuturesRow> {
                    const json::Value* rows = cur->find("ft_rows");
                    const json::Value* r = rows ? rows->find(u) : nullptr;
                    if (r == nullptr || r->is_null()) return std::nullopt;
                    kp::strategy::deltaadj::FuturesRow fr;
                    fr.bid_price = onum(r->find("bid_price"));
                    fr.ask_price = onum(r->find("ask_price"));
                    if (const auto* t = r->find("timestamp")) fr.timestamp = t->integer();
                    return fr;
                },
                kp::strategy::deltaadj::FUTURES_POLL_S, [] { return (double)CLK.ns / 1e9; },
                [] { return CLK.mono; });
            b.ft->set_ticker(u == "SPX" ? "ESU6" : "NQU6");
            b.da = std::make_unique<kp::strategy::deltaadj::DeltaAdjuster>(b.ft.get());
        }
        if (has_fut) b.trk = std::make_unique<kp::pricing::basis::BasisTracker>(b.underlying);
        if (const json::Value* lj = sess.find("lean"); lj && lj->find(b.underlying)) {
            const json::Value* ld = lj->find(b.underlying);
            kp::rvol::live::Fetch f;
            f.closes_ms = [ld](const std::string&, const std::string&) {
                std::vector<std::pair<std::int64_t, double>> v;
                for (const auto& r : ld->find("closes")->arr()) v.emplace_back(r.arr()[0].integer(), r.arr()[1].num());
                return v;
            };
            f.second_bars = [ld](const std::string&, const std::string&, const std::string&) {
                std::vector<kp::rvol::live::Bar> v;
                for (const auto& r : ld->find("proxy")->arr()) {
                    kp::rvol::live::Bar bar;
                    bar.t = r.find("t")->integer();
                    if (const auto* c = r.find("c"); c && c->is_number()) bar.c = c->num();
                    v.push_back(bar);
                }
                return v;
            };
            b.lc = std::make_unique<kp::strategy::leanchain::LeanChain>(b.underlying, f, root);
            const json::Value& c0 = sess.find("cycles")->arr()[0];
            CLK.ns = c0.find("now_ns")->integer();
            CLK.mono = c0.find("mono")->num();
            b.lc->start(kp::time::Instant{CLK.ns / 1000 * 1000});
        }
    }

    auto jrnl = kp::strategy::journal::open_journal(tmp.string(), "replay", "quotes.jsonl");
    auto rjrnl = kp::strategy::journal::open_journal(tmp.string(), "replay", "risk.jsonl");
    kp::strategy::risktape::RiskTape rtape(rjrnl.get());
    kp::strategy::risklimit::SessionRisk risk(std::nullopt, (tmp / "HALT").string());
    risk.set_state_dir((tmp / "risk").string());
    risk.set_wall_clock([] { return kp::time::Instant{CLK.ns / 1000 * 1000}; });
    kp::strategy::orders::RestingBook book;

    app::CycleState state;
    for (const auto& t : sess.find("state_tickers")->arr()) state.tickers.push_back(t.str());
    state.max_batch = (long)sess.find("max_batch")->integer();
    state.risk = &risk;
    state.risktape = &rtape;
    std::atomic<bool> stop{false};
    state.stop = &stop;
    app::CycleArgs args{live, sess.find("tiers")->boolean(), sess.find("no_reprice_at_send")->boolean()};

    std::vector<std::string> errs;
    std::vector<json::Value> picked;
    long reconnects = 0;
    app::CycleIO io;
    io.now = [] { return kp::time::Instant{CLK.ns}; };
    io.mono = [] { return CLK.mono; };
    io.time_ns = [] { return CLK.ns; };
    io.perf = [] { return 0.0; };
    io.has_mdfeed = sess.find("has_mdfeed")->boolean();
    io.drain_fills = [&] {
        std::vector<json::Value> v;
        for (const auto& f : C().find("fills")->arr()) v.push_back(f);
        v.insert(v.end(), picked.begin(), picked.end());
        return v;
    };
    io.feed_alive = [&] { return C().find("feed_alive")->boolean(); };
    io.feed_health = [&] {
        json::Value h = json::Value::make_object({});
        h.set("coverage", *C().find("coverage"));
        h.set("pump_error", *C().find("pump_error"));
        return h;
    };
    io.feed_pump_error = [&]() -> std::optional<std::string> {
        const auto* p = C().find("pump_error");
        return p && p->is_string() ? std::optional<std::string>(p->str()) : std::nullopt;
    };
    const auto books_for = [&](const std::vector<std::string>& tickers) {
        json::Value o = json::Value::make_object({});
        const json::Value* b = C().find("books");
        for (const auto& t : tickers)
            if (const auto* e = b->find(t)) o.set(t, *e);
        return o;
    };
    io.feed_books = books_for;
    io.rest_orderbooks = books_for;
    io.feed_positions = [&] { return *C().find("positions"); };
    io.feed_entry = [&](const std::string& t) -> std::optional<json::Value> {
        const json::Value* e = C().find("books")->find(t);
        return e ? std::optional<json::Value>(*e) : std::nullopt;
    };
    io.acct_latest = [&]() -> std::optional<kp::strategy::risklimit::AccountSnap> {
        const json::Value* a = C().find("acct");
        if (a == nullptr || a->is_null()) return std::nullopt;
        kp::strategy::risklimit::AccountSnap s;
        s.t = a->arr()[0].num();
        s.balance = a->arr()[1].num();
        for (const auto& [tk, q] : a->arr()[2].obj()) s.positions.emplace_back(tk, q.num());
        return s;
    };
    io.col_health = [&] {
        const json::Value& c = *C().find("col");
        app::FeedHealth h;
        h.alive = c.find("alive")->boolean();
        if (const auto* p = c.find("pump_error"); p && p->is_string()) h.pump_error = p->str();
        h.quote_age_s = onum(c.find("quote_age_s"));
        h.raw = c;
        return h;
    };
    io.col_reconnect = [&] { ++reconnects; return C().find("reconnect")->boolean(); };
    io.col_reconnects = [&] { return reconnects; };
    io.col_snapshot = [&] {
        kp::massive::OptionSnapshot s;
        for (const auto& [sym, q] : C().find("snap")->obj()) {
            const auto& a = q.arr();
            kp::massive::OptionQuote oq;
            oq.q.bp = onum(&a[0]);
            oq.q.ap = onum(&a[1]);
            oq.q.bs = onum(&a[2]);
            oq.q.as = onum(&a[3]);
            oq.q.present = true;
            oq.t_ms = a[4].integer();
            s[sym] = oq;
        }
        return s;
    };
    io.col_recenter = [&](const std::vector<std::string>&) -> std::optional<std::pair<std::size_t, std::size_t>> {
        const json::Value& r = *C().find("recenter");
        if (r.is_null()) return std::nullopt;
        if (r.is_string()) {
            const std::string s = r.str().substr(6);         // "error:<Type>:<msg>"
            const auto c = s.find(':');
            throw kp::kalshi::PyNamedError(s.substr(0, c), s.substr(c + 1));
        }
        return std::make_pair((std::size_t)r.arr()[0].integer(), (std::size_t)r.arr()[1].integer());
    };
    io.discover = [&](const app::Board& b) {
        app::Markets d, h;
        for (const auto& s : b.daily_series) {
            auto it = by_series.find({'d', s});
            if (it != by_series.end()) d.insert(d.end(), it->second.begin(), it->second.end());
        }
        for (const auto& s : b.hourly_series) {
            auto it = by_series.find({'h', s});
            if (it != by_series.end()) h.insert(h.end(), it->second.begin(), it->second.end());
        }
        return std::make_pair(d, h);
    };
    io.index_quotes = [&](const std::vector<std::string>& us) {
        app::IndexQuotes out;
        for (const auto& [u, d] : C().find("index")->obj())
            if (std::find(us.begin(), us.end(), u) != us.end()) out[u] = iq_of(u, d);
        return out;
    };
    io.has_futures = has_fut;
    std::map<std::string, int> fut_reads;
    io.fut_mid = [&](const std::string& u) -> std::pair<OptD, OptD> {
        // first read per underlying per cycle = "fut", every later one = "fut2" (see make_session)
        const json::Value* f = C().find(fut_reads[u]++ == 0 ? "fut" : "fut2");
        const json::Value* v = f ? f->find(u) : nullptr;
        if (v == nullptr || v->is_null()) return {std::nullopt, std::nullopt};
        return {v->arr()[0].num(), v->arr()[1].num()};
    };
    io.fut_move_scale = [&](const std::string& u, double, double) -> OptD {
        const json::Value* f = C().find("move_scale");
        return f ? onum(f->find(u)) : std::nullopt;
    };
    io.bucket = &bucket;
    io.api = live ? &exch : nullptr;
    io.ids = [&] {
        char b[64];
        std::snprintf(b, sizeof b, "00000000-0000-4000-8000-%012ld", ++ids);
        return std::string(b);
    };
    io.err = [&](const std::string& s) {
        // print() of a string with embedded newlines is several lines, as splitlines() sees them
        std::size_t p0 = 0;
        for (std::size_t p = 0; p <= s.size(); ++p)
            if (p == s.size() || s[p] == '\n') { errs.push_back(s.substr(p0, p - p0)); p0 = p + 1; }
    };

    std::size_t jseen = 0, rseen = 0;
    long i = 0;
    for (const auto& c : sess.find("cycles")->arr()) {
        cur = &c;
        CLK.ns = c.find("now_ns")->integer();
        CLK.mono = c.find("mono")->num();
        errs.clear();
        picked.clear();
        fut_reads.clear();
        {
            const auto os = book.all_orders();
            if (const auto* fp = c.find("fill_picks"))
                for (const auto& p : fp->arr()) {
                    if (os.empty()) break;
                    const auto& o = os[(std::size_t)(p.arr()[0].integer() % (long long)os.size())];
                    json::Value f = json::Value::make_object({});
                    f.set("market_ticker", json::Value::make_string(o.ticker));
                    f.set("order_id", o.order_id ? json::Value::make_string(*o.order_id) : json::Value());
                    f.set("count_fp", p.arr()[1]);
                    f.set("yes_price_dollars", p.arr()[2]);
                    f.set("side", json::Value::make_string(o.side == "bid" ? "yes" : "no"));
                    f.set("outcome_side", json::Value::make_string(o.side == "bid" ? "yes" : "no"));
                    f.set("fee_cost", json::Value::make_string("0.0000"));
                    picked.push_back(f);
                }
        }
        if (const auto* bo = c.find("basis_obs"))
            for (const auto& ob : bo->arr()) {
                const auto& a = ob.arr();
                for (auto& b : boards)
                    if (b.underlying == a[0].str()) {
                        CLK.mono = a[3].num();
                        std::optional<kp::massive::index::IndexQuote> q = iq_of(b.underlying, a[2]);
                        b.trk->observe(a[1].num(), q, CLK.mono);
                    }
            }
        CLK.mono = c.find("mono")->num();
        for (auto& b : boards)
            if (b.ft) b.ft->poll_once();

        app::CycleTrace tr;
        json::Value rec;
        try {
            rec = app::cycle(state, io, boards, book, args, jrnl.get(), &tr);
        } catch (const std::exception& e) {
            rec = json::Value::make_object({});
            rec.set("error", json::Value::make_string(kp::kalshi::py_type_name(e)));
            rec.set("msg", json::Value::make_string(kp::kalshi::utf8_prefix(e.what(), 200)));
        }
        jrnl->flush();
        rjrnl->flush();
        const auto jl = lines_of(jrnl->path());
        const auto rl = lines_of(rjrnl->path());
        json::Value o = json::Value::make_object({});
        o.set("i", json::Value::make_int(i));
        json::Value r2 = json::Value::make_object({});
        for (const auto& [k, v] : rec.obj())
            if (k != "ms" && k != "reprice_ms") r2.set(k, v);
        o.set("rec", r2);
        json::Value se = json::Value::make_array({});
        for (const auto& e : errs) se.push(json::Value::make_string(e));
        o.set("stderr", se);
        o.set("book", orders_json(book.all_orders()));
        o.set("risk", app::to_json(risk.summary()));
        json::Value jj = json::Value::make_array({}), rr = json::Value::make_array({});
        for (std::size_t k = jseen; k < jl.size(); ++k) jj.push(json::Value::make_string(jl[k]));
        for (std::size_t k = rseen; k < rl.size(); ++k) rr.push(json::Value::make_string(rl[k]));
        o.set("journal", jj);
        o.set("risktape", rr);
        jseen = jl.size();
        rseen = rl.size();
        if (tr.planned) {
            json::Value ch = json::Value::make_array({});
            for (const auto& t : tr.chosen) ch.push(json::Value::make_string(t));
            o.set("chosen", ch);
            json::Value fj = json::Value::make_object({});
            for (const auto& [tk, d] : tr.fairs) {
                json::Value a = json::Value::make_array({});
                for (OptD v : {d.fair, d.fair_vol, d.delta_adj, d.f_move, d.fair_at_price})
                    a.push(v ? json::Value::make_number(*v) : json::Value());
                fj.set(tk, a);
            }
            o.set("fairs", fj);
            o.set("desired", desired_json(tr.desired));
            json::Value nj = json::Value::make_object({});
            for (const auto& [tk, n] : tr.notes) nj.set(tk, n);
            o.set("notes", nj);
            o.set("place", orders_json(tr.place));
            o.set("cancel", orders_json(tr.cancel));
            o.set("apply", tr.apply);
        }
        out << json::dumps(o) << "\n";
        ++i;
    }
    std::filesystem::remove_all(tmp);
    return 0;
}
