// Scripted transports for the massive_* suites: the C++ half of tools/golden/gen_massive.py's
// monkeypatching. One clock, one canned-HTTP queue, one scripted-WebSocket pool -- each doing
// exactly what the Python fakes do, so the reference's outputs are reproducible here.
#pragma once
#include <algorithm>
#include <chrono>
#include <thread>
#include <cstdio>
#include <deque>
#include <optional>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "kp/json/value.hpp"
#include "kp/massive/rest.hpp"
#include "kp/massive/transport.hpp"
#include "vecio.hpp"

namespace kp::test::massive {

using kp::json::Value;
namespace km = kp::massive;

inline std::string unhex(const std::string& h) {
    std::string out;
    out.reserve(h.size() / 2);
    for (std::size_t i = 0; i + 1 < h.size(); i += 2) out += (char)std::stoi(h.substr(i, 2), nullptr, 16);
    return out;
}

// time.monotonic / time.time_ns / time.sleep, scripted exactly as gen_massive.C.
struct FakeClock {
    double mono = 1000.0, mono_step = 0.0;
    long long ns = 1787327400000000000LL, ns_step = 0;
    std::vector<double> sleeps;

    kp::massive::Clock clock() {
        kp::massive::Clock c;
        c.monotonic = [this] { const double v = mono; mono += mono_step; return v; };
        c.time_ns = [this] { const long long v = ns; ns += ns_step; return v; };
        c.sleep = [this](double s) { sleeps.push_back(s); mono += s; };
        return c;
    }
};

// urllib.request.urlopen, canned.
struct FakeHttp {
    std::deque<Value> resps;          // ["ok"|"http"|"err", status, body]
    std::vector<std::string> urls;

    kp::massive::HttpGet getter() {
        return [this](const std::string& url, double) {
            urls.push_back(url);
            if (resps.empty()) throw std::logic_error("no canned response for " + url);
            const Value r = resps.front();
            resps.pop_front();
            if (r.arr()[0].str() == "err") throw std::runtime_error("canned transport failure");
            return kp::massive::HttpResult{(long)r.arr()[1].integer(), r.arr()[2].str()};
        };
    }
};

// websocket.create_connection, scripted. Each connection is a list of items:
//   {"f": text} -> a frame;  "timeout" -> recv times out;  "err" -> the socket dies;
//   "yield" -> pause the pump (on_yield) and time out;  and the connection "connfail" fails
//   to connect at all. An exhausted script dies like a closed socket.
struct FakeWsPool {
    std::deque<Value> conns;
    std::vector<std::pair<int, std::string>> sends;   // (connection index, payload)
    std::vector<std::string> urls;
    int opened = 0;
    std::function<void()> on_yield;
    std::function<void()> on_exhausted;               // streams: stop the feed, as the Python does
    int timeout_sleep_ms = 0;                          // threaded checks: a timeout takes time

    class Ws : public kp::massive::WsTransport {
    public:
        Ws(FakeWsPool* p, int idx, Value script) : p_(p), idx_(idx), script_(std::move(script)) {}
        void connect(const std::string& url, double) override {
            p_->urls.push_back(url);
            if (script_.is_string() && script_.str() == "connfail")
                throw std::runtime_error("connection refused");
        }
        void send_text(const std::string& s) override { p_->sends.emplace_back(idx_, s); }
        std::optional<std::string> recv(double) override {
            if (pos_ >= script_.arr().size()) throw std::runtime_error("script exhausted");
            const Value& it = script_.arr()[pos_++];
            if (it.is_object()) return it.find("f")->str();
            const std::string& k = it.str();
            if (k == "timeout") {
                if (p_->timeout_sleep_ms)
                    std::this_thread::sleep_for(std::chrono::milliseconds(p_->timeout_sleep_ms));
                return std::nullopt;
            }
            if (k == "yield") {
                if (p_->on_yield) p_->on_yield();
                return std::nullopt;
            }
            throw std::runtime_error("Connection to remote host was lost.");
        }
        void close() override {}

    private:
        FakeWsPool* p_;
        int idx_;
        Value script_;
        std::size_t pos_ = 0;
    };

    kp::massive::WsFactory factory() {
        return [this]() -> std::unique_ptr<kp::massive::WsTransport> {
            if (conns.empty()) {
                if (on_exhausted) on_exhausted();
                throw std::runtime_error("no more connections");
            }
            Value s = conns.front();
            conns.pop_front();
            return std::make_unique<Ws>(this, opened++, std::move(s));
        };
    }
};

// --- shared by the WebSocket suites: json builders, send normalisation, the comparison ---

using kp::test::vec::Tally;


inline Value B(bool b) { return Value::make_bool(b); }
inline Value I(long long i) { return Value::make_int(i); }
inline Value S(const std::string& s) { return Value::make_string(s); }
inline Value N(std::optional<double> d) { return d ? Value::make_number(*d) : Value(); }
inline Value OS(const std::optional<std::string>& s) { return s ? S(*s) : Value(); }
inline Value arr(std::initializer_list<Value> v) {
    Value a = Value::make_array({});
    for (const auto& x : v) a.push(x);
    return a;
}
inline Value strs(const std::vector<std::string>& v) {
    Value a = Value::make_array({});
    for (const auto& x : v) a.push(S(x));
    return a;
}
inline std::vector<std::string> strv(const Value& v) {
    std::vector<std::string> out;
    for (const auto& x : v.arr()) out.push_back(x.str());
    return out;
}
inline double num(const Value& v) { return v.is_int() ? (double)v.integer() : v.num(); }

inline const char* label(const std::exception& e) {
    if (dynamic_cast<const km::MassiveError*>(&e)) return "MassiveError";
    if (dynamic_cast<const km::MassiveHistoryError*>(&e)) return "MassiveHistoryError";
    return "error";
}

inline Value norm_sends(FakeWsPool& pool, bool raw) {
    Value out = Value::make_array({});
    for (const auto& [idx, s] : pool.sends) {
        if (raw) { out.push(arr({I(idx), S(s)})); continue; }
        Value d = kp::json::parse(s);
        const Value* a = d.find("action");
        if (a && a->is_string() && (a->str() == "subscribe" || a->str() == "unsubscribe")) {
            std::vector<std::string> ps;
            const std::string& p = d.find("params")->str();
            std::size_t i = 0;
            while (true) {
                const auto j = p.find(',', i);
                ps.push_back(p.substr(i, j == std::string::npos ? std::string::npos : j - i));
                if (j == std::string::npos) break;
                i = j + 1;
            }
            std::sort(ps.begin(), ps.end());
            Value n = Value::make_object({});
            n.set("action", *a);
            n.set("params", strs(ps));
            d = n;
        }
        out.push(arr({I(idx), d}));
    }
    pool.sends.clear();
    return out;
}

inline Value take_sleeps(FakeClock& clk) {
    Value out = Value::make_array({});
    for (double s : clk.sleeps) out.push(Value::make_number(s));
    clk.sleeps.clear();
    return out;
}

inline bool compare(Tally& T, const std::string& tag, const std::string& where, const Value& got, const Value& want) {
    const std::string g = kp::json::dumps(got), w = kp::json::dumps(want);
    const bool ok = g == w;
    if (!ok) {
        std::size_t i = 0;
        while (i < g.size() && i < w.size() && g[i] == w[i]) ++i;
        const std::size_t from = i > 60 ? i - 60 : 0;
        std::printf("  [%s] diverges at %zu:\n    got  ...%s\n    want ...%s\n", where.c_str(), i,
                    g.substr(from, 220).c_str(), w.substr(from, 220).c_str());
    }
    T.hit(tag, ok, where);
    return ok;
}

}  // namespace kp::test::massive
