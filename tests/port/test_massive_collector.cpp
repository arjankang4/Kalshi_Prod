// massive/client.QuoteCollector against the reference, driven op by op through identical scripted
// sockets and a scripted clock: the frame handling (sequence rule, status log bound, the frames the
// reference's pump dies on), health and quote age, recenter's diffing and its rate limit on the
// exact boundary, subscribe/unsubscribe eviction, start's max_connections retry and reconnect's
// cache clear -- plus what went on the WIRE. Each op's result is compared as json.dumps text.
// A threaded section then runs the real pump thread against a fake socket (not golden).
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "kp/massive/client.hpp"
#include "kp/time/et.hpp"
#include "massive_fakes.hpp"
#include "vecio.hpp"

using namespace kp::test::vec;
using namespace kp::test::massive;
namespace km = kp::massive;

namespace {

// --- QuoteCollector ---------------------------------------------------------------------------

Value col_op(km::client::QuoteCollector& col, FakeClock& clk, FakeWsPool& pool, const Value& st) {
    const std::string op = st.find("op")->str();
    auto get = [&](const char* k) { return st.find(k); };
    if (op == "clock") {
        clk.mono = num(*get("t"));
        clk.mono_step = get("step") ? num(*get("step")) : 0.0;
        return Value();
    }
    if (op == "start") {
        Value r = Value::make_object({});
        try {
            col.start((int)get("attempts")->integer(), num(*get("backoff")));
            r.set("raised", Value());
            r.set("msg", Value());
        } catch (const std::exception& e) {
            r.set("raised", S(label(e)));
            r.set("msg", dynamic_cast<const km::MassiveError*>(&e) ? S(e.what()) : Value());
        }
        return r;
    }
    if (op == "pump") {
        col.clear_stop();
        const bool died = col.run_pump();
        Value r = Value::make_object({});
        r.set("died", B(died));
        return r;
    }
    if (op == "state") {
        Value r = Value::make_object({});
        r.set("frame_count", I(col.frame_count()));
        r.set("msg_count", I(col.msg_count()));
        r.set("n_latest", I((long long)col.health(0.0).n_symbols));
        r.set("status_log", I((long long)col.status_log_size()));
        r.set("status_dropped", I(col.status_dropped()));
        r.set("last_msg_t", N(col.last_msg_t()));
        const auto pe = col.pump_error();
        r.set("pump_error", B(pe.has_value()));
        r.set("reconnect_failed", B(pe && pe->rfind("reconnect failed: ", 0) == 0));
        r.set("alive", B(col.alive()));
        r.set("reconnects", I(col.reconnects()));
        r.set("recenter_count", I(col.recenter_count()));
        r.set("tickers", strs(col.tickers()));
        const auto sub = col.subscribed();
        r.set("subscribed", strs(std::vector<std::string>(sub.begin(), sub.end())));
        return r;
    }
    if (op == "snapshot") {
        const auto snap = col.snapshot();
        std::map<std::string, km::OptionQuote> sorted(snap.begin(), snap.end());
        Value r = Value::make_object({});
        for (const auto& [k, q] : sorted)
            r.set(k, arr({N(q.q.bp), N(q.q.ap), N(q.q.bs), N(q.q.as), q.t_ms ? I(*q.t_ms) : Value()}));
        return r;
    }
    if (op == "health") {
        const Value* now = get("now");
        const auto h = col.health(now && !now->is_null() ? std::optional<double>(num(*now)) : std::nullopt);
        Value r = Value::make_object({});
        r.set("alive", B(h.alive));
        r.set("pump_error", B(h.pump_error.has_value()));
        r.set("quote_age_s", N(h.quote_age_s));
        r.set("n_symbols", I((long long)h.n_symbols));
        r.set("reconnects", I(h.reconnects));
        r.set("frames", I(h.frames));
        return r;
    }
    if (op == "recenter" || op == "subscribe" || op == "unsubscribe") {
        Value r = Value::make_object({});
        try {
            if (op == "recenter") {
                const Value* f = get("force");
                const auto rc = col.recenter(strv(*get("want")), f && f->boolean());
                r.set("ret", rc ? arr({I((long long)rc->first), I((long long)rc->second)}) : Value());
            } else {
                const auto t = strv(*get("tickers"));
                r.set("ret", I((long long)(op == "subscribe" ? col.subscribe(t) : col.unsubscribe(t))));
            }
            r.set("raised", Value());
            r.set("msg", Value());
        } catch (const km::MassiveError& e) {
            r.set("ret", Value());
            r.set("raised", S("MassiveError"));
            r.set("msg", S(e.what()));
        } catch (const std::exception&) {
            r.set("ret", Value());
            r.set("raised", S("error"));
            r.set("msg", Value());
        }
        return r;
    }
    if (op == "reconnect") {
        const bool okc = col.reconnect((int)get("attempts")->integer(), num(*get("backoff")));
        const auto pe = col.pump_error();
        Value r = Value::make_object({});
        r.set("ok", B(okc));
        r.set("pump_error", B(pe.has_value()));
        r.set("reconnect_failed", B(pe && pe->rfind("reconnect failed: ", 0) == 0));
        return r;
    }
    if (op == "wait") {
        Value r = Value::make_object({});
        r.set("ret", B(col.wait_for_coverage((std::size_t)get("min")->integer(), num(*get("timeout")),
                                             num(*get("poll")))));
        return r;
    }
    if (op == "stop") {
        col.stop();
        return Value();
    }
    if (op == "sends") return norm_sends(pool, get("raw") && get("raw")->boolean());
    if (op == "sleeps") return take_sleeps(clk);
    throw std::logic_error("unknown op " + op);
}

void run_col(Tally& T, const Value& sc) {
    FakeClock clk;
    FakeWsPool pool;
    pool.conns.assign(sc.find("conns")->arr().begin(), sc.find("conns")->arr().end());
    km::client::QuoteCollector col(strv(*sc.find("tickers")), "TESTKEY", pool.factory(), clk.clock(), "",
                                   "wss://socket.massive.com/options", false);
    pool.on_yield = [&] { col.request_stop(); };
    const std::string name = sc.find("name")->str();
    int i = 0;
    for (const auto& st : sc.find("steps")->arr()) {
        const std::string op = st.find("op")->str();
        Value got;
        try {
            got = col_op(col, clk, pool, st);
        } catch (const std::exception& e) {
            got = S(std::string("C++ THREW: ") + e.what());
        }
        compare(T, "col." + op, "col " + name + " step " + std::to_string(i++) + " " + op, got,
                *st.find("expect"));
    }
}

// --- the REAL pump thread (not golden: the generator replaces the reference's thread) -----------
//
// What the golden replay cannot see: that the spawned pump actually drains the socket, that a
// re-centre SENT from the loop thread while the pump reads does not wedge either side, that a
// dead socket shows as alive=false with a pump_error, that reconnect() rebuilds it, and that
// stop() joins promptly.

Value frame(const std::string& text) {
    Value f = Value::make_object({});
    f.set("f", S(text));
    return f;
}

Value script(int n_syms, int n_timeouts, bool end_err) {
    Value s = Value::make_array({});
    s.push(frame(R"([{"ev":"status","status":"connected"}])"));
    s.push(frame(R"([{"ev":"status","status":"auth_success"}])"));
    for (int i = 0; i < n_syms; ++i)
        s.push(frame("[{\"ev\":\"Q\",\"sym\":\"O:T" + std::to_string(i) +
                     "\",\"bp\":1.5,\"ap\":1.6,\"bs\":3,\"as\":4,\"q\":" + std::to_string(i) + ",\"t\":17}]"));
    for (int i = 0; i < n_timeouts; ++i) s.push(S("timeout"));
    if (end_err) s.push(S("err"));
    return s;
}

template <class F>
bool eventually(F f, double seconds) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < end) {
        if (f()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return f();
}

void threaded_checks() {
    using kp::test::check;
    kp::test::section("QuoteCollector, threaded");
    FakeWsPool pool;
    pool.timeout_sleep_ms = 1;
    pool.conns.push_back(script(20, 400, true));
    pool.conns.push_back(script(5, 5000, false));
    std::vector<std::string> tk;
    for (int i = 0; i < 20; ++i) tk.push_back("O:T" + std::to_string(i));
    km::client::QuoteCollector col(tk, "TESTKEY", pool.factory());   // real clock, threaded
    col.start();
    check("pump drains the socket", col.wait_for_coverage(20, 5.0, 0.01));
    check("snapshot sees all 20", col.snapshot().size() == 20);
    check("alive while reading", col.health().alive);
    const auto rc = col.recenter({"O:T0", "O:T1", "O:NEW"}, true);
    check("recenter from the loop thread while the pump reads", rc && rc->first == 1 && rc->second == 18);
    check("evicted on unsubscribe", col.snapshot().size() == 2);
    check("a dead socket shows as alive=false", eventually([&] { return !col.health().alive; }, 5.0));
    check("... with a pump_error", col.health().pump_error.has_value());
    check("reconnect rebuilds it", col.reconnect(1, 0.0) && col.health().alive && col.reconnects() == 1);
    check("the reconnect resubscribed the CURRENT set",
          !pool.sends.empty() && pool.sends.back().second.find("Q.O:NEW") != std::string::npos);
    check("fresh quotes after reconnect", col.wait_for_coverage(2, 5.0, 0.01));
    const auto t0 = std::chrono::steady_clock::now();
    col.stop();
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    check("stop joins promptly", took < 1.0, std::to_string(took) + "s");
    check("not alive after stop", !col.alive());
}

}  // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "tests/data/golden";
    kp::time::init();
    Tally T("massive_collector");
    T.expect({"col.start", "col.pump", "col.state", "col.snapshot", "col.health", "col.recenter",
              "col.subscribe", "col.unsubscribe", "col.reconnect", "col.wait", "col.sends",
              "col.sleeps"});
    for (const auto& l : read(dir, "massive_client"))
        if (l.rfind("col ", 0) == 0) run_col(T, kp::json::parse(unhex(words(l)[1])));
    threaded_checks();
    return T.report();
}
