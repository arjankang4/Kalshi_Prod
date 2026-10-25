// strategy/fastcancel against the reference: reprice_shifted on the REAL daily and hourly pricers,
// FastGuard's decision logic stepped one tick at a time over fakes (dry and live, with a replayed
// exchange), and over a real daily pricer. Then strategy/test_fastcancel.py's checks, directly.
// The guard's thread is exercised once at the end, only to prove it starts, ticks and stops.
#include <chrono>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "boards.hpp"
#include "kp/pricing/hourly.hpp"
#include "kp/pricing/pricer.hpp"
#include "kp/pricing/varcal.hpp"
#include "kp/pricing/skew.hpp"
#include "kp/strategy/fastcancel.hpp"
#include "order_fixtures.hpp"
#include "vecio.hpp"

using namespace kp::test::vec;
using namespace kp::test::ofx;
namespace fc = kp::strategy::fastcancel;
namespace ko = kp::strategy::orders;
namespace th = kp::pricing::theo;
using kp::json::Value;
using OptD = std::optional<double>;

namespace {

bool debug_diff(const std::string& tag, const std::string& got, const std::string& want) {
    static std::set<std::string> shown;
    if (got != want && std::getenv("KP_DEBUG") && shown.insert(tag).second)
        std::fprintf(stderr, "[%s]\n  got:  %s\n  want: %s\n", tag.c_str(), got.c_str(), want.c_str());
    return got == want;
}

struct FakeShift : fc::ShiftablePricer {
    OptD spot;
    double slope = 0.01, base = 0.50, origin = 0.0;
    bool dead = false, boom = false;
    int calls = 0;
    FakeShift(double s, double sl, double b = 0.50) : spot(s), slope(sl), base(b), origin(s) {}
    OptD forward() const override { return spot; }
    void set_forward(OptD f) override { spot = f; }
    kp::pricing::skew::SkewFit* curve() override { return nullptr; }   // a fake has no smile
    OptD market_fair(const th::Market&) override {
        ++calls;
        if (boom) throw std::runtime_error("smile blew up");
        if (dead) return std::nullopt;
        return base + slope * (*spot - origin);
    }
};

struct FakeFutures : fc::FuturesFeed {
    std::map<std::string, OptD> mid_;
    std::pair<OptD, OptD> mid(const std::string& u) override {
        const auto it = mid_.find(u);
        return {it == mid_.end() ? std::nullopt : it->second, 0.01};
    }
};

struct FakeTracker : fc::SpotTracker {
    std::string refuse;
    std::pair<OptD, Value> spot(OptD mid) override {
        if (!refuse.empty() || !mid) return {std::nullopt, Value()};
        return {mid, Value()};
    }
};

// The guard holds one Api for its life; each tick replays a different recorded call list.
struct SwitchApi : kp::kalshi::Api {
    kp::kalshi::Api* target = nullptr;
    Value batch_cancel_orders(const std::vector<std::string>& ids) override {
        return target->batch_cancel_orders(ids);
    }
    Value batch_create_orders(const std::vector<Value>& o) override {
        return target->batch_create_orders(o);
    }
};

struct Guard {
    FakeFutures fut;
    FakeTracker spx, ndx;
    ko::RestingBook book;
    SwitchApi api;
    std::unique_ptr<fc::FastGuard> g;
    // Insertion-ordered: the reference's `restored` dict is built in publish order.
    kp::pycompat::ODict<std::string, std::pair<std::shared_ptr<FakeShift>, std::shared_ptr<FakeShift>>>
        pricers;
    Value pulled = Value::make_array({});
    bool ndx_none = false;
};

void unit_checks();
void thread_check();

}  // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "tests/data/golden";
    kp::time::init();
    Tally T("fastcancel");
    T.expect({"ref", "markets", "rp", "rps_curve", "rps", "g_new", "g_pub", "g_retire", "g_tick", "gr_new", "gr_tick"});

    std::string root;
    std::vector<th::Market> markets;
    const auto boards = kp::test::load_boards(dir + "/skew_boards.vec");
    std::unique_ptr<kp::pricing::varcal::VarianceCalendar> cal;

    // rp: one pricer pair per (board, spot, now), reused across moves as the reference reuses it.
    std::string rp_key;
    std::unique_ptr<kp::pricing::pricer::Pricer> rp_daily;
    std::unique_ptr<kp::pricing::hourly::HourlyPricer> rp_hourly;

    // rps: pricers with a smile installed, reused across moves like rp's.
    std::unique_ptr<kp::pricing::skew::SkewFit> rps_curve;
    double rps_F = 0.0;
    std::string rps_key;
    std::unique_ptr<kp::pricing::pricer::Pricer> rps_daily;
    std::unique_ptr<kp::pricing::hourly::HourlyPricer> rps_hourly;

    std::map<long, Guard> guards;
    std::unique_ptr<kp::pricing::pricer::Pricer> real_daily;
    std::map<long, std::pair<std::unique_ptr<ko::RestingBook>, std::unique_ptr<fc::FastGuard>>> rguards;
    std::map<long, std::unique_ptr<FakeFutures>> rfut;
    FakeTracker rtrk;

    for (const auto& line : read(dir, "fastcancel")) {
        const auto w = words(line);
        const std::string& fn = w[0];
        try {
            if (fn == "ref") {
                root = unhex(w[1]);
                cal = std::make_unique<kp::pricing::varcal::VarianceCalendar>(
                    kp::pricing::varcal::VarianceCalendar::load("SPX", "default", root));
                T.hit(fn, true, line);
            } else if (fn == "markets") {
                const Value ms = jhex(w[1]);
                for (const auto& m : ms.arr()) markets.push_back(th::market_from_json(m));
                T.hit(fn, !markets.empty(), line);
            } else if (fn == "rp") {
                const std::string key = w[1] + "|" + w[2] + "|" + w[3];
                const auto& b = boards.at((std::size_t)std::stol(w[1]));
                if (key != rp_key) {
                    rp_key = key;
                    rp_daily = std::make_unique<kp::pricing::pricer::Pricer>(b.board, b.strikes, ohx(w[2]));
                    rp_hourly = std::make_unique<kp::pricing::hourly::HourlyPricer>(
                        b.board, b.strikes, kp::time::Instant{std::stoll(w[3])}, *cal);
                }
                fc::DailyShift ds(*rp_daily);
                fc::HourlyShift hs(*rp_hourly);
                const OptD bd = w[4] == "1" ? ds.forward() : std::nullopt;
                const OptD bh = w[5] == "1" ? hs.forward() : std::nullopt;
                std::vector<fc::RepriceItem> items;
                const Value spec = jhex(w[7]);
                for (const auto& s : spec.arr())
                    items.emplace_back(&markets.at((std::size_t)s.arr()[0].integer()),
                                       s.arr()[1].integer() ? (fc::ShiftablePricer*)&hs
                                                            : (fc::ShiftablePricer*)&ds);
                const auto fairs = fc::reprice_shifted(&ds, &hs, bd, bh, hx(w[6]), items);
                Value got = Value::make_object({});
                for (const auto& [t, f] : fairs) got.set(t, Value::make_number(f));
                T.hit(fn, debug_diff(fn, js(got), unhex(w[8])) && same(rp_daily->spot(), w[9])
                              && same(rp_hourly->fit().forward, w[10]), line);
            } else if (fn == "rps_curve") {
                // The curve both engines install: fitted on the captured board at its parity spot.
                const auto& b = boards.at((std::size_t)std::stol(w[1]));
                const double F = hx(w[2]);
                const OptD wa = kp::pricing::skew::atm_vol(b.board, b.strikes, F);
                rps_curve = std::make_unique<kp::pricing::skew::SkewFit>(
                    kp::pricing::skew::fit(b.board, b.strikes, F, wa));
                rps_F = F;
                T.hit(fn, same(wa, w[3]) && (rps_curve->flat() ? "1" : "0") == w[4], line);
            } else if (fn == "rps") {
                const auto& b = boards.at((std::size_t)std::stol(w[1]));
                const std::string mode = w[3];
                const std::string key = w[1] + "|" + w[2] + "|" + mode;
                if (key != rps_key) {
                    rps_key = key;
                    rps_daily = std::make_unique<kp::pricing::pricer::Pricer>(b.board, b.strikes, rps_F);
                    rps_hourly = std::make_unique<kp::pricing::hourly::HourlyPricer>(
                        b.board, b.strikes, kp::time::Instant{std::stoll(w[2])}, *cal);
                    if (mode == "shared") { rps_daily->use_skew(*rps_curve); rps_hourly->use_skew(*rps_curve); }
                    else if (mode == "daily_only") rps_daily->use_skew(*rps_curve);
                    else (void)rps_daily->skew();                       // the lazy build, at F
                }
                fc::DailyShift ds(*rps_daily);
                fc::HourlyShift hs(*rps_hourly);
                std::vector<fc::RepriceItem> items;
                const Value spec = jhex(w[5]);             // named: a range-for over jhex(..).arr()
                for (const auto& sp : spec.arr())          // would read a destroyed temporary
                    items.emplace_back(&markets.at((std::size_t)sp.arr()[0].integer()),
                                       sp.arr()[1].integer() ? (fc::ShiftablePricer*)&hs
                                                             : (fc::ShiftablePricer*)&ds);
                const auto run = [&](double mv) {
                    const auto f = fc::reprice_shifted(&ds, &hs, ds.forward(), hs.forward(), mv, items);
                    Value got = Value::make_object({});
                    for (const auto& [t, x] : f) got.set(t, Value::make_number(x));
                    return js(got);
                };
                const std::string before = run(0.0), moved = run(hx(w[4])), after = run(0.0);
                // The reference checks the ONE installed curve object; here that is the daily copy,
                // plus the hourly copy when it was installed too. (In the other modes the hourly
                // pricer builds its own curve lazily, at its own forward.)
                const auto* dc = rps_daily->installed_skew();
                const auto* hc = mode == "shared" ? rps_hourly->installed_skew() : nullptr;
                const bool curves_back = (!dc || same(dc->forward(), w[11])) && (!hc || same(hc->forward(), w[11]));
                T.hit(fn, debug_diff("rps.before", before, unhex(w[6])) && debug_diff("rps.moved", moved, unhex(w[7]))
                              && debug_diff("rps.after", after, unhex(w[8])) && same(rps_daily->spot(), w[9])
                              && same(rps_hourly->fit().forward, w[10]) && curves_back, line);
            } else if (fn == "g_new") {
                Guard& G = guards[std::stol(w[1])];
                const bool live = w[2] == "1";
                G.ndx_none = w[5] == "1";
                load_book(G.book, jhex(w[6]));
                std::map<std::string, fc::SpotTracker*> trk{{"SPX", &G.spx},
                                                           {"NDX", G.ndx_none ? nullptr : &G.ndx}};
                Guard* gp = &G;
                G.g = std::make_unique<fc::FastGuard>(
                    G.fut, trk, G.book, &G.api, live, nullptr, std::nullopt, ohx(w[4]),
                    [gp](const std::vector<ko::Order>& cs, const ko::ApplyResult&) {
                        gp->pulled.push(orders_json(cs));
                    },
                    std::stol(w[3]));
                T.hit(fn, true, line);
            } else if (fn == "g_pub") {
                Guard& G = guards.at(std::stol(w[1]));
                const std::string u = w[2];
                const double base = hx(w[3]), slope = hx(w[4]);
                auto d = std::make_shared<FakeShift>(base, slope);
                d->dead = w[5] == "dead";
                d->boom = w[5] == "boom";
                std::shared_ptr<FakeShift> hsh;
                if (w[6] != "-") hsh = std::make_shared<FakeShift>(hx(w[6]), slope * 2);
                std::vector<th::Market> dm, hm;
                const Value dmv = jhex(w[7]), hmv = jhex(w[8]);
                for (const auto& t : dmv.arr()) { th::Market m; m.ticker = t.str(); dm.push_back(m); }
                for (const auto& t : hmv.arr()) { th::Market m; m.ticker = t.str(); hm.push_back(m); }
                G.g->publish(u, d, hsh, dm, hm, ohx(w[9]));
                G.pricers.set(u, {d, hsh});
                T.hit(fn, true, line);
            } else if (fn == "g_retire") {
                guards.at(std::stol(w[1])).g->retire(w[2]);
                T.hit(fn, true, line);
            } else if (fn == "g_tick") {
                Guard& G = guards.at(std::stol(w[1]));
                const Value st = jhex(w[2]);
                for (const auto& [u, v] : st.obj()) {
                    G.fut.mid_[u] = onum(v.arr()[0]);
                    FakeTracker& t = (u == "SPX") ? G.spx : G.ndx;
                    t.refuse = v.arr()[1].is_null() ? "" : v.arr()[1].str();
                }
                ReplayApi replay(jhex(w[7]));
                G.api.target = &replay;
                G.pulled = Value::make_array({});
                std::string err = "-";
                try {
                    G.g->tick();
                } catch (const std::runtime_error& e) {
                    err = e.what();
                }
                Value restored = Value::make_object({});
                for (const auto& [u, p] : G.pricers) {
                    Value r = Value::make_array({});
                    r.push(on(p.first->spot));
                    r.push(p.second ? on(p.second->spot) : Value());
                    restored.set(u, r);
                }
                const bool ok = (w[3] == "-" ? err == "-" : err == unhex(w[3]))
                                && G.g->cancels() == std::stol(w[4])
                                && G.g->repriced() == std::stol(w[5])
                                && debug_diff("g_tick_pulled", js(G.pulled), unhex(w[6]))
                                && replay.ok()
                                && debug_diff("g_tick_book", js(book_json(G.book)), unhex(w[8]))
                                && debug_diff("g_tick_restored", js(restored), unhex(w[9]));
                T.hit(fn, ok, line);
            } else if (fn == "gr_new") {
                const long gid = std::stol(w[1]);
                if (!real_daily) {
                    const auto& b = boards.at(0);
                    real_daily = std::make_unique<kp::pricing::pricer::Pricer>(b.board, b.strikes);
                    // The reference prices every row once (to pick the near-money markets) BEFORE
                    // any guard shifts the forward, so its lazy skew fit is built at the UNSHIFTED
                    // spot. Skipping this builds it inside the first tick, at the shifted one --
                    // the memo would then carry the move forever. Measured: 795 of 900 ticks.
                    for (const auto& m : markets) real_daily->market_fair(m);
                }
                auto& slot = rguards[gid];
                slot.first = std::make_unique<ko::RestingBook>();
                load_book(*slot.first, jhex(w[3]));
                rfut[gid] = std::make_unique<FakeFutures>();
                slot.second = std::make_unique<fc::FastGuard>(
                    *rfut[gid], std::map<std::string, fc::SpotTracker*>{{"SPX", &rtrk}}, *slot.first);
                std::vector<th::Market> sub;
                const Value tks = jhex(w[2]);
                for (const auto& t : tks.arr())
                    for (const auto& m : markets)
                        if (m.ticker == t.str()) { sub.push_back(m); break; }
                // Shared, as the reference shares it: one pricer, shifted and restored per tick.
                slot.second->publish("SPX", std::make_shared<fc::DailyShift>(*real_daily), nullptr,
                                     sub, {}, real_daily->spot());
                T.hit(fn, sub.size() == tks.arr().size(), line);
            } else if (fn == "gr_tick") {
                const long gid = std::stol(w[1]);
                rfut.at(gid)->mid_["SPX"] = hx(w[2]);
                auto& slot = rguards.at(gid);
                slot.second->tick();
                T.hit(fn, slot.second->cancels() == std::stol(w[3])
                              && slot.second->repriced() == std::stol(w[4])
                              && debug_diff(fn, js(book_json(*slot.first)), unhex(w[5]))
                              && same(real_daily->spot(), w[6]), line);
            } else {
                T.hit("unknown_tag:" + fn, false, line);
            }
        } catch (const std::exception& e) {
            T.hit(fn + "_threw", false, std::string(e.what()) + " @ " + line.substr(0, 200));
        }
    }
    unit_checks();
    thread_check();
    return T.report();
}

// ------------------------------------------------------------------------------------------------
namespace {

using kp::test::check;

const char* TK = "KXINXU-T7675";

ko::RestingBook book_with(const std::string& side, double price) {
    ko::RestingBook b;
    ko::Order o;
    o.ticker = TK; o.side = side; o.price = price; o.count = "20.00"; o.order_id = "o1";
    o.client_order_id = "o1";
    b.state(TK).set_side(side, {o});
    return b;
}

struct Rig {
    FakeFutures fut;
    FakeTracker trk;
    ko::RestingBook book;
    std::shared_ptr<FakeShift> pr;
    std::unique_ptr<fc::FastGuard> g;
    Rig(double mid, ko::RestingBook b, std::shared_ptr<FakeShift> p, OptD min_move = std::nullopt)
        : book(std::move(b)), pr(std::move(p)) {
        fut.mid_["SPX"] = mid;
        g = std::make_unique<fc::FastGuard>(fut, std::map<std::string, fc::SpotTracker*>{{"SPX", &trk}},
                                            book, nullptr, false, nullptr, std::nullopt, min_move);
        th::Market m;
        m.ticker = TK;
        g->publish("SPX", pr, nullptr, {m}, {}, 7700.0);
    }
};

std::shared_ptr<FakeShift> fp() { return std::make_shared<FakeShift>(7700.0, 0.01); }

void unit_checks() {
    kp::test::section("a resting BID dies when the forward falls through it");
    {
        Rig r(7698.0, book_with("bid", 0.49), fp());
        r.g->tick();
        check("the order was pulled, and it left the local book",
              r.g->cancels() == 1 && r.book.state(TK).bid.empty());
        Rig r2(7702.0, book_with("bid", 0.49), fp());
        r2.g->tick();
        check("  ... and survives a move in its FAVOUR", r2.g->cancels() == 0 && r2.book.state(TK).bid.size() == 1);
    }
    kp::test::section("an ASK dies on the opposite move");
    {
        Rig r(7702.0, book_with("ask", 0.51), fp());
        r.g->tick();
        check("the ask was pulled", r.g->cancels() == 1);
        Rig r2(7698.0, book_with("ask", 0.51), fp());
        r2.g->tick();
        check("  and survives the other way", r2.g->cancels() == 0);
    }
    kp::test::section("edge EXACTLY zero is pulled -- the floor is `<= 0`, as in plan_side");
    {
        Rig r(7698.0, book_with("bid", 0.48), fp());
        r.g->tick();
        check("a zero-edge order is not held", r.g->cancels() == 1);
    }
    kp::test::section("the move must clear MIN_MOVE_POINTS before anything is re-priced");
    {
        auto p = fp();
        Rig r(7699.99, book_with("bid", 0.49), p, 0.05);
        r.g->tick();
        check("no re-price at all; the pricer was never called", r.g->repriced() == 0 && p->calls == 0);
    }
    kp::test::section("the forward is RESTORED after the tick, even when pricing RAISES");
    {
        auto p = fp();
        Rig r(7690.0, book_with("bid", 0.49), p);
        r.g->tick();
        check("pricer.spot is back where the cycle left it", p->spot == 7700.0);
        auto b = fp();
        b->boom = true;
        Rig r2(7690.0, book_with("bid", 0.49), b);
        bool threw = false;
        try { r2.g->tick(); } catch (const std::runtime_error&) { threw = true; }
        check("  still restored (and the error propagated to the loop)", threw && b->spot == 7700.0);
    }
    kp::test::section("a market that cannot be priced is NOT evidence the order is bad");
    {
        auto d = fp();
        d->dead = true;
        Rig r(7690.0, book_with("bid", 0.49), d);
        r.g->tick();
        check("nothing pulled on a None fair", r.g->cancels() == 0 && r.book.state(TK).bid.size() == 1);
    }
    kp::test::section("no fast spot means no action -- it never falls back to a guess");
    {
        Rig r(7690.0, book_with("bid", 0.49), fp());
        r.trk.refuse = "basis_break_+20.00pts";
        r.g->tick();
        check("a refused basis pulls nothing and re-prices nothing",
              r.g->cancels() == 0 && r.g->repriced() == 0);
    }
    kp::test::section("it only touches markets that are actually RESTING; a retired board is not acted on");
    {
        auto p = fp();
        Rig r(7600.0, ko::RestingBook(), p);
        r.g->tick();
        check("an empty book is no work", r.g->repriced() == 0 && p->calls == 0);
        Rig r2(7690.0, book_with("bid", 0.49), fp());
        r2.g->retire("SPX");
        r2.g->tick();
        check("nothing pulled after retire", r2.g->cancels() == 0);
    }
    kp::test::section("a whole ladder goes in one tick, and only the side the move hurt");
    {
        ko::RestingBook b;
        std::vector<ko::Order> bids;
        int i = 0;
        for (double p : {0.49, 0.485, 0.48}) {
            ko::Order o;
            o.ticker = TK; o.side = "bid"; o.price = p; o.count = "20.00"; o.order_id = "b" + std::to_string(i++);
            bids.push_back(o);
        }
        b.state(TK).set_side("bid", bids);
        ko::Order a;
        a.ticker = TK; a.side = "ask"; a.price = 0.51; a.count = "20.00"; a.order_id = "a1";
        b.state(TK).set_side("ask", {a});
        Rig r(7698.0, std::move(b), fp());
        r.g->tick();
        check("all three bids pulled, the ask untouched",
              r.g->cancels() == 3 && r.book.state(TK).bid.empty() && r.book.state(TK).ask.size() == 1);
    }
    kp::test::section("duplicate order ids are sent once; a None id is not sent at all");
    {
        FakeFutures fut;
        ko::RestingBook b;
        fc::FastGuard g(fut, {}, b);
        ko::Order x;
        x.ticker = "T"; x.side = "bid"; x.price = 0.5; x.count = "1.0"; x.order_id = "x";
        ko::Order n = x;
        n.order_id.reset();
        g.cancel({x, x, n});
        check("one unique id, one cancel counted", g.cancels() == 1);
    }
    kp::test::section("the dry guard never reaches the exchange");
    {
        ForbiddenApi forbid;
        FakeFutures fut;
        fut.mid_["SPX"] = 7690.0;
        FakeTracker trk;
        ko::RestingBook b = book_with("bid", 0.49);
        fc::FastGuard g(fut, {{"SPX", &trk}}, b, &forbid, false);
        th::Market m;
        m.ticker = TK;
        g.publish("SPX", fp(), nullptr, {m}, {}, 7700.0);
        g.tick();
        check("pulled locally, the Api untouched", g.cancels() == 1 && forbid.touched == 0);
    }
}

// The thread: start it, let it tick against a board that must be pulled, stop it. The decision
// logic is pinned above; this only proves the loop drives it and shuts down.
void thread_check() {
    kp::test::section("the guard's own thread ticks, counts, and stops");
    FakeFutures fut;
    fut.mid_["SPX"] = 7698.0;
    FakeTracker trk;
    ko::RestingBook b = book_with("bid", 0.49);
    auto lock = std::make_shared<std::recursive_mutex>();
    fc::FastGuard g(fut, {{"SPX", &trk}}, b, nullptr, false, lock, 0.001);
    th::Market m;
    m.ticker = TK;
    g.publish("SPX", fp(), nullptr, {m}, {}, 7700.0);
    g.start();
    for (int i = 0; i < 2000 && g.cancels() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const bool alive = g.ok();
    g.stop();
    std::lock_guard<std::recursive_mutex> hold(*lock);
    check("it pulled the order from its own thread", g.cancels() == 1 && b.state(TK).bid.empty());
    check("it was alive while running and is not after stop", alive && !g.ok() && g.wakes() > 0);
    check("detail() reports the board and no errors",
          g.detail().boards == std::vector<std::string>{"SPX"} && g.detail().errors == 0);
}

}  // namespace
