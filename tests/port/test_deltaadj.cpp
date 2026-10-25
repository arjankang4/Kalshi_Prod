// strategy/deltaadj (DeltaAdjuster + FuturesTicker) and massive/futures_pure against the
// reference, plus a threaded smoke test of the real poll thread with an injected fetch.
#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <thread>

#include "kp/massive/futures_pure.hpp"
#include "kp/strategy/deltaadj.hpp"
#include "kp/time/et.hpp"
#include "tapes_io.hpp"
#include "vecio.hpp"

using namespace kp::test::vec;
using namespace kp::test::tapes;
namespace da = kp::strategy::deltaadj;
namespace fu = kp::massive::futures;

namespace {

struct Env {
    double wall = 0.0;
    double mono0 = 0.0, mono1 = 0.0;
    int mono_calls = 0;
    // what the next fetch does
    enum { ROW, EMPTY, ERR } kind = EMPTY;
    da::FuturesRow row;
    std::string err_type, err_msg;
};

fu::Date parse_date(const std::string& s) {
    return fu::Date{std::stoi(s.substr(0, 4)), std::stoi(s.substr(5, 2)), std::stoi(s.substr(8, 2))};
}

std::vector<std::string> split(const std::string& s, char c, int max_splits = -1) {
    std::vector<std::string> out;
    std::size_t p0 = 0;
    for (std::size_t p = 0; p < s.size(); ++p) {
        if (s[p] == c && (max_splits < 0 || (int)out.size() < max_splits)) {
            out.push_back(s.substr(p0, p - p0));
            p0 = p + 1;
        }
    }
    out.push_back(s.substr(p0));
    return out;
}

int run_deltaadj(const std::string& dir) {
    Tally T("deltaadj");
    T.expect({"ticker", "poll", "mid", "tdet", "adj", "anchor", "adjust", "adet", "start"});
    auto env = std::make_shared<Env>();
    std::map<int, std::unique_ptr<da::FuturesTicker>> tick;
    std::map<int, std::unique_ptr<da::DeltaAdjuster>> adj;
    const auto fetch = [env](const std::string&) -> std::optional<da::FuturesRow> {
        if (env->kind == Env::ERR) throw da::FetchError(env->err_type, env->err_msg);
        if (env->kind == Env::EMPTY) return std::nullopt;
        return env->row;
    };
    const auto wall = [env] { return env->wall; };
    const auto mono = [env] { return env->mono_calls++ == 0 ? env->mono0 : env->mono1; };

    for (const auto& line : read(dir, "deltaadj")) {
        const auto parts = tabs(line);
        const auto w = words(parts[0]);
        const std::string& fn = w[0];
        if (fn == "ticker") {
            auto t = std::make_unique<da::FuturesTicker>(
                w[2], fetch, none(w[3]) ? da::FUTURES_POLL_S : hx(w[3]), wall, mono);
            t->set_ticker(w[4] == "-" ? std::nullopt : std::optional<std::string>(w[4]));
            tick[std::stoi(w[1])] = std::move(t);
            T.hit(fn, true, line);
        } else if (fn == "poll") {
            auto& t = *tick.at(std::stoi(w[1]));
            const std::string& tok = w[2];
            if (tok == "empty") {
                env->kind = Env::EMPTY;
            } else if (tok.rfind("err:", 0) == 0) {
                // the exception's message rides in the third tab field (it has spaces)
                env->kind = Env::ERR;
                env->err_type = tok.substr(4);
                env->err_msg = parts.at(2);
            } else {
                const auto f = split(tok, ':');
                env->kind = Env::ROW;
                env->row.bid_price = ohx(f[1]);
                env->row.ask_price = ohx(f[2]);
                env->row.timestamp.reset();
                if (!none(f[3])) env->row.timestamp = std::stoll(f[3]);
            }
            env->mono0 = hx(w[3]);
            env->mono1 = hx(w[4]);
            env->mono_calls = 0;
            const double waited = t.poll_once();
            const std::string le = t.last_error().value_or("");
            T.hit(fn, same(waited, w[5]) && std::to_string(t.polls()) == w[6]
                      && std::to_string(t.errors()) == w[7] && le == (parts.size() > 1 ? parts[1] : ""),
                  line);
        } else if (fn == "mid") {
            env->wall = hx(w[2]);
            auto& t = *tick.at(std::stoi(w[1]));
            const auto m = none(w[3]) ? t.mid() : t.mid(hx(w[3]));
            T.hit(fn, same(m.mid, w[4]) && same(m.age_s, w[5]), line);
        } else if (fn == "tdet") {
            env->wall = hx(w[2]);
            T.hit(fn, J(tick.at(std::stoi(w[1]))->detail().to_json()) == parts[1], line);
        } else if (fn == "adj") {
            const da::FuturesTicker* t = none(w[2]) ? nullptr : tick.at(std::stoi(w[2])).get();
            adj[std::stoi(w[1])] = std::make_unique<da::DeltaAdjuster>(
                t, none(w[3]) ? da::MAX_ADJ_DOLLARS : hx(w[3]));
            T.hit(fn, true, line);
        } else if (fn == "anchor") {
            env->wall = hx(w[2]);
            auto& a = *adj.at(std::stoi(w[1]));
            const auto v = a.anchor_now();
            T.hit(fn, same(v, w[3]) && same(a.anchor_age_s(), w[4]), line);
        } else if (fn == "adjust") {
            env->wall = hx(w[2]);
            auto& a = *adj.at(std::stoi(w[1]));
            const auto [f, d] = a.adjust(ohx(w[3]), ohx(w[4]));
            T.hit(fn, same(f, w[5]) && J(d.to_json()) == parts[1], line);
        } else if (fn == "adet") {
            env->wall = hx(w[2]);
            T.hit(fn, J(adj.at(std::stoi(w[1]))->detail().to_json()) == parts[1], line);
        } else if (fn == "start") {
            da::FuturesTicker t(w[1], fetch, da::FUTURES_POLL_S, wall, mono);
            const bool raise = w[2] == "raise";
            t.start(fu::Date{2026, 8, 21}, [raise](const std::string& p, const fu::Date&)
                        -> std::optional<std::string> {
                if (raise) throw da::FetchError("MassiveHistoryError", "no bars for " + p);
                return std::nullopt;
            });
            T.hit(fn, J(t.detail().to_json()) == parts[1] && !t.ok(), line);
        } else {
            T.hit("unknown_tag:" + fn, false, line);
        }
    }
    return T.report();
}

int run_futures(const std::string& dir) {
    Tally T("futures_pure");
    T.expect({"tf", "qc", "product", "res", "fm", "pt", "etns"});
    for (const auto& line : read(dir, "futures_pure")) {
        const auto w = words(line);
        const std::string& fn = w[0];
        if (fn == "tf") {
            T.hit(fn, fu::third_friday(std::stoi(w[1]), std::stoi(w[2])).iso() == w[3], line);
        } else if (fn == "qc") {
            const auto c = fu::quarterly_candidates(w[1], parse_date(w[2]), std::stoi(w[3]));
            bool ok = std::to_string(c.size()) == w[4] && w.size() == 5 + c.size();
            for (std::size_t i = 0; ok && i < c.size(); ++i)
                ok = c[i].first + "@" + c[i].second.iso() == w[5 + i];
            T.hit(fn, ok, line);
        } else if (fn == "product") {
            const std::string u = w[1] == "-" ? "" : w[1];
            const std::string pf = fu::product_for(u);
            T.hit(fn, (pf.empty() ? "-" : pf) == w[2] && fu::product(u).value_or("None") == w[3]
                      && fu::cache_ticker(u).value_or("None") == w[4], line);
        } else if (fn == "res") {
            T.hit(fn, (fu::valid_resolution(w[1] == "-" ? "" : w[1]) ? "1" : "0") == w[2], line);
        } else if (fn == "fm") {
            const fu::Date d = parse_date(w[2]);
            const int n = std::stoi(w[3]);
            std::optional<std::string> inc;
            if (w[4] != "None") inc = w[4] == "-" ? std::string() : w[4];
            const std::size_t nv = std::stoul(w[5]);
            std::map<std::string, double> vol;
            for (std::size_t i = 0; i < nv; ++i) {
                const auto kv = split(w[6 + i], '=');
                vol[kv[0]] = std::stod(kv[1]);
            }
            const auto got = fu::front_month("ES", d, [&](const std::string& tk, const fu::Date&) {
                const auto it = vol.find(tk);
                return it == vol.end() ? 0.0 : it->second;
            }, n, inc);
            T.hit(fn, got.value_or("None") == w[6 + nv], line);
        } else if (fn == "pt") {
            // the ticker is a JSON string literal (it can carry a \n)
            const auto q0 = line.find('"');
            const auto q1 = line.rfind('"');
            const std::string tk = kp::json::parse(line.substr(q0, q1 - q0 + 1)).str();
            const auto rest = words(line.substr(q1 + 1));
            const auto p = fu::parse_ticker(tk, std::stoi(w[1]));
            bool ok = rest[0] == "None" ? !p
                      : (p && p->product == rest[0] && std::to_string(p->month) == rest[1]
                         && std::to_string(p->year) == rest[2]);
            T.hit(fn, ok, line);
        } else if (fn == "etns") {
            T.hit(fn, std::to_string(fu::et_ns(parse_date(w[1]), std::stoi(w[2]), std::stoi(w[3])))
                      == w[4], line);
        } else {
            T.hit("unknown_tag:" + fn, false, line);
        }
    }
    return T.report();
}

// The real thread: start() with a resolving front month, a fetch that counts, stop() joins.
int run_thread() {
    std::printf("\nFuturesTicker thread\n");
    std::atomic<int> calls{0};
    const long long ts = (long long)(da::system_wall() * 1e9);
    da::FuturesTicker t("SPX", [&](const std::string& tk) -> std::optional<da::FuturesRow> {
        ++calls;
        if (tk != "ESU6") throw std::runtime_error("wrong contract " + tk);
        return da::FuturesRow{7700.0, 7700.5, ts};
    }, 0.005);
    kp::test::check("not ok before start", !t.ok());
    t.start(fu::Date{2026, 8, 21}, [](const std::string& p, const fu::Date&) {
        return std::optional<std::string>(p + "U6");
    });
    for (int i = 0; i < 400 && t.polls() < 5; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    kp::test::check("ok while polling", t.ok());
    kp::test::check("it polled repeatedly", t.polls() >= 5 && t.errors() == 0);
    const auto m = t.mid();
    kp::test::check("mid is (bid+ask)/2 on the vendor stamp", m.mid && *m.mid == 7700.25);
    t.stop();
    const int after = calls.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    kp::test::check("stop() joins: no poll after it returns", calls.load() == after);
    kp::test::check("not ok after stop", !t.ok());
    return kp::test::report("futures_thread");
}

}  // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "tests/data/golden";
    kp::time::init();
    int rc = run_deltaadj(dir);
    rc |= run_futures(dir);
    rc |= run_thread();
    return rc;
}
