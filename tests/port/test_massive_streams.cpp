// massive/streams FuturesFeed / IndexFeed against the reference, op by op through identical scripted
// sockets and clock: mid-change notification, (mid, age) on the vendor stamp and its bounds,
// move_scale BIT-EXACT (3.12 compensated sum, libm pow for the squares -- see pow_pin), the tee,
// status/handle errors, reconnect with backoff and resubscription. A threaded section then runs
// the real pump thread against a fake socket (not golden).
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "kp/massive/streams.hpp"
#include "kp/time/et.hpp"
#include "massive_fakes.hpp"
#include "vecio.hpp"

using namespace kp::test::vec;
using namespace kp::test::massive;
namespace km = kp::massive;

namespace {

// --- FuturesFeed / IndexFeed ------------------------------------------------------------------

Value norm_err(const std::optional<std::string>& le) {
    if (!le) return Value();
    if (le->rfind("status: ", 0) == 0) return S(*le);
    const auto a = le->find(": ");
    if (le->rfind("handle: ", 0) == 0) {
        const auto b = le->find(": ", a + 2);
        return S(le->substr(0, b));
    }
    return S(le->substr(0, a));
}

struct TeeLog {
    std::vector<std::pair<long long, std::string>> rows;
};

Value stream_op(km::streams::Stream& feed, km::streams::FuturesFeed* ff, km::streams::IndexFeed* xf,
                FakeClock& clk, FakeWsPool& pool, TeeLog& tee, const Value& st) {
    const std::string op = st.find("op")->str();
    auto get = [&](const char* k) -> const Value* {
        const Value* v = st.find(k);
        return v && !v->is_null() ? v : nullptr;
    };
    if (op == "clock") {
        clk.mono = num(*get("t"));
        clk.mono_step = get("step") ? num(*get("step")) : 0.0;
        return Value();
    }
    if (op == "ns") {
        clk.ns = get("t")->integer();
        clk.ns_step = get("step") ? get("step")->integer() : 0;
        return Value();
    }
    if (op == "start") {
        feed.start();
        Value r = Value::make_object({});
        r.set("ok", B(feed.ok()));
        r.set("errors", I(feed.health(1.0).errors));
        r.set("last_error", norm_err(feed.last_error()));
        return r;
    }
    if (op == "pump") {
        feed.clear_stop();
        Value r = Value::make_object({});
        r.set("died", B(feed.run_pump()));
        return r;
    }
    if (op == "state") {
        const auto h = feed.health(get("now") ? std::optional<double>(num(*get("now"))) : std::nullopt);
        Value r = Value::make_object({});
        r.set("name", S(h.name));
        r.set("ok", B(h.ok));
        r.set("frames", I(h.frames));
        r.set("updates", I(h.updates));
        r.set("reconnects", I(h.reconnects));
        r.set("errors", I(h.errors));
        r.set("last_error", norm_err(h.last_error));
        r.set("silent_s", N(h.silent_s));
        r.set("subs", strs(feed.subs()));
        if (ff) {
            Value c = Value::make_object({});
            for (const auto& [u, t] : ff->contract()) c.set(u, S(t));
            r.set("contract", c);
            Value t = Value::make_object({});
            for (const auto& u : ff->underlyings()) t.set(u, I(ff->ticks(u)));
            r.set("ticks", t);
            std::vector<std::string> re;
            for (const auto& [u, _] : ff->resolve_error()) re.push_back(u);
            r.set("resolve_error", strs(re));
        }
        return r;
    }
    if (op == "mid") {
        const auto m = ff->mid(get("u")->str(),
                               get("max_age") ? std::optional<double>(num(*get("max_age"))) : std::nullopt);
        return arr({N(m.mid), N(m.age_s)});
    }
    if (op == "move_scale")
        return N(ff->move_scale(get("u")->str(), num(*get("h")), num(*get("sig")),
                                (std::size_t)get("min")->integer()));
    if (op == "snapshot") {
        Value r = Value::make_object({});
        for (const auto& [u, q] : xf->snapshot(get("us") ? strv(*get("us")) : std::vector<std::string>{}))
            r.set(u, arr({N(q.value), q.ts_ns ? I(*q.ts_ns) : Value(), S(q.ticker), OS(q.market_status),
                          S(q.underlying)}));
        return r;
    }
    if (op == "tee") {
        Value r = Value::make_array({});
        for (const auto& [ns, m] : tee.rows) r.push(arr({I(ns), S(m)}));
        tee.rows.clear();
        return r;
    }
    if (op == "sends") return norm_sends(pool, true);
    if (op == "sleeps") return take_sleeps(clk);
    if (op == "stop") {
        feed.stop();
        return Value();
    }
    throw std::logic_error("unknown op " + op);
}

void run_stream(Tally& T, const Value& sc) {
    FakeClock clk;
    FakeWsPool pool;
    TeeLog tee;
    pool.conns.assign(sc.find("conns")->arr().begin(), sc.find("conns")->arr().end());
    const Value* tv = sc.find("tee");
    km::streams::Tee tee_fn;
    if (tv && tv->boolean())
        tee_fn = [&tee](long long ns, const Value& m) { tee.rows.emplace_back(ns, kp::json::dumps(m)); };
    std::unique_ptr<km::streams::Stream> feed;
    km::streams::FuturesFeed* ff = nullptr;
    km::streams::IndexFeed* xf = nullptr;
    const auto us = strv(*sc.find("underlyings"));
    if (sc.find("kind")->str() == "futures") {
        std::map<std::string, std::string> contracts;
        if (const Value* c = sc.find("contracts"); c && c->is_object())
            for (const auto& [u, t] : c->obj()) contracts[u] = t.str();
        Value resolve = sc.find("resolve") ? *sc.find("resolve") : Value::make_object({});
        auto resolver = [resolve](const std::string& u, const std::string&) -> std::optional<std::string> {
            const Value* r = resolve.is_object() ? resolve.find(u) : nullptr;
            if (!r || r->is_null()) return std::nullopt;
            if (r->str() == "raise") throw std::runtime_error("no volume");
            return r->str();
        };
        auto f = std::make_unique<km::streams::FuturesFeed>(us, "TESTKEY", pool.factory(), contracts, resolver,
                                                            "2026-08-21", tee_fn, clk.clock(),
                                                            "wss://socket.massive.com/futures", false);
        ff = f.get();
        feed = std::move(f);
    } else {
        auto f = std::make_unique<km::streams::IndexFeed>(us, "TESTKEY", pool.factory(), tee_fn, clk.clock(),
                                                          "wss://socket.massive.com/indices", false);
        xf = f.get();
        feed = std::move(f);
    }
    km::streams::Stream* fp = feed.get();
    pool.on_yield = [fp] { fp->request_stop(); };
    pool.on_exhausted = [fp] { fp->request_stop(); };
    const std::string name = sc.find("name")->str();
    int i = 0;
    for (const auto& st : sc.find("steps")->arr()) {
        const std::string op = st.find("op")->str();
        Value got;
        try {
            got = stream_op(*feed, ff, xf, clk, pool, tee, st);
        } catch (const std::exception& e) {
            got = S(std::string("C++ THREW: ") + e.what());
        }
        compare(T, (ff ? "fut." : "idx.") + op, name + " step " + std::to_string(i++) + " " + op, got,
                *st.find("expect"));
    }
}

// --- the REAL pump thread (not golden: the generator replaces the reference's thread) -----------
//
// The spawned pump drains the socket and mid() sees it from another thread; a dead socket drops
// into the reconnect loop, and stop() interrupts the reconnect BACKOFF (a 2-30 s wait) promptly
// rather than sleeping it out -- the reference's `_stop.wait(backoff)` property.

Value frame(const std::string& text) {
    Value f = Value::make_object({});
    f.set("f", S(text));
    return f;
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
    kp::test::section("FuturesFeed / IndexFeed, threaded");
    const long long now_ms = km::Clock::system().time_ns() / 1000000;
    FakeWsPool pool;
    pool.timeout_sleep_ms = 1;
    Value s = Value::make_array({});
    s.push(frame(R"([{"ev":"status","status":"connected"}])"));
    s.push(frame(R"([{"ev":"status","status":"auth_success"}])"));
    for (int i = 0; i < 50; ++i) {
        const double m = 6650.0 + 0.25 * (i % 7);
        s.push(frame("[{\"ev\":\"Q\",\"sym\":\"ESZ6\",\"bp\":" + std::to_string(m - 0.125) + ",\"ap\":" +
                     std::to_string(m + 0.125) + ",\"t\":" + std::to_string(now_ms - 60 + i) + "}]"));
    }
    for (int i = 0; i < 50; ++i) s.push(S("timeout"));
    s.push(S("err"));
    pool.conns.push_back(s);
    km::streams::FuturesFeed feed({"SPX"}, "TESTKEY", pool.factory(), {{"SPX", "ESZ6"}});
    feed.start();
    check("futures feed ok after start", feed.ok());
    check("the pump thread delivers mids", eventually([&] { return feed.ticks("SPX") > 40; }, 5.0));
    const auto m = feed.mid("SPX");
    check("mid() fresh from another thread", m.mid.has_value() && m.age_s && *m.age_s < 3.0);
    check("move_scale from another thread", feed.move_scale("SPX", 0.1).has_value());
    check("a dead socket drops into the reconnect loop",
          eventually([&] { return !feed.ok() && feed.health().errors >= 2; }, 5.0));
    const auto t0 = std::chrono::steady_clock::now();
    feed.stop();
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    check("stop() interrupts the 2 s reconnect backoff", took < 0.5, std::to_string(took) + "s");

    FakeWsPool ipool;
    ipool.conns.push_back(Value::make_array({}));      // connect then die at once: start() never throws
    km::streams::IndexFeed idx({"SPX", "NDX"}, "TESTKEY", ipool.factory());
    idx.start();
    check("a failed start is soft: ok=false with last_error", !idx.ok() && idx.last_error().has_value());
    idx.stop();
}

}  // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "tests/data/golden";
    kp::time::init();
    Tally T("massive_streams");
    T.expect({"fut.start", "fut.pump", "fut.state", "fut.mid", "fut.move_scale", "fut.tee",
              "fut.sends", "fut.sleeps", "fut.stop", "idx.start", "idx.pump", "idx.state",
              "idx.snapshot", "idx.tee", "idx.sends", "idx.sleeps"});
    for (const auto& l : read(dir, "massive_streams"))
        if (l.rfind("stream ", 0) == 0) run_stream(T, kp::json::parse(unhex(words(l)[1])));
    threaded_checks();
    return T.report();
}
