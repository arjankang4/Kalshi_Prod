// kalshi::Client (+ strategy::cancel_all) against the reference's own _request, replayed from a
// script through a fake transport (see gen_kalshi_client.py). NO NETWORK: nothing here can reach
// Kalshi, and the write paths are exercised only against the script.
//
// The whole TRACE of each call must match byte for byte as JSON: request lines, bodies, headers,
// timeouts, every sleep (pacer and backoff), the write bucket after the call, the read rate, and the
// result or exception.
#include <deque>
#include <sstream>

#include "kp/kalshi/client.hpp"
#include "kp/pycompat/round.hpp"
#include "kp/storage/pyerr.hpp"
#include "kp/strategy/cancel_all.hpp"
#include "vecio.hpp"

using namespace kp::test::vec;
using kp::json::Value;
namespace K = kp::kalshi;

namespace {

struct FakeClock : K::ClientClock {
    double mono = 0, wall = 0;
    std::vector<double> sleeps;
    double monotonic() override { return mono; }
    double time() override { return wall; }
    void sleep(double d) override { sleeps.push_back(d); mono += d; wall += d; }
};

// kalshi/client.TokenBucket, replicated for the test only (the real one is bucket.hpp's).
struct TestBucket {
    FakeClock* c;
    double capacity = 900, rate = 300, level = 900, t = 1000.0, spent = 0;
    long denied = 0;
    double refill(double now) {
        if (now > t) {
            const double x = level + (now - t) * rate;
            level = x < capacity ? x : capacity;
            t = now;
        }
        return level;
    }
    double debit(double n) { refill(c->monotonic()); level -= n; spent += n; return level; }
    double penalise(double n) { ++denied; return debit(n); }
    void resize(std::optional<double> cap, std::optional<double> r) {
        refill(c->monotonic());
        if (cap) { capacity = *cap; level = level < capacity ? level : capacity; }
        if (r) rate = *r;
    }
    Value detail() {
        Value d = Value::make_object({});
        d.set("available", Value::make_number(kp::pycompat::py_round_nd(refill(c->monotonic()), 1)));
        d.set("capacity", Value::make_number(capacity));
        d.set("refill_rate", Value::make_number(rate));
        d.set("spent", Value::make_number(kp::pycompat::py_round_nd(spent, 1)));
        d.set("denied", Value::make_int(denied));
        return d;
    }
};

struct FakeTransport : K::Transport {
    std::deque<Value> script;
    Value reqs = Value::make_array({});
    kp::net::HttpResponse request(const std::string& method, const std::string& target,
                                  const kp::net::Headers& headers, const std::string& body,
                                  double timeout_s) override {
        if (script.empty()) throw std::out_of_range("script exhausted");
        const Value item = script.front();
        script.pop_front();
        Value r = Value::make_object({});
        r.set("method", Value::make_string(method));
        r.set("target", Value::make_string(target));
        const bool has_body = method == "POST" || method == "PUT" || method == "PATCH" || !body.empty();
        r.set("body", has_body && !body.empty() ? Value::make_string(body) : Value());
        Value hs = Value::make_array({});
        for (const auto& [k, v] : headers) hs.push(Value::make_array({Value::make_string(k), Value::make_string(v)}));
        r.set("headers", hs);
        r.set("timeout", timeout_s == (double)(long long)timeout_s ? Value::make_int((long long)timeout_s)
                                                                    : Value::make_number(timeout_s));
        reqs.push(r);
        const Value* reused = item.find("reused");
        const bool ru = reused && reused->is_bool() && reused->boolean();
        if (const Value* e = item.find("error")) throw kp::net::HttpError(e->str(), ru);
        kp::net::HttpResponse resp;
        resp.status = item.find("status")->integer();
        if (const Value* hx = item.find("body_hex")) {
            for (std::size_t i = 0; i + 1 < hx->str().size(); i += 2)
                resp.body.push_back((char)std::stoi(hx->str().substr(i, 2), nullptr, 16));
        } else {
            resp.body = item.find("body")->str();
        }
        resp.reused = ru;
        return resp;
    }
};

K::Params params_of(const Value& a) {
    K::Params p;
    const Value* ps = a.find("params");
    if (ps) for (const auto& kv : ps->arr()) p.emplace_back(kv.arr()[0].str(), kv.arr()[1]);
    return p;
}
std::vector<std::string> strs(const Value* v) {
    std::vector<std::string> out;
    if (v) for (const auto& s : v->arr()) out.push_back(s.str());
    return out;
}
std::optional<double> onum(const Value* v) {
    if (!v || v->is_null()) return std::nullopt;
    return v->is_int() ? (double)v->integer() : v->num();
}

std::string first_diff(const std::string& a, const std::string& b) {
    std::size_t i = 0;
    while (i < a.size() && i < b.size() && a[i] == b[i]) ++i;
    const std::size_t from = i > 80 ? i - 80 : 0;
    return "\n   at byte " + std::to_string(i) + "\n   want ..." + b.substr(from, 200) + "\n   got  ..." + a.substr(from, 200);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "tests/data/golden";
    Tally T("kalshi_client");
    T.expect({"reset", "events", "markets", "get", "paged", "orderbooks", "resting_orders", "balance", "fills",
              "open_positions", "cancel_order", "batch_create_orders", "batch_cancel_orders", "write_budget",
              "set_write_budget", "set_read_budget", "endpoint_costs", "cancel_events", "resting_across"});

    FakeClock clock;
    auto tr = std::make_shared<FakeTransport>();
    K::Client::Config cfg{"https://example.test/trade-api/v2", "KEYID", "/nonexistent"};
    const K::Client::SignFn fake_sign = [](const std::string& ts, const std::string& m, const std::string& p) {
        return "SIG|" + ts + "|" + m + "|" + p;
    };
    auto cl = std::make_unique<K::Client>(cfg, tr, &clock, fake_sign);
    TestBucket bucket{&clock};
    cl->set_write_bucket(K::bind_bucket(bucket));

    Value pending;          // the `call` line awaiting its `trace`
    for (const auto& line : read(dir, "kalshi_client")) {
        const auto sp = line.find(' ');
        const std::string tag = line.substr(0, sp);
        const std::string rest = line.substr(sp + 1);
        if (tag == "reset") {
            // A new TokenBucket(900, 300) at mono 1000, the pacer's next slot at 0, the rate given.
            bucket = TestBucket{&clock};
            clock.mono = 1000.0;
            cl = std::make_unique<K::Client>(cfg, tr, &clock, fake_sign);
            cl->set_write_bucket(K::bind_bucket(bucket));
            cl->set_read_rate(hx(rest));
            T.hit(tag, true, line);
            continue;
        }
        if (tag == "events") {
            const Value e = kp::json::parse(rest);
            const std::string d = e.find("date")->str();
            const auto got = kp::strategy::cancel_all::today_events(std::stoi(d.substr(0, 4)), std::stoi(d.substr(5, 2)),
                                                                     std::stoi(d.substr(8, 2)));
            T.hit(tag, got == strs(e.find("events")), line);
            continue;
        }
        if (tag == "call") { pending = kp::json::parse(rest); continue; }
        if (tag != "trace") { T.hit("unknown_tag:" + tag, false, line); continue; }

        const std::string name = pending.find("name")->str();
        const Value& a = *pending.find("args");
        tr->script.clear();
        for (const auto& s : pending.find("script")->arr()) tr->script.push_back(s);
        tr->reqs = Value::make_array({});
        clock.mono = pending.find("t")->arr()[0].num();
        clock.wall = pending.find("t")->arr()[1].num();
        clock.sleeps.clear();
        K::Client& client = *cl;
        std::ostringstream out, err;
        bool want_stdout = false;
        Value res = Value::make_object({});
        try {
            Value r;
            if (name == "markets") r = client.markets(params_of(a));
            else if (name == "get") r = client.get(a.find("path")->str(), params_of(a));
            else if (name == "paged") r = client.paged(a.find("path")->str(), a.find("key")->str(),
                                                       a.find("signed")->boolean(), params_of(a));
            else if (name == "orderbooks") {
                const Value* d = a.find("depth");
                r = client.orderbooks(strs(a.find("tickers")),
                                      d->is_null() ? std::nullopt : std::optional<long>(d->integer()));
            } else if (name == "resting_orders") r = client.resting_orders(params_of(a));
            else if (name == "balance") r = client.balance();
            else if (name == "fills") r = client.fills(params_of(a));
            else if (name == "open_positions") r = client.open_positions();
            else if (name == "cancel_order") r = client.cancel_order(a.find("order_id")->str());
            else if (name == "batch_create_orders") r = client.batch_create_orders(a.find("orders")->arr());
            else if (name == "batch_cancel_orders") r = client.batch_cancel_orders(strs(a.find("order_ids")));
            else if (name == "write_budget") r = client.write_budget();
            else if (name == "endpoint_costs") r = client.endpoint_costs();
            else if (name == "set_write_budget")
                r = client.set_write_budget(onum(a.find("refill_rate")), (int)a.find("samples")->integer());
            else if (name == "set_read_budget")
                r = client.set_read_budget(onum(a.find("refill_rate")), *onum(a.find("cap")),
                                           (int)a.find("samples")->integer(), *onum(a.find("share")));
            else if (name == "cancel_events") {
                want_stdout = true;
                r = Value::make_int(kp::strategy::cancel_all::cancel_events(
                    client, strs(a.find("events")), a.find("verbose")->boolean(), out, err));
            } else if (name == "resting_across") {
                r = Value::make_array(kp::strategy::cancel_all::resting_across(client, strs(a.find("events"))));
            } else {
                T.hit("unknown_call:" + name, false, line);
                continue;
            }
            res.set("ok", r);
        } catch (const K::KalshiError& e) {
            res.set("KalshiError", Value::make_array({Value::make_int(e.status), Value::make_string(e.body)}));
        } catch (const kp::net::HttpError& e) {
            res.set("HttpError", Value::make_string(e.what()));
        } catch (const kp::storage::PyError& e) {
            res.set("error", Value::make_string(e.type));
        } catch (const std::exception& e) {
            // The one non-exchange, non-transport failure _request has: a 2xx that is not JSON.
            res.set("error", Value::make_string("JSONDecodeError"));
        }
        Value trace = Value::make_object({});
        trace.set("requests", tr->reqs);
        Value sl = Value::make_array({});
        for (double s : clock.sleeps) sl.push(Value::make_number(s));
        trace.set("sleeps", sl);
        trace.set("result", res);
        trace.set("bucket", Value::make_array({Value::make_number(bucket.level), Value::make_number(bucket.spent),
                                               Value::make_int(bucket.denied), Value::make_number(bucket.capacity),
                                               Value::make_number(bucket.rate)}));
        trace.set("rate", Value::make_number(client.read_rate()));
        trace.set("unused_script", Value::make_int((long long)tr->script.size()));
        if (want_stdout) {
            trace.set("stdout", Value::make_string(out.str()));
            trace.set("stderr", Value::make_string(err.str()));
        }
        const std::string got = kp::json::dumps(trace, kp::json::COMPACT);
        T.hit(name, got == rest, name + first_diff(got, rest));
    }
    return T.report();
}
