// strategy/budget against the reference: ladder_value, the moneyness floors, build_items' priority
// order, admit, and select_markets -- then the invariants strategy/test_budget.py pins, directly.
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

#include "kp/strategy/budget.hpp"
#include "order_fixtures.hpp"
#include "vecio.hpp"

using namespace kp::test::vec;
using namespace kp::test::ofx;
namespace kb = kp::strategy::budget;
namespace ko = kp::strategy::orders;
using kp::json::Value;

namespace {

bool debug_diff(const std::string& tag, const std::string& got, const std::string& want) {
    static std::set<std::string> shown;
    if (got != want && std::getenv("KP_DEBUG") && shown.insert(tag).second)
        std::fprintf(stderr, "[%s]\n  got:  %s\n  want: %s\n", tag.c_str(), got.c_str(), want.c_str());
    return got == want;
}

std::vector<std::string> strs(const Value& a) {
    std::vector<std::string> out;
    for (const auto& x : a.arr()) out.push_back(x.str());
    return out;
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

kb::Theos theos_from(const Value& obj) {
    kb::Theos t;
    for (const auto& [k, v] : obj.obj()) t[k] = onum(v);
    return t;
}

void unit_checks();

}  // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "tests/data/golden";
    Tally T("budget");
    T.expect({"lv", "ms", "adm", "items", "sel"});
    for (const auto& line : read(dir, "budget")) {
        const auto w = words(line);
        const std::string& fn = w[0];
        try {
            if (fn == "lv") {
                const Value cn = jhex(w[2]), sd = jhex(w[3]);
                T.hit(fn, same(kb::ladder_value(hx(w[1]), strs(cn), strs(sd)), w[4]), line);
            } else if (fn == "ms") {
                const auto r = kb::moneyness_skip(w[1] == "-" ? "" : w[1], ohx(w[2]));
                T.hit(fn, js(r ? Value::make_string(*r) : Value()) == unhex(w[3]), line);
            } else if (fn == "adm") {
                const Value in = jhex(w[1]);
                const auto place = orders_from(*in.find("place"));
                const auto cancel = orders_from(*in.find("cancel"));
                kb::Theos theos;
                kb::EdgeViews em;
                kb::SideSet resting;
                const bool has_t = !in.find("theos")->is_null(), has_e = !in.find("em")->is_null();
                const bool has_r = !in.find("resting")->is_null();
                if (has_t) theos = theos_from(*in.find("theos"));
                if (has_e) em = em_from(*in.find("em"));
                if (has_r)
                    for (const auto& p : in.find("resting")->arr())
                        resting.insert({p.arr()[0].str(), p.arr()[1].str()});
                const auto a = kb::admit(place, cancel, in.find("tokens")->num(),
                                         has_t ? &theos : nullptr, has_e ? &em : nullptr,
                                         onum(in.find("reserve")), has_r ? &resting : nullptr);
                Value got = Value::make_array({});
                got.push(orders_json(a.place));
                got.push(orders_json(a.cancel));
                got.push(a.detail.to_json());
                T.hit(fn, debug_diff(fn, js(got), unhex(w[2])), line);

                auto items = kb::build_items(place, cancel, has_t ? &theos : nullptr,
                                             has_e ? &em : nullptr, has_r ? &resting : nullptr);
                kb::sort_items(items);
                const Value want_items = jhex(w[3]);
                Value recs = Value::make_array({});
                for (std::size_t i = 0; i < items.size() && i < want_items.arr().size(); ++i)
                    recs.push(items[i].record());
                const bool full = want_items.arr().size() == items.size()
                                  || want_items.arr().size() == 50;   // the 532-board is truncated
                T.hit("items", full && debug_diff("items", js(recs), unhex(w[3])), line);
            } else if (fn == "sel") {
                const Value in = jhex(w[1]);
                std::vector<kp::pricing::theo::Market> markets;
                for (const auto& m : in.find("markets")->arr()) {
                    kp::pricing::theo::Market mk;
                    mk.ticker = m.find("ticker")->str();
                    const Value* st = m.find("strike_type");
                    if (st && !st->is_null()) mk.strike_type = st->str();
                    markets.push_back(mk);
                }
                const kb::Theos theos = theos_from(*in.find("theos"));
                const kb::EdgeViews em = em_from(*in.find("em"));
                const std::string mode = in.find("mode")->str();
                std::map<std::string, std::vector<std::string>> cmap;
                std::vector<std::string> ctup;
                if (mode == "map")
                    for (const auto& [k, v] : in.find("counts")->obj()) cmap[k] = strs(v);
                if (mode == "tuple") ctup = strs(*in.find("counts"));
                std::set<std::string> inc;
                for (const auto& s : in.find("incumbents")->arr()) inc.insert(s.str());
                const Value& kw = *in.find("kw");
                const auto sel = kb::select_markets(
                    markets, &theos, &em, mode == "map" ? &cmap : nullptr,
                    mode == "tuple" ? &ctup : nullptr, inc, onum(kw.find("max_value")),
                    onum(kw.find("max_contracts")), onum(kw.find("hysteresis")),
                    onum(kw.find("band_fraction")));
                Value chosen = Value::make_array({});
                for (const auto& t : sel.chosen) chosen.push(Value::make_string(t));
                Value got = Value::make_array({});
                got.push(chosen);
                got.push(sel.detail.to_json());
                T.hit(fn, debug_diff(fn, js(got), unhex(w[2])), line);
            } else {
                T.hit("unknown_tag:" + fn, false, line);
            }
        } catch (const std::exception& e) {
            T.hit(fn + "_threw", false, std::string(e.what()) + " @ " + line.substr(0, 200));
        }
    }
    unit_checks();
    return T.report();
}

namespace {

using kp::test::check;

ko::Order cancel(const std::string& t, const std::string& side = "bid", double price = 0.50,
                 const std::string& count = "10.00", const std::string& reason = "edge_negative",
                 const std::string& pair = "") {
    ko::Order o;
    o.ticker = t; o.side = side; o.price = price; o.count = count; o.reason = reason;
    o.order_id = "oid-" + t; o.pair = pair;
    return o;
}
ko::Order place(const std::string& t, const std::string& side = "bid", double price = 0.50,
                const std::string& reason = "top_up", const std::string& pair = "") {
    ko::Order o;
    o.ticker = t; o.side = side; o.price = price; o.count = "10.00"; o.reason = reason; o.pair = pair;
    return o;
}
std::pair<ko::Order, ko::Order> improve_pair(const std::string& t) {
    const std::string pid = ko::pair_id(t, "bid", 0.50);
    return {cancel(t, "bid", 0.50, "10.00", "improve_1t", pid), place(t, "bid", 0.51, "improved", pid)};
}

void unit_checks() {
    kp::test::section("INVARIANT 1 -- a reducing cancel is never deferred, even at zero budget");
    {
        std::vector<ko::Order> cs;
        for (int i = 0; i < 20; ++i) cs.push_back(cancel("M" + std::to_string(i)));
        auto a = kb::admit({}, cs, 0.0, nullptr, nullptr, 0.0);
        check("all 20 negative-edge cancels admitted on an empty bucket",
              a.cancel.size() == 20 && a.place.empty() && a.detail.n_deferred == 0);
        check("  and it says so: the bucket went into debt",
              a.detail.reduce_over_budget && a.detail.tokens_left < 0);
    }
    kp::test::section("INVARIANT 2 -- a replacement cancel is never sent without its place");
    {
        auto [cx, px] = improve_pair("M1");
        auto a = kb::admit({px}, {cx}, 12.0, nullptr, nullptr, 0.0);
        check("at exactly the pair's cost, both go", a.cancel.size() == 1 && a.place.size() == 1);
        auto b = kb::admit({px}, {cx}, 11.0, nullptr, nullptr, 0.0);
        check("one token short, NEITHER goes, deferred whole",
              b.cancel.empty() && b.place.empty() && b.detail.n_deferred == 1
                  && b.detail.deferred_cost == 12);
        check("bare improve_* cancel is not admitted",
              kb::admit({}, {cx}, 1e4, nullptr, nullptr, 0.0).cancel.empty());
        check("bare 'improved' place is not admitted",
              kb::admit({px}, {}, 1e4, nullptr, nullptr, 0.0).place.empty());
    }
    kp::test::section("PRIORITY -- reduce, then improve, then cold, then top-up");
    {
        auto [cx, px] = improve_pair("M9");
        auto items = kb::build_items({px, place("M8")}, {cx, cancel("M7")});
        kb::sort_items(items);
        check("order of classes", items.size() == 3 && items[0].cls == kb::REDUCE
                                      && items[1].cls == kb::IMPROVE && items[2].cls == kb::TOPUP);
        kb::SideSet resting{{"WARM", "bid"}};
        auto a = kb::admit({place("WARM"), place("COLD")}, {}, 10.0, nullptr, nullptr, 0.0, &resting);
        check("the unquoted side takes the only ten tokens",
              a.place.size() == 1 && a.place[0].ticker == "COLD");
        kb::SideSet empty;
        auto it2 = kb::build_items({place("WARM"), place("COLD")}, {}, nullptr, nullptr, &empty);
        check("an EMPTY resting set means every side really is cold",
              it2[0].cls == kb::COLD && it2[1].cls == kb::COLD);
    }
    kp::test::section("URGENCY ranks on risk_u, falling back to R");
    {
        auto [cn, pn] = improve_pair("KXNASDAQ100U-X");
        auto [cs, ps] = improve_pair("KXINXU-X");
        kb::EdgeViews em{{"KXNASDAQ100U-X", {0.99, 0.030, std::nullopt}},
                         {"KXINXU-X", {0.10, 0.045, std::nullopt}}};
        auto items = kb::build_items({pn, ps}, {cn, cs}, nullptr, &em);
        kb::sort_items(items);
        check("the higher PERCENTILE goes first, not the higher R",
              items[0].ticker == "KXNASDAQ100U-X");
        kb::EdgeViews er{{"KXNASDAQ100U-X", {std::nullopt, 0.030, std::nullopt}},
                         {"KXINXU-X", {std::nullopt, 0.045, std::nullopt}}};
        auto it2 = kb::build_items({pn, ps}, {cn, cs}, nullptr, &er);
        kb::sort_items(it2);
        check("  falls back to R when there is no percentile", it2[0].ticker == "KXINXU-X");
    }
    kp::test::section("THE RESERVE holds tokens back from improves, not from reduces");
    {
        auto [cx, px] = improve_pair("M1");
        check("12-token pair does not fit above a 95 reserve of 100",
              kb::admit({px}, {cx}, 100.0, nullptr, nullptr, 95.0).place.empty());
        check("  it fits at a reserve of 88",
              kb::admit({px}, {cx}, 100.0, nullptr, nullptr, 88.0).place.size() == 1);
        check("a reducing cancel ignores the reserve entirely",
              kb::admit({}, {cancel("M1")}, 100.0, nullptr, nullptr, 100.0).cancel.size() == 1);
    }
    kp::test::section("CAPITAL SELECTION");
    {
        const std::vector<std::string> BAND{"20.00", "20.00", "20.00"}, WING{"20.00"};
        for (double fair : {0.03, 0.25, 0.50, 0.75, 0.97})
            check("a ladder costs its FACE at any fair",
                  std::fabs(kb::ladder_value(fair, BAND) - 60.0) < 1e-9
                      && std::fabs(kb::ladder_value(fair, WING) - 20.0) < 1e-9);
        std::vector<kp::pricing::theo::Market> ms;
        kb::Theos th;
        kb::EdgeViews em;
        for (int i = 0; i < 532; ++i) {
            char t[8];
            std::snprintf(t, sizeof t, "M%03d", i);
            kp::pricing::theo::Market m;
            m.ticker = t; m.strike_type = "between";
            ms.push_back(m);
            th[t] = 0.50;
            em[t] = {std::nullopt, 1.0 - i * 0.001, true};
        }
        auto s = kb::select_markets(ms, &th, &em, nullptr, &BAND);
        // MAX_RESTING_ORDER_VALUE / $60 a ladder = 2000 / 60 -> 33 markets, $1,980.
        check("  33 chosen at $60 each, no capital idle, the rest reported",
              s.chosen.size() == 33 && s.detail.value == 1980.0 && s.detail.unaffordable == 499);
        check("a 4pm bracket at 2c is skipped; at 4c it is not",
              kb::moneyness_skip("between", 0.02).has_value()
                  && !kb::moneyness_skip("between", 0.04).has_value());
        check("no fair is a skip", kb::moneyness_skip("between", std::nullopt) == std::string("no_fair"));
    }
}

}  // namespace
