// The Massive REST helpers against the reference, replayed through canned HTTP: client.rest /
// option_contracts, universe.option_root_and_strikes, history.get_url's retry ladder and the
// /v2/aggs pager, pm_settled_root, index.snapshot / closes_ms, and futures._get / aggs bisection /
// the window guard / session_volume.
//
// Each case compares, as json.dumps text: the result, the exception class (and message where the
// reference's is deterministic), EVERY URL requested in order, every sleep (rate limiter and
// retry backoff, on one scripted clock shared across cases exactly as the module-level limiters
// are shared in Python), and how many canned responses went unread.
#include <algorithm>
#include <string>
#include <tuple>
#include <vector>

#include "kp/massive/contracts.hpp"
#include "kp/massive/futures_net.hpp"
#include "kp/massive/history.hpp"
#include "kp/massive/index_rest.hpp"
#include "kp/time/et.hpp"
#include "massive_fakes.hpp"
#include "vecio.hpp"

using namespace kp::test::vec;
using namespace kp::test::massive;
namespace km = kp::massive;

namespace {

Value arr_of(const std::vector<Value>& v) {
    Value a = Value::make_array({});
    for (const auto& x : v) a.push(x);
    return a;
}

std::optional<std::string> ostr_of(const Value& v) {
    return v.is_null() ? std::nullopt : std::optional<std::string>(v.str());
}

km::Params params_of(const Value& o) {
    km::Params p;
    for (const auto& [k, v] : o.obj()) {
        if (v.is_null()) p.emplace_back(k, std::nullopt);
        else if (v.is_string()) p.emplace_back(k, v.str());
        else p.emplace_back(k, std::to_string(v.integer()));
    }
    return p;
}

Value opt_num(const std::optional<double>& d) { return d ? Value::make_number(*d) : Value(); }
Value opt_s(const std::optional<std::string>& s) { return s ? Value::make_string(*s) : Value(); }

Value run(const km::Rest& rest, const std::string& fn, const Value& a) {
    const auto& A = a.arr();
    if (fn == "option_contracts")
        return arr_of(km::client::option_contracts(rest, A[0].str(), ostr_of(A[1])));
    if (fn == "root_strikes") {
        const auto rs = km::client::option_root_and_strikes(rest, A[0].str(), A[1].str());
        Value ks = Value::make_array({});
        for (double k : rs.strikes) ks.push(Value::make_number(k));
        return arr_of({opt_s(rs.root), ks, arr_of(rs.contracts)});
    }
    if (fn == "rest") return rest.rest(A[0].str(), params_of(A[1]));
    if (fn == "get_url") return rest.get_url(A[0].str());
    if (fn == "hist_get") return rest.get(A[0].str(), params_of(A[1]));
    if (fn == "index_snapshot") {
        std::vector<std::string> us;
        for (const auto& u : A[0].arr()) us.push_back(u.str());
        Value o = Value::make_object({});
        for (const auto& [u, q] : km::index::snapshot(rest, us)) {
            o.set(u, arr_of({opt_num(q.value), q.ts_ns ? Value::make_int(*q.ts_ns) : Value(),
                             Value::make_string(q.ticker), opt_s(q.market_status), opt_s(q.name)}));
        }
        return o;
    }
    if (fn == "closes_ms") {
        Value o = Value::make_array({});
        for (const auto& [t, c] : km::index::closes_ms(rest, A[0].str(), A[1].str()))
            o.push(arr_of({Value::make_int(t), Value::make_number(c)}));
        return o;
    }
    if (fn == "second_bars") return arr_of(km::history::second_bars(rest, A[0].str(), A[1].str(), A[2].str()));
    if (fn == "fut_get") return km::futures::get(rest, A[0].str(), params_of(A[1]));
    if (fn == "quote_page") return km::futures::quote_page(rest, A[0].str());
    if (fn == "aggs")
        return arr_of(km::futures::aggs(rest, A[0].str(), A[1].str(), A[2].integer(), A[3].integer(),
                                        (long)A[4].integer()));
    if (fn == "session_volume") {
        const std::string& d = A[1].str();
        return Value::make_number(km::futures::session_volume(rest, A[0].str(), std::stoi(d.substr(0, 4)),
                                                              std::stoi(d.substr(5, 2)), std::stoi(d.substr(8, 2))));
    }
    if (fn == "pm_settled_root") return opt_s(km::history::pm_settled_root(A[0].arr()));
    throw std::logic_error("unknown fn " + fn);
}

const char* rest_label(const std::exception& e) {
    if (dynamic_cast<const km::MassiveError*>(&e)) return "MassiveError";
    if (dynamic_cast<const km::MassiveHistoryError*>(&e)) return "MassiveHistoryError";
    if (dynamic_cast<const std::invalid_argument*>(&e)) return "ValueError";
    if (dynamic_cast<const std::out_of_range*>(&e)) return "KeyError";
    return "error";
}

}  // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "tests/data/golden";
    kp::time::init();
    Tally T("massive_rest");
    T.expect({"option_contracts", "root_strikes", "rest", "get_url", "hist_get", "index_snapshot",
              "closes_ms", "second_bars", "fut_get", "quote_page", "aggs", "session_volume",
              "pm_settled_root"});

    FakeClock clk;
    FakeHttp http;
    km::Rest rest("TESTKEY", http.getter(), clk.clock(), "https://api.massive.com");

    // Suite order matters: the limiters are shared across every case, as the reference's
    // module-level ones are across one generator run (client, history, index, futures, universe
    // cases were generated in the order gen_rest emits them).
    std::vector<std::tuple<long long, std::string, Value>> cases;
    for (const char* s : {"massive_client", "massive_universe", "massive_history", "massive_index",
                          "massive_futures"})
        for (const auto& l : read(dir, s))
            if (l.rfind("rest ", 0) == 0) {
                Value c = kp::json::parse(unhex(words(l)[1]));
                cases.emplace_back(c.find("seq")->integer(), s, std::move(c));
            }
    std::stable_sort(cases.begin(), cases.end(),
                     [](const auto& a, const auto& b) { return std::get<0>(a) < std::get<0>(b); });
    for (const auto& [seq, suite, c] : cases) {
        const std::string fn = c.find("fn")->str();
        const Value& ex = *c.find("expect");
        http.resps.assign(c.find("resps")->arr().begin(), c.find("resps")->arr().end());
        http.urls.clear();
        clk.sleeps.clear();

        Value got = Value::make_object({});
        Value result, raised, msg;
        try {
            result = run(rest, fn, *c.find("args"));
        } catch (const std::exception& e) {
            raised = Value::make_string(rest_label(e));
            if (!ex.find("msg")->is_null()) msg = Value::make_string(e.what());
        }
        got.set("result", result);
        got.set("raised", raised);
        got.set("msg", msg);
        Value urls = Value::make_array({});
        for (const auto& u : http.urls) urls.push(Value::make_string(u));
        got.set("urls", urls);
        Value sl = Value::make_array({});
        for (double s : clk.sleeps) sl.push(Value::make_number(s));
        got.set("sleeps", sl);
        got.set("left", Value::make_int((long long)http.resps.size()));

        const std::string g = kp::json::dumps(got), want = kp::json::dumps(ex);
        const bool ok = g == want;
        if (!ok) {
            std::size_t i = 0;
            while (i < g.size() && i < want.size() && g[i] == want[i]) ++i;
            const std::size_t from = i > 80 ? i - 80 : 0;
            std::printf("  [%s %s] diverges at %zu:\n    got  ...%s\n    want ...%s\n", suite.c_str(), fn.c_str(),
                        i, g.substr(from, 200).c_str(), want.substr(from, 200).c_str());
        }
        T.hit(fn, ok, fn + " " + suite);
    }
    return T.report();
}
