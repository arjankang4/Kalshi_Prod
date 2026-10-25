// strategy/orders against the reference: CPython's compensated sum, order_value / pair ids / the
// zombie hold, plan_side, reconcile, the risk gate, apply_fill, apply against a replayed exchange,
// and run_04's cycle end to end on a persistent book. Then the behaviours strategy/test_orders.py
// pins, as direct checks with the same labels.
#include <algorithm>
#include <cstdlib>
#include <set>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "kp/config.hpp"
#include "kp/strategy/budget.hpp"
#include "kp/strategy/orders.hpp"
#include "order_fixtures.hpp"
#include "vecio.hpp"

using namespace kp::test::vec;
using namespace kp::test::ofx;
namespace ko = kp::strategy::orders;
namespace kb = kp::strategy::budget;
namespace kq = kp::strategy::quoting;
using kp::json::Value;

namespace {

Value triple(const std::vector<ko::Order>& a, const std::vector<ko::Order>& b,
             const std::vector<ko::Order>& c) {
    Value v = Value::make_array({});
    v.push(orders_json(a));
    v.push(orders_json(b));
    v.push(orders_json(c));
    return v;
}

Value strings_json(std::vector<std::string> v, bool sort) {
    if (sort) std::sort(v.begin(), v.end());
    Value a = Value::make_array({});
    for (auto& s : v) a.push(Value::make_string(s));
    return a;
}

ko::Desired desired_from(const Value& arr) {
    ko::Desired d;
    for (const auto& e : arr.arr())
        d.set({e.arr()[0].str(), e.arr()[1].str()}, rungs_from(e.arr()[2]));
    return d;
}

kb::Theos theos_from(const Value& obj) {
    kb::Theos t;
    for (const auto& [k, v] : obj.obj()) t[k] = onum(v);
    return t;
}

kb::EdgeViews em_from(const Value& obj) {
    kb::EdgeViews em;
    for (const auto& [k, v] : obj.obj()) {
        kb::EdgeView e;
        e.risk_u = onum(v.find("risk_u"));
        e.R = onum(v.find("R"));
        if (const Value* q = v.find("quotable")) e.quotable = q->boolean();
        em[k] = e;
    }
    return em;
}

struct Session {
    ko::RestingBook book;
    std::shared_ptr<FakeClock> clock;
    std::unique_ptr<kp::kalshi::TokenBucket> bucket;
    bool live = false;
    long mb = 20;
    ko::IdSource ids;
    std::map<std::string, std::vector<std::string>> counts;
    std::vector<ko::Order> place, cancel;
};

// Run one apply case; returns true when result, book, bucket and the call sequence all match.
bool apply_case(const Value& inp, const Value& calls, const std::string& want_res,
                const std::string& want_book, const std::string& want_bucket) {
    ko::RestingBook book;
    load_book(book, *inp.find("book"));
    const bool live = inp.find("live")->boolean();
    std::unique_ptr<kp::kalshi::TokenBucket> bucket;
    const Value* bj = inp.find("budget");
    if (bj != nullptr && !bj->is_null()) {
        auto clk = std::make_shared<FakeClock>();
        clk->t = bj->find("t0")->num();
        clk->step = bj->find("step")->num();
        bucket = std::make_unique<kp::kalshi::TokenBucket>(bj->find("cap")->num(),
                                                           bj->find("rate")->num(),
                                                           [clk] { return (*clk)(); });
    }
    ReplayApi api(calls, bucket.get());
    ForbiddenApi forbid;
    const auto res = ko::apply(orders_from(*inp.find("place")), orders_from(*inp.find("cancel")),
                               book, live ? (kp::kalshi::Api*)&api : (kp::kalshi::Api*)&forbid,
                               live, inp.find("max_batch")->integer(), bucket.get(),
                               inp.find("now")->num(), counter_ids(inp.find("ids")->str()));
    const std::string got_bucket = bucket ? js(bucket->detail().to_json()) : "null";
    return forbid.touched == 0 && (!live || api.ok()) && js(res.to_json()) == want_res
        && js(book_json(book)) == want_book && got_bucket == want_bucket;
}

void unit_checks();

// KP_DEBUG=1 prints the first got/want pair of each mismatching tag.
bool debug_diff(const std::string& tag, const std::string& got, const std::string& want) {
    static std::set<std::string> shown;
    if (got != want && std::getenv("KP_DEBUG") && shown.insert(tag).second)
        std::fprintf(stderr, "[%s]\n  got:  %s\n  want: %s\n", tag.c_str(), got.c_str(), want.c_str());
    return got == want;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "tests/data/golden";
    Tally T("orders");
    T.expect({"pysum", "ov", "pid", "zh", "plan", "plan_err", "recon", "risk", "reduce", "fbook",
              "fill", "apply", "cy_new", "cy_fill", "cy_stamp", "cy_recon", "cy_admit", "cy_risk",
              "cy_apply"});
    std::unique_ptr<ko::RestingBook> fbook;
    std::map<long, Session> sessions;

    for (const auto& line : read(dir, "orders")) {
        const auto w = words(line);
        const std::string& fn = w[0];
      try {
        if (fn == "pysum") {
            ko::PySum s;
            const Value xs = jhex(w[1]);
            for (const auto& x : xs.arr()) s.add(x.num());
            T.hit(fn, same(s.result(), w[2]), line);
        } else if (fn == "ov") {
            const std::string count = w[3] == "None" ? "" : unhex(w[3]);
            T.hit(fn, same(ko::order_value(w[1], ohx(w[2]), count), w[4]), line);
        } else if (fn == "pid") {
            T.hit(fn, ko::pair_id(w[1], w[2], hx(w[3])) == unhex(w[4]), line);
        } else if (fn == "zh") {
            T.hit(fn, (ko::zombie_held(w[1], ohx(w[2]), ohx(w[3]), ohx(w[4]), ohx(w[5]), ohx(w[6]))
                           ? "1" : "0") == w[7], line);
        } else if (fn == "plan") {
            const Value in = jhex(w[1]);
            std::vector<std::string> counts;
            for (const auto& c : in.find("counts")->arr()) counts.push_back(c.str());
            try {
                const auto sp = ko::plan_side(
                    in.find("ticker")->str(), in.find("side")->str(), onum(in.find("theo")),
                    rungs_from(*in.find("rungs")), orders_from(*in.find("live")), counts,
                    (int)in.find("front")->integer(), (int)in.find("back")->integer(),
                    onum(in.find("touch")), onum(in.find("zat")), onum(in.find("zt")),
                    onum(in.find("now")));
                T.hit(w[2] == "ERR" ? "plan_err" : fn,
                      w[2] != "ERR" && js(triple(sp.cancels, sp.places, sp.keeps)) == unhex(w[2]),
                      line);
            } catch (const std::invalid_argument&) {
                T.hit("plan_err", w[2] == "ERR", line);
            }
        } else if (fn == "recon") {
            const Value in = jhex(w[1]);
            ko::RestingBook book;
            load_book(book, *in.find("book"));
            const std::string mode = in.find("mode")->str();
            std::map<std::string, std::vector<std::string>> cmap;
            std::vector<std::string> ctup;
            if (mode == "map")
                for (const auto& [k, v] : in.find("counts")->obj())
                    for (const auto& c : v.arr()) cmap[k].push_back(c.str());
            if (mode == "tuple")
                for (const auto& c : in.find("counts")->arr()) ctup.push_back(c.str());
            try {
                const auto r = ko::reconcile(book, desired_from(*in.find("desired")),
                                             mode == "map" ? &cmap : nullptr,
                                             mode == "tuple" ? &ctup : nullptr,
                                             in.find("now")->num());
                Value got = triple(r.to_place, r.to_cancel, r.kept);
                got.push(book_json(book));
                T.hit(fn, w[2] != "ERR" && js(got) == unhex(w[2]), line);
            } catch (const std::invalid_argument&) {
                T.hit("recon_err", w[2] == "ERR", line);
            }
        } else if (fn == "risk") {
            const Value in = jhex(w[1]);
            ko::RestingBook book;
            load_book(book, *in.find("book"));
            const auto [ok, viol] = ko::within_risk_limits(book, orders_from(*in.find("place")),
                                                           orders_from(*in.find("cancel")));
            T.hit(fn, (ok ? "1" : "0") == w[2] && js(strings_json(viol, true)) == unhex(w[3]),
                  line);
        } else if (fn == "reduce") {
            T.hit(fn, js(orders_json(ko::reducing_only(orders_from(jhex(w[1]))))) == unhex(w[2]),
                  line);
        } else if (fn == "fbook") {
            fbook = std::make_unique<ko::RestingBook>();
            load_book(*fbook, jhex(w[1]));
            T.hit(fn, js(book_json(*fbook)) == unhex(w[1]), line);
        } else if (fn == "fill") {
            const int r = ko::apply_fill(*fbook, jhex(w[1]), hx(w[2]));
            T.hit(fn, r == std::stoi(w[3]) && js(book_json(*fbook)) == unhex(w[4]), line);
        } else if (fn == "apply") {
            T.hit(fn, apply_case(jhex(w[1]), jhex(w[2]), unhex(w[3]), unhex(w[4]), unhex(w[5])),
                  line);
        } else if (fn == "cy_new") {
            Session& s = sessions[std::stol(w[1])];
            s.live = w[2] == "1";
            s.mb = std::stol(w[3]);
            s.clock = std::make_shared<FakeClock>();
            s.clock->t = hx(w[4]);
            s.clock->step = hx(w[5]);
            auto clk = s.clock;
            s.bucket = std::make_unique<kp::kalshi::TokenBucket>(hx(w[6]), hx(w[7]),
                                                                 [clk] { return (*clk)(); });
            s.ids = counter_ids(w[8]);
            const Value cm = jhex(w[9]);
            for (const auto& [k, v] : cm.obj())
                for (const auto& c : v.arr()) s.counts[k].push_back(c.str());
            T.hit(fn, true, line);
        } else if (fn.rfind("cy_", 0) == 0) {
            Session& s = sessions.at(std::stol(w[1]));
            if (fn == "cy_fill") {
                T.hit(fn, ko::apply_fill(s.book, jhex(w[2]), hx(w[3])) == std::stoi(w[4]), line);
            } else if (fn == "cy_stamp") {
                const Value stamps = jhex(w[2]);
                for (const auto& e : stamps.arr()) {
                    auto& st = s.book.state(e.arr()[0].str());
                    if (!e.arr()[1].is_null()) st.theo = e.arr()[1].num();
                    st.set_touch(onum(e.arr()[2]), onum(e.arr()[3]));
                }
                T.hit(fn, true, line);
            } else if (fn == "cy_recon") {
                const auto r = ko::reconcile(s.book, desired_from(jhex(w[2])), &s.counts, nullptr,
                                             hx(w[3]));
                s.place = r.to_place;
                s.cancel = r.to_cancel;
                T.hit(fn, debug_diff(fn, js(triple(r.to_place, r.to_cancel, r.kept)), unhex(w[4])),
                      line);
            } else if (fn == "cy_admit") {
                const double tokens = s.bucket->available();
                const kb::Theos theos = theos_from(jhex(w[3]));
                const kb::EdgeViews em = em_from(jhex(w[4]));
                const auto resting = s.book.resting_sides();
                auto a = kb::admit(s.place, s.cancel, tokens, &theos, &em, std::nullopt, &resting);
                s.place = a.place;
                s.cancel = a.cancel;
                Value got = Value::make_array({});
                got.push(orders_json(a.place));
                got.push(orders_json(a.cancel));
                got.push(a.detail.to_json());
                T.hit(fn, same(tokens, w[2]) && js(got) == unhex(w[5]), line);
            } else if (fn == "cy_risk") {
                const auto [ok, viol] = ko::within_risk_limits(s.book, s.place, s.cancel);
                if (!ok) {
                    s.cancel = ko::reducing_only(s.cancel);
                    s.place.clear();
                }
                T.hit(fn, (ok ? "1" : "0") == w[2] && js(strings_json(viol, true)) == unhex(w[3]),
                      line);
            } else if (fn == "cy_apply") {
                ReplayApi api(jhex(w[3]), s.bucket.get());
                ForbiddenApi forbid;
                const auto res = ko::apply(s.place, s.cancel, s.book,
                                           s.live ? (kp::kalshi::Api*)&api : (kp::kalshi::Api*)&forbid,
                                           s.live, s.mb, s.bucket.get(), hx(w[2]), s.ids);
                T.hit(fn, forbid.touched == 0 && (!s.live || api.ok())
                              && js(res.to_json()) == unhex(w[4])
                              && js(book_json(s.book)) == unhex(w[5])
                              && js(s.bucket->detail().to_json()) == unhex(w[6]), line);
            } else {
                T.hit("unknown_tag:" + fn, false, line);
            }
        } else {
            T.hit("unknown_tag:" + fn, false, line);
        }
      } catch (const std::exception& e) {
        // A throw the reference did not make is a mismatch, reported against its own tag.
        T.hit(fn + "_threw", false, std::string(e.what()) + " @ " + line.substr(0, 200));
      }
    }

    unit_checks();
    return T.report();
}

// ------------------------------------------------------------------------------------------------
// strategy/test_orders.py, check for check where it is a behaviour rather than a restatement of a
// vector. Labels are the reference's, so a failure greps against the Python.
namespace {

using kp::test::check;
const std::vector<std::string> C3{"20.00", "20.00", "20.00"};

std::vector<kq::Rung> rungs(const std::vector<double>& prices,
                            const std::vector<std::string>& counts = C3) {
    std::vector<kq::Rung> out;
    for (std::size_t i = 0; i < prices.size(); ++i)
        out.push_back({(int)i, prices[i], counts[std::min(i, counts.size() - 1)], 0.01, "t",
                       std::nullopt});
    return out;
}

ko::Order mk(const std::string& t, const std::string& side, double price, const std::string& count,
             std::optional<std::string> oid = std::nullopt, const std::string& reason = "",
             const std::string& pair = "") {
    ko::Order o;
    o.ticker = t; o.side = side; o.price = price; o.count = count; o.order_id = std::move(oid);
    o.reason = reason; o.pair = pair;
    return o;
}

ko::MarketState& seed(ko::RestingBook& b, const std::string& t, const std::string& side,
                      double theo, const std::vector<double>& prices,
                      const std::vector<std::string>& counts = C3) {
    auto& st = b.state(t);
    st.theo = theo;
    std::vector<ko::Order> rows;
    for (std::size_t i = 0; i < prices.size(); ++i)
        rows.push_back(mk(t, side, prices[i], counts[std::min(i, counts.size() - 1)],
                          "o" + std::to_string(i)));
    st.set_side(side, rows);
    return st;
}

std::vector<double> prices(const std::vector<ko::Order>& rows) {
    std::vector<double> p;
    for (const auto& o : rows) p.push_back(kp::pycompat::py_round_nd(o.price.value(), 4));
    std::sort(p.begin(), p.end());
    return p;
}

// A client whose Nth create batch 429s; cancels always succeed with reduced_by 10. Charges the
// bucket as the transport would.
struct Rejecting : kp::kalshi::Api {
    int fail_on = 1, creates = 0, cancels = 0;
    kp::kalshi::TokenBucket* bucket = nullptr;
    bool cancel_fails = false, once500 = false;
    Value batch_cancel_orders(const std::vector<std::string>& ids) override {
        ++cancels;
        if (bucket) bucket->debit(2.0 * (double)ids.size());
        Value out = Value::make_array({});
        for (const auto& i : ids) {
            Value r;
            r.set("order_id", Value::make_string(i));
            r.set("reduced_by", Value::make_int(cancel_fails ? 0 : 10));
            out.push(r);
        }
        return out;
    }
    Value batch_create_orders(const std::vector<Value>& bodies) override {
        ++creates;
        if (bucket) bucket->debit(10.0 * (double)bodies.size());
        if (once500 && creates == 1) throw kp::kalshi::KalshiError(500, "boom");
        if (!once500 && creates == fail_on)
            throw kp::kalshi::KalshiError(429, "{\"error\":{\"code\":\"too_many_requests\"}}");
        Value out = Value::make_array({});
        for (std::size_t i = 0; i < bodies.size(); ++i) {
            Value r;
            r.set("order_id", Value::make_string("n" + std::to_string(creates) + "-" + std::to_string(i)));
            out.push(r);
        }
        return out;
    }
};

void unit_checks() {
    kp::test::section("cold start");
    {
        auto sp = ko::plan_side("T", "bid", 0.42, rungs({0.41, 0.40, 0.39}), {}, C3);
        check("places all three", sp.cancels.empty() && sp.places.size() == 3 && sp.keeps.empty());
        check("at the ladder prices", prices(sp.places) == std::vector<double>{0.39, 0.40, 0.41});
    }
    kp::test::section("the deadband: nothing changed, nothing sent");
    {
        ko::RestingBook b; seed(b, "T", "bid", 0.42, {0.41, 0.40, 0.39});
        auto sp = ko::plan_side("T", "bid", 0.42, rungs({0.41, 0.40, 0.39}), b.state("T").bid, C3);
        check("sends nothing", sp.cancels.empty() && sp.places.empty());
        check("keeps all three", sp.keeps.size() == 3);
    }
    kp::test::section("theo moved our way: the front and back deadbands differ");
    {
        constexpr int FI = ko::FRONT_IMPROVE_TICKS, BI = ko::BACK_IMPROVE_TICKS;
        static_assert(FI < BI, "the checks below need a tighter front deadband than back");
        const auto shifted = [](int n) {
            std::vector<double> v;
            for (double x : {0.41, 0.40, 0.39}) v.push_back(kp::pycompat::py_round_nd(x + n * 0.01, 4));
            return v;
        };
        const auto at = [](int n) { return kp::pycompat::py_round_nd(0.42 + n * 0.01, 4); };
        ko::RestingBook b; seed(b, "T", "bid", 0.42, {0.41, 0.40, 0.39});
        auto sp0 = ko::plan_side("T", "bid", at(FI - 1), rungs(shifted(FI - 1)), b.state("T").bid, C3);
        check("a gain one tick short of the front deadband moves nothing", sp0.places.empty());
        auto sp = ko::plan_side("T", "bid", at(FI), rungs(shifted(FI)), b.state("T").bid, C3);
        check("back two (FI-tick gain) do NOT", sp.places.size() == 1 && sp.keeps.size() == 2);
        auto sp2 = ko::plan_side("T", "bid", at(BI), rungs(shifted(BI)), b.state("T").bid, C3);
        check("a BI-tick gain moves all three", sp2.places.size() == 3);
    }
    kp::test::section("backfill: cancel the dead front rung, top up BEHIND the survivors");
    {
        ko::RestingBook b; seed(b, "T", "bid", 0.42, {0.41, 0.40, 0.39});
        auto sp = ko::plan_side("T", "bid", 0.405, rungs({0.395, 0.385, 0.375}), b.state("T").bid, C3);
        check("the through-theo order is cancelled",
              sp.cancels.size() == 1 && *sp.cancels[0].price == 0.41
                  && sp.cancels[0].reason == "edge_negative");
        check("a third order is sent behind the worst survivor",
              sp.places.size() == 1 && *sp.places[0].price < 0.39 && sp.places[0].reason == "top_up");
        const std::vector<std::string> GR{"10.00", "20.00", "50.00"};
        ko::RestingBook bt; seed(bt, "T", "bid", 0.42, {0.41, 0.40, 0.39}, GR);
        auto st = ko::plan_side("T", "bid", 0.405, rungs({0.395, 0.385, 0.375}, GR),
                                bt.state("T").bid, GR);
        check("  so the refill is TRIMMED to the remaining 10, not its slot's 50",
              st.places.size() == 1 && st.places[0].count == "10.00");
    }
    kp::test::section("risk limits");
    {
        // A three-rung ladder at LOT_SIZE fills the per-side cap EXACTLY (3 x 10 = 30 contracts), so
        // replacing a rung in place is at the cap -- the case the netting arithmetic must survive.
        const std::vector<std::string> CAP3{"10.00", "10.00", "10.00"};
        static_assert(kp::config::MAX_RESTING_CONTRACTS_PER_MARKET_SIDE == 3 * kp::config::LOT_SIZE,
                      "CAP3 sums to the cap");
        constexpr int FI = ko::FRONT_IMPROVE_TICKS;
        std::vector<double> up;
        for (double x : {0.41, 0.40, 0.39}) up.push_back(kp::pycompat::py_round_nd(x + FI * 0.01, 4));
        ko::RestingBook b6; seed(b6, "T", "bid", 0.42, {0.41, 0.40, 0.39}, CAP3);
        auto sp = ko::plan_side("T", "bid", kp::pycompat::py_round_nd(0.42 + FI * 0.01, 4),
                                rungs(up, CAP3), b6.state("T").bid, CAP3);
        check("  an FI-tick improvement replaces the front rung",
              sp.cancels.size() == 1 && sp.places.size() == 1);
        check("  ignoring the cancels blocks it (the shipped bug)",
              !ko::within_risk_limits(b6, sp.places).first);
        auto r = ko::within_risk_limits(b6, sp.places, sp.cancels);
        check("  netting them out allows it", r.first && r.second.empty());
        std::vector<ko::Order> rich;
        for (int i = 0; i < 60; ++i) rich.push_back(mk("T" + std::to_string(i), "bid", 0.90, "50.00"));
        auto rr = ko::within_risk_limits(ko::RestingBook(), rich);
        bool names = false;
        for (const auto& v : rr.second) names |= v.find("resting value") != std::string::npos;
        check("the SAME contract count at 90c breaches it, naming the dollar limit", !rr.first && names);
        check("  offer value is (1-p)*count",
              kp::pycompat::py_round_nd(ko::order_value("ask", 0.90, "50.00"), 6) == 5.0);
    }
    kp::test::section("a BLOCKED plan must not send its replacement cancels naked");
    {
        auto safe = ko::reducing_only({mk("T", "bid", 0.41, "10.00", std::nullopt, "improve_1t"),
                                       mk("T", "bid", 0.39, "50.00", std::nullopt, "edge_negative")});
        check("  only the exposure-reducing cancel survives",
              safe.size() == 1 && safe[0].reason == "edge_negative");
    }
    kp::test::section("apply: a 429 mid-batch stops cleanly -- and is NEVER re-sent");
    {
        std::vector<ko::Order> places;
        for (int i = 0; i < 6; ++i) places.push_back(mk("T" + std::to_string(i), "bid", 0.40, "10.00"));
        ko::RestingBook bk;
        Rejecting fake; fake.fail_on = 2;
        auto r = ko::apply(places, {}, bk, &fake, true, 2, nullptr, 0.0);
        check("the first chunk landed", r.placed == 2);
        check("  the 429 is counted as its own thing", r.rate_limited == 1);
        check("  and the remaining chunk is reported unsent", r.unsent_places == 2);
        check("  the loop stopped rather than burning chunk 3 on a closed door (no double send)",
              fake.creates == 2);
        Rejecting once; once.once500 = true;
        ko::RestingBook bk2;
        auto r2 = ko::apply(places, {}, bk2, &once, true, 2, nullptr, 0.0);
        check("a non-429 batch error does not stop the remaining chunks",
              r2.placed == 4 && r2.rate_limited == 0);
    }
    kp::test::section("a place whose paired cancel did NOT land is dropped");
    {
        const std::string pid = ko::pair_id("T", "bid", 0.40);
        auto cx = mk("T", "bid", 0.40, "10.00", std::string("old-1"), "improve_1t", pid);
        auto px = mk("T", "bid", 0.41, "10.00", std::nullopt, "improved", pid);
        ko::RestingBook bk;
        Rejecting fails; fails.cancel_fails = true; fails.fail_on = 99;
        auto r = ko::apply({px}, {cx}, bk, &fails, true, 2, nullptr, 0.0);
        check("  so the replacement is NOT placed",
              r.cancelled == 0 && r.placed == 0 && r.orphaned_places == 1 && bk.size() == 0);
        ko::RestingBook bk2;
        Rejecting okk; okk.fail_on = 99;
        auto r2 = ko::apply({px}, {cx}, bk2, &okk, true, 2, nullptr, 0.0);
        check("  ... and when the cancel DOES land, the replacement goes",
              r2.cancelled == 1 && r2.placed == 1 && r2.orphaned_places == 0);
        ko::RestingBook bk3;
        ForbiddenApi forbid;
        auto r3 = ko::apply({px}, {cx}, bk3, &forbid, false, 2, nullptr, 0.0);
        check("  ... a dry run is not held to a confirmation, and never reaches the client",
              r3.placed == 1 && r3.orphaned_places == 0 && forbid.touched == 0 && r3.dry_run);
        ko::RestingBook bk4;
        auto r4 = ko::apply({px}, {cx}, bk4, nullptr, false, 2, nullptr, 0.0);
        check("  ... and the dry path runs with no client at all", r4.placed == 1);
    }
    kp::test::section("an exhausted budget leaves work UNSENT rather than pushing it into a 429");
    {
        std::vector<ko::Order> places;
        for (int i = 0; i < 6; ++i) places.push_back(mk("T" + std::to_string(i), "bid", 0.40, "10.00"));
        double t = 0.0;
        kp::kalshi::TokenBucket empty(0.0, 0.0, [&t] { return t; });
        ko::RestingBook bk;
        Rejecting fake; fake.fail_on = 99;
        auto r = ko::apply(places, {}, bk, &fake, true, 2, &empty, 0.0);
        check("nothing was sent; all 6 reported unsent, not as an error",
              r.placed == 0 && fake.creates == 0 && r.unsent_places == 6 && r.place_errors.empty()
                  && r.rate_limited == 0);
        kp::kalshi::TokenBucket b(40.0, 0.0, [&t] { return t; });
        Rejecting f2; f2.fail_on = 99; f2.bucket = &b;
        ko::RestingBook bk2;
        auto r2 = ko::apply(places, {}, bk2, &f2, true, 2, &b, 0.0);
        check("  a budget that covers two chunks sends two, and empties rather than overdraws",
              r2.placed == 4 && r2.unsent_places == 2 && b.available() == 0.0);
    }
    kp::test::section("fills reduce the local book");
    {
        ko::RestingBook bf;
        bf.state("T").bid = {mk("T", "bid", 0.40, "50.00", std::string("oid-1")),
                             mk("T", "bid", 0.39, "20.00", std::string("oid-2"))};
        auto fill = [](const char* oid, const char* t, const char* n) {
            Value f;
            f.set("order_id", Value::make_string(oid));
            if (t) f.set("market_ticker", Value::make_string(t));
            f.set("count_fp", Value::make_string(n));
            return f;
        };
        check("a partial fill decrements, as a fixed-point STRING",
              ko::apply_fill(bf, fill("oid-1", "T", "20.00"), 1.0) == 1
                  && bf.state("T").bid[0].count == "30.00");
        ko::apply_fill(bf, fill("oid-1", "T", "30.00"), 1.0);
        check("a full fill REMOVES it", bf.state("T").bid.size() == 1
                                            && *bf.state("T").bid[0].order_id == "oid-2");
        ko::apply_fill(bf, fill("oid-2", "T", "999.00"), 1.0);
        check("an over-fill removes rather than going negative", bf.state("T").bid.empty());
        check("unknown order_id is a no-op", ko::apply_fill(bf, fill("nope", "T", "1.00"), 1.0) == 0);
        ko::RestingBook b2;
        auto& st = b2.state("M");
        st.bid = {mk("M", "bid", 0.48, "20.00", std::string("b1"))};
        st.ask = {mk("M", "ask", 0.52, "20.00", std::string("a1"))};
        ko::apply_fill(b2, fill("b1", "M", "5.00"), 42.0);
        check("the bid clock is armed on a partial; the ask is untouched",
              st.last_fill("bid") == 42.0 && !st.last_fill("ask") && st.bid.size() == 1);
    }
    kp::test::section("the zombie hold: an improve waits, a cancel never does");
    {
        const double T0 = 1000.0;
        ko::RestingBook bk;
        auto& st = seed(bk, "Z", "bid", 0.1850, {0.18, 0.17, 0.16});
        auto L = rungs({0.19, 0.18, 0.17});
        auto improved = [](const std::vector<ko::Order>& cs) {
            std::vector<double> p;
            for (const auto& c : cs) if (c.reason.rfind("improve", 0) == 0) p.push_back(*c.price);
            return p;
        };
        check("cold clock: the front rung improves",
              improved(ko::plan_side("Z", "bid", 0.195, L, st.bid, C3, 1, 2, 0.18, std::nullopt,
                                     std::nullopt, T0).cancels) == std::vector<double>{0.18});
        auto held = ko::plan_side("Z", "bid", 0.195, L, st.bid, C3, 1, 2, 0.18, T0, 0.18, T0 + 1);
        check("armed, touch unmoved: the same improve is KEPT instead",
              improved(held.cancels).empty()
                  && std::any_of(held.keeps.begin(), held.keeps.end(),
                                 [](const ko::Order& o) { return o.reason == "zombie_hold"; }));
        check("the touch moved 0.18 -> 0.19 while we were out: the hold lifts",
              improved(ko::plan_side("Z", "bid", 0.195, L, st.bid, C3, 1, 2, 0.19, T0, 0.18, T0 + 1)
                           .cancels) == std::vector<double>{0.18});
        check("  a touch that moved AWAY does not lift it",
              improved(ko::plan_side("Z", "bid", 0.195, L, st.bid, C3, 1, 2, 0.17, T0, 0.18, T0 + 1)
                           .cancels).empty());
        check("after the hold the improve fires again",
              improved(ko::plan_side("Z", "bid", 0.195, L, st.bid, C3, 1, 2, 0.18, T0, 0.18,
                                     T0 + kp::config::QUOTE_ZOMBIE_HOLD_S + 0.1).cancels)
                  == std::vector<double>{0.18});
        ko::RestingBook bk2;
        seed(bk2, "A", "bid", 0.1850, {0.19, 0.18});
        bk2.state("A").set_touch(0.18, 0.22);
        auto res = ko::apply({}, {mk("A", "bid", 0.19, "20.00", std::string("o0"), "edge_negative")},
                             bk2, nullptr, false, 20, nullptr, T0);
        check("an edge_negative cancel arms the side at the touch we left",
              bk2.state("A").zombie("bid") == T0 && bk2.state("A").zombie_at_touch("bid") == 0.18
                  && res.zombies_armed == 1 && !bk2.state("A").zombie("ask"));
        ko::RestingBook bk3;
        seed(bk3, "A", "bid", 0.1850, {0.19, 0.18});
        ko::apply({}, {mk("A", "bid", 0.19, "20.00", std::string("o0"), "improve_1t", "x")}, bk3,
                  nullptr, false, 20, nullptr, T0);
        check("an improve cancel does NOT arm it", !bk3.state("A").zombie("bid"));
    }
    kp::test::section("a FAILED cancel leaves the order visible, and the next reconcile retries it");
    {
        ko::RestingBook b8;
        seed(b8, "T", "bid", 0.42, {0.41, 0.40});
        std::vector<ko::Order> cs;
        for (const auto& o : b8.state("T").bid) { auto c = o; c.reason = "edge_negative"; cs.push_back(c); }
        struct HalfFail : kp::kalshi::Api {
            Value batch_cancel_orders(const std::vector<std::string>& ids) override {
                Value a = Value::make_array({}), r0, r1;
                r0.set("order_id", Value::make_string(ids[0])); r0.set("reduced_by", Value::make_int(0));
                r1.set("order_id", Value::make_string(ids[1])); r1.set("reduced_by", Value::make_int(20));
                a.push(r0); a.push(r1);
                return a;
            }
            Value batch_create_orders(const std::vector<Value>&) override { return Value::make_array({}); }
        } hf;
        auto r8 = ko::apply({}, cs, b8, &hf, true, 20, nullptr, 0.0);
        check("  one cancel confirmed, one not", r8.cancelled == 1 && r8.cancel_errors.size() == 1);
        check("  the FAILED order is still in the book",
              b8.state("T").bid.size() == 1 && *b8.state("T").bid[0].price == 0.41);
        auto retry = ko::reconcile(b8, {}, nullptr, nullptr, 0.0);
        check("    so the next reconcile plans another cancel for it",
              retry.to_cancel.size() == 1 && retry.to_cancel[0].reason == "no_longer_quoting");
    }
    kp::test::section("apply_fill's MarketState is created for the frame's ticker, as the reference does");
    {
        ko::RestingBook b;
        Value f;
        f.set("order_id", Value::make_string("x"));
        f.set("market_ticker", Value::make_string("NEW"));
        f.set("count_fp", Value::make_string("1.00"));
        ko::apply_fill(b, f, 0.0);
        check("tickers() now lists it", b.tickers() == std::vector<std::string>{"NEW"});
    }
}

}  // namespace
