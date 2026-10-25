// kalshi::MarketFeed's state machine against the reference's own _pump, with NO socket: the same
// script of frames, calls and checkpoints (gen_kalshi_ws.py), the same fake clock, and at every
// checkpoint the whole observable state -- books as the quote loop reads them, validity, positions,
// fills, health, sids, the command pacer, and everything sent, slept and teed -- as identical JSON.
#include <openssl/evp.h>

#include <deque>

#include "kp/kalshi/ws.hpp"
#include "kp/storage/pyerr.hpp"
#include "vecio.hpp"

using namespace kp::test::vec;
using kp::json::Value;
namespace K = kp::kalshi;

namespace {

struct FakeClock : K::FeedClock {
    std::int64_t ns = 0;
    double mono = 0;
    std::vector<double> sleeps;
    double monotonic() override { return mono; }
    std::int64_t time_ns() override { return ns; }
    void sleep(double d) override {
        sleeps.push_back(d);
        mono += d;
        ns += (std::int64_t)(d * 1e9);
    }
};

std::string sha16(const std::string& s) {
    unsigned char d[32];
    unsigned n = 0;
    EVP_Digest(s.data(), s.size(), d, &n, EVP_sha256(), nullptr);
    static const char* hx = "0123456789abcdef";
    std::string out;
    for (unsigned i = 0; i < 8; ++i) { out.push_back(hx[d[i] >> 4]); out.push_back(hx[d[i] & 15]); }
    return out;
}

// Code points, as Python's len() of the JSON text counts them.
std::size_t cp_len(const std::string& s) {
    std::size_t n = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) ++n;
    return n;
}

Value digest(const Value& x) {
    const std::string s = kp::json::dumps(x, kp::json::COMPACT);
    if (cp_len(s) <= 4000) return x;
    Value d = Value::make_object({});
    d.set("sha", Value::make_string(sha16(s)));
    d.set("len", Value::make_int((long long)(x.is_array() ? x.arr().size() : x.obj().size())));
    return d;
}

std::string first_diff(const std::string& a, const std::string& b) {
    std::size_t i = 0;
    while (i < a.size() && i < b.size() && a[i] == b[i]) ++i;
    const std::size_t from = i > 100 ? i - 100 : 0;
    return "\n   at byte " + std::to_string(i) + "\n   want ..." + b.substr(from, 260) + "\n   got  ..." + a.substr(from, 260);
}

struct Run {
    K::MarketFeed* f = nullptr;
    FakeClock* clk = nullptr;
    Value sent = Value::make_array({});
    Value tee = Value::make_array({});
    bool has_tee = true;
    std::optional<std::string> fail;

    Value state() {
        std::vector<std::string> known = f->tickers();
        const std::set<std::string> want(known.begin(), known.end());
        // + books the feed holds that were never wanted, in the feed's own order
        for (const auto& t : f->book_tickers()) if (!want.count(t)) known.push_back(t);
        Value books = Value::make_object({});
        for (const auto& t : known) {
            auto e = f->entry(t);
            books.set(t, e ? *e : Value());
        }
        Value sample = Value::make_object({});
        const std::size_t step = std::max<std::size_t>(1, known.size() / 8);
        for (std::size_t i = 0; i < known.size(); i += step) sample.set(known[i], *books.find(known[i]));
        Value st = Value::make_object({});
        st.set("books", digest(books));
        st.set("sample", sample);
        st.set("valid", digest(f->valid_map()));
        st.set("positions", digest(f->positions()));
        st.set("fills", digest(Value::make_array(f->pending_fills())));
        st.set("health", f->health());
        st.set("sids", f->sids());
        st.set("cmd", Value::make_array({Value::make_int(f->cmd_id()), Value::make_number(f->next_cmd_t()),
                                         Value::make_number(f->cmd_backoff())}));
        st.set("sent", digest(sent));
        Value sl = Value::make_array({});
        for (double d : clk->sleeps) sl.push(Value::make_number(d));
        st.set("sleeps", digest(sl));
        st.set("tee", digest(tee));
        sent = Value::make_array({});
        clk->sleeps.clear();
        tee = Value::make_array({});
        return st;
    }
    // The feed's book order: valid_map() is _valid's order; books with no _valid entry (a first
    // snapshot that failed half-way) are exactly the ones health() counts in n_books but not here.
    std::vector<std::pair<std::string, bool>> books_order() {
        std::vector<std::pair<std::string, bool>> out;
        const Value vm = f->valid_map();
        for (const auto& kv : vm.arr()) out.emplace_back(kv.arr()[0].str(), kv.arr()[1].boolean());
        return out;
    }

    Value call(const std::string& name, const Value& a) {
        Value r;
        auto strs = [](const Value* v) {
            std::vector<std::string> out;
            if (v && !v->is_null()) for (const auto& s : v->arr()) out.push_back(s.str());
            return out;
        };
        Value res = Value::make_object({});
        try {
            if (name == "subscribe") {
                auto t = strs(a.find("tickers"));
                if (t.empty()) t = f->tickers();
                const Value* ch = a.find("chunk");
                const Value* st = a.find("sid_timeout");
                const auto id = f->subscribe(t, ch && !ch->is_null() ? (std::size_t)ch->integer() : 0,
                                             st ? (st->is_int() ? (double)st->integer() : st->num()) : 10.0);
                r = id ? Value::make_int(*id) : Value();
            } else if (name == "request_snapshot") {
                r = Value::make_int(f->request_snapshot(strs(a.find("tickers"))));
            } else if (name == "drain_fills") {
                r = Value::make_array(f->drain_fills());
            } else if (name == "seed_positions") {
                r = Value::make_int(f->seed_positions(*a.find("positions")));
            } else if (name == "books") {
                const Value* t = a.find("tickers");
                r = digest(t ? f->books(strs(t)) : f->books());
            } else if (name == "entry") {
                auto e = f->entry(a.find("ticker")->str());
                r = e ? *e : Value();
            } else if (name == "position") {
                r = Value::make_number(f->position(a.find("ticker")->str()));
            } else if (name == "wait_for_coverage") {
                r = Value::make_number(f->wait_for_coverage(a.find("fraction")->num(), a.find("timeout")->num(),
                                                            a.find("poll")->num()));
            } else if (name == "health_at") {
                r = f->health(a.find("now")->num());
            } else {
                throw std::runtime_error("unknown call " + name);
            }
            res.set("ok", r);
        } catch (const kp::storage::PyError& e) {
            res.set("error", Value::make_string(e.type + ": " + e.what()));
        }
        return res;
    }
};

}  // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "tests/data/golden";
    Tally T("kalshi_ws");
    T.expect({"scenario", "check", "call", "final"});

    // Group the file into scenarios.
    struct Scn { Value head; std::vector<std::pair<Value, std::string>> events; std::string final_want; };
    std::vector<Scn> scns;
    for (const auto& line : read(dir, "kalshi_ws")) {
        const auto sp = line.find(' ');
        const std::string tag = line.substr(0, sp), rest = line.substr(sp + 1);
        if (tag == "scenario") scns.push_back({kp::json::parse(rest), {}, ""});
        else if (tag == "ev") scns.back().events.emplace_back(kp::json::parse(rest), "");
        else if (tag == "want") scns.back().events.back().second = rest;
        else if (tag == "final") scns.back().final_want = rest;
        else T.hit("unknown_tag:" + tag, false, line);
    }

    for (auto& sc : scns) {
        const std::string name = sc.head.find("name")->str();
        std::vector<std::string> tickers;
        for (const auto& t : sc.head.find("tickers")->arr()) tickers.push_back(t.str());
        std::vector<std::string> channels;
        for (const auto& c : sc.head.find("channels")->arr()) channels.push_back(c.str());
        const bool with_tee = sc.head.find("tee")->boolean();
        const bool with_ws = sc.head.find("ws")->boolean();
        FakeClock clk;
        clk.ns = sc.head.find("t0")->arr()[0].integer();
        clk.mono = sc.head.find("t0")->arr()[1].num();
        Run run;
        run.clk = &clk;
        run.has_tee = with_tee;
        K::MarketFeed::TeeFn tee;
        if (with_tee)
            tee = [&run](std::int64_t ns, const Value& frame) {
                run.tee.push(Value::make_array({Value::make_int(ns), Value::make_string(kp::json::dumps(frame, kp::json::COMPACT))}));
            };
        K::MarketFeed feed(tickers, channels, tee, &clk);
        run.f = &feed;
        if (with_ws)
            feed.set_sender([&run](const std::string& s) {
                if (run.fail) throw kp::storage::PyError("BrokenPipeError", *run.fail);
                run.sent.push(Value::make_string(s));
            });
        T.hit("scenario", true, name);

        std::size_t i = 0;
        auto step = [&]() -> std::optional<std::string> {
            // One recv(): frames return, everything else acts and "times out".
            if (i >= sc.events.size()) { feed.request_stop(); return std::nullopt; }
            const auto& [ev, want] = sc.events[i++];
            const std::string kind = ev.arr()[0].str();
            if (kind == "frame") {
                clk.ns = ev.arr()[2].integer();
                clk.mono = ev.arr()[3].num();
                return ev.arr()[1].str();
            }
            if (kind == "clock") {
                clk.ns = ev.arr()[1].integer();
                clk.mono = ev.arr()[2].num();
            } else if (kind == "check") {
                const std::string got = kp::json::dumps(run.state(), kp::json::COMPACT);
                T.hit("check", got == want, name + " check #" + std::to_string(i) + first_diff(got, want));
            } else if (kind == "call") {
                const std::string got = kp::json::dumps(run.call(ev.arr()[1].str(), ev.arr()[2]), kp::json::COMPACT);
                T.hit("call", got == want, name + " call " + ev.arr()[1].str() + first_diff(got, want));
            } else if (kind == "sendfail") {
                run.fail = ev.arr()[1].is_null() ? std::nullopt : std::optional<std::string>(ev.arr()[1].str());
            } else if (kind == "recv_error") {
                throw kp::storage::PyError(ev.arr()[1].str(), ev.arr()[2].str());
            } else if (kind == "end") {
                feed.request_stop();
            }
            return std::nullopt;
        };
        if (with_ws) {
            feed.run_pump(step);
        } else {
            while (i < sc.events.size()) {
                try {
                    auto raw = step();
                    if (raw) feed.handle(kp::json::parse(*raw));
                } catch (const std::exception&) {
                    break;
                }
            }
        }
        const std::string got = kp::json::dumps(run.state(), kp::json::COMPACT);
        T.hit("final", got == sc.final_want, name + " final" + first_diff(got, sc.final_want));
    }
    return T.report();
}
