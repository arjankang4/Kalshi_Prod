// The continuous quoter -- strategy/run_04_quote_loop.main, in C++.
//
//     quote_loop --minutes 5                      # dry: plans every order, sends none
//     KP_ALLOW_LIVE=1 quote_loop --minutes 30 --live
//
// Same flags, same startup order, same per-cycle record, same end-of-run summary and VERDICT line
// as the reference. The cycle itself is kp::app::cycle (src/app/cycle.cpp), which is held to the
// reference by `make parity`; this file is the wiring: it builds the boards, opens the feeds,
// fills CycleIO from the real clients, runs the loop, and tears everything down.
//
// TWO DEPARTURES FROM THE REFERENCE, both deliberate and both on the safety side:
//   --live ALSO REQUIRES KP_ALLOW_LIVE=1 in the environment. Parity with the Python has been
//     proven by replay and by dry runs; the C++ has never sent an order. Until someone decides
//     otherwise, a live run needs two independent switches, and refusing is loud.
//   --out-root DIR puts everything this process WRITES (journals, the feed tapes, the HALT file it
//     watches) under DIR instead of the Python tree, so a test run cannot mix its journals and
//     tapes into ~/Kalshi's, which the storage timers ship to S3. Model artifacts and secrets are
//     still READ from --kalshi-root. The default for both is the Python tree, as production wants.
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "kp/app/cycle.hpp"
#include "kp/app/run04.hpp"
#include "kp/config.hpp"
#include "kp/kalshi/bucket.hpp"
#include "kp/kalshi/client.hpp"
#include "kp/kalshi/env.hpp"
#include "kp/kalshi/ws.hpp"
#include "kp/massive/client.hpp"
#include "kp/massive/contracts.hpp"
#include "kp/massive/futures_net.hpp"
#include "kp/massive/futures_pure.hpp"
#include "kp/massive/history.hpp"
#include "kp/massive/index_rest.hpp"
#include "kp/massive/rest.hpp"
#include "kp/massive/streams.hpp"
#include "kp/massive/transport.hpp"
#include "kp/pricing/theo_core.hpp"
#include "kp/pricing/universe.hpp"
#include "kp/pycompat/repr.hpp"
#include "kp/storage/feed_tee.hpp"
#include "kp/strategy/cancel_all.hpp"

namespace json = kp::json;
namespace app = kp::app;
namespace r4 = kp::app::run04;
namespace th = kp::pricing::theo;
namespace mv = kp::massive;
using OptD = std::optional<double>;

namespace {

// --- the stop event --------------------------------------------------------------------------
// The handler only SETS it: cancel-on-exit makes network calls, and doing real work in a signal
// handler risks re-entering whatever it interrupted. Idempotent, so a second Ctrl-C during the
// cancel sweep does not abort it.
std::atomic<bool> STOP{false};
std::mutex stop_mu;
std::condition_variable stop_cv;

extern "C" void on_signal(int signum) {
    if (!STOP.exchange(true)) {
        char b[96];
        const int n = std::snprintf(b, sizeof b, "\n[run_04] signal %d -- stopping after this cycle, then cancelling\n", signum);
        if (n > 0) { ssize_t w = ::write(2, b, (size_t)n); (void)w; }
    }
    stop_cv.notify_all();
}

// _STOP.wait(s): sleep up to s, returning early on a signal.
void stop_wait(double s) {
    std::unique_lock<std::mutex> lk(stop_mu);
    stop_cv.wait_for(lk, std::chrono::duration<double>(std::max(0.0, s)), [] { return STOP.load(); });
}

double mono_now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
long long wall_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}
// datetime.now(ET) has microsecond resolution; so does this.
kp::time::Instant now_et() { return kp::time::Instant{wall_ns() / 1000 * 1000}; }

void err(const std::string& s) { std::cerr << s << std::endl; }

struct Args {
    bool live = false;
    double minutes = 5.0, interval = r4::QUOTE_INTERVAL_S, warmup = r4::WARMUP_S;
    bool tiers = kp::config::TIERS_ENABLED, edge = kp::config::EDGE_MANAGER_ENABLED;
    bool no_delta_adj = false, no_smooth_skew = false, no_fast_spot = false;
    bool no_reprice_at_send = false, no_guard = false, no_tee = false, no_ws = false;
    bool no_gate = false, adopt_resting = false;
    std::string kalshi_root = KP_KALSHI_ROOT_DEFAULT, out_root;
};

[[noreturn]] void usage(const char* msg) {
    std::fprintf(stderr, "quote_loop: %s\n"
        "usage: quote_loop [--live] [--minutes M] [--interval S] [--warmup S] [--tiers] [--edge]\n"
        "                  [--no-delta-adj] [--no-smooth-skew] [--no-fast-spot] [--no-reprice-at-send]\n"
        "                  [--no-guard] [--no-tee] [--no-ws] [--no-gate] [--adopt-resting]\n"
        "                  [--kalshi-root DIR] [--out-root DIR]\n", msg);
    std::exit(2);
}

Args parse(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string s = argv[i];
        auto val = [&]() -> std::string { if (i + 1 >= argc) usage(("missing value for " + s).c_str()); return argv[++i]; };
        if (s == "--live") a.live = true;
        else if (s == "--minutes") a.minutes = std::stod(val());
        else if (s == "--interval") a.interval = std::stod(val());
        else if (s == "--warmup") a.warmup = std::stod(val());
        else if (s == "--tiers") a.tiers = true;
        else if (s == "--edge") a.edge = true;
        else if (s == "--no-delta-adj") a.no_delta_adj = true;
        else if (s == "--no-smooth-skew") a.no_smooth_skew = true;
        else if (s == "--no-fast-spot") a.no_fast_spot = true;
        else if (s == "--no-reprice-at-send") a.no_reprice_at_send = true;
        else if (s == "--no-guard") a.no_guard = true;
        else if (s == "--no-tee") a.no_tee = true;
        else if (s == "--no-ws") a.no_ws = true;
        else if (s == "--no-gate") a.no_gate = true;
        else if (s == "--adopt-resting") a.adopt_resting = true;
        else if (s == "--kalshi-root") a.kalshi_root = val();
        else if (s == "--out-root") a.out_root = val();
        else usage(("unrecognized argument " + s).c_str());
    }
    if (a.out_root.empty()) a.out_root = a.kalshi_root;
    return a;
}

std::string py_bool(bool b) { return b ? "True" : "False"; }
std::string fmt(const char* f, double x) { char b[64]; std::snprintf(b, sizeof b, f, x); return b; }
std::string exc_line(const std::exception& e, std::size_t n) {
    return kp::kalshi::py_type_name(e) + ": " + kp::kalshi::utf8_prefix(e.what(), n);
}

app::Markets markets_of(const json::Value& rows) {
    app::Markets out;
    if (rows.is_array())
        for (const auto& m : rows.arr()) out.push_back(th::market_from_json(m));
    return out;
}

// --- adapters the fast guard reads through ------------------------------------------------------
struct GuardFutures : kp::strategy::fastcancel::FuturesFeed {
    mv::streams::FuturesFeed* f;
    explicit GuardFutures(mv::streams::FuturesFeed* ff) : f(ff) {}
    std::pair<OptD, OptD> mid(const std::string& u) override {
        const auto m = f->mid(u);
        return {m.mid, m.age_s};
    }
};
struct GuardTracker : kp::strategy::fastcancel::SpotTracker {
    app::Board* b;
    explicit GuardTracker(app::Board* bb) : b(bb) {}
    std::pair<OptD, json::Value> spot(OptD fmid) override {
        std::lock_guard<std::mutex> g(*b->trk_mu);
        const auto r = b->trk->spot(fmid);
        return {r.spot, r.detail.to_json()};
    }
};

// The kalshi::Api the order path sends through -- the real client, but only reachable when both
// switches are on (see the file comment).
struct GatedApi : kp::kalshi::Api {
    kp::kalshi::Client* c;
    explicit GatedApi(kp::kalshi::Client* cc) : c(cc) {}
    json::Value batch_create_orders(const std::vector<json::Value>& o) override { return c->batch_create_orders(o); }
    json::Value batch_cancel_orders(const std::vector<std::string>& ids) override { return c->batch_cancel_orders(ids); }
};

}  // namespace

int run(const Args& args) {
    kp::time::init();

    if (args.live) {
        const char* allow = std::getenv("KP_ALLOW_LIVE");
        if (allow == nullptr || std::string(allow) != "1") {
            std::fprintf(stderr,
                "[run_04] *** REFUSING --live: the C++ quoter has never sent an order. Its parity with the\n"
                "[run_04] *** Python reference is proven by replay and dry runs only. Set KP_ALLOW_LIVE=1 to\n"
                "[run_04] *** confirm a live run deliberately. Nothing was started. ***\n");
            return 2;
        }
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    STOP = false;

    const std::string stamp = r4::run_stamp(now_et());
    const std::string root = args.kalshi_root;

    // --- clients -------------------------------------------------------------------------------
    kp::kalshi::Client k(kp::kalshi::Client::from_env(root));
    kp::kalshi::TokenBucket write_bucket(900.0, 300.0);
    {
        kp::kalshi::BucketHooks h;
        h.debit = [&](double t) { return write_bucket.debit(t); };
        h.penalise = [&](double t) { return write_bucket.penalise(t); };
        h.resize = [&](OptD c, OptD r) { write_bucket.resize(c, r); };
        h.detail = [&] { return write_bucket.detail().to_json(); };
        k.set_write_bucket(h);
    }
    GatedApi api(&k);
    const kp::kalshi::KalshiEnv kenv = kp::kalshi::kalshi_env(root);
    const std::string mkey = mv::load_api_key(root);
    std::unique_ptr<mv::Rest> rest = mv::Rest::live(root);

    // --- discovery (run_04.discover_boards) ------------------------------------------------------
    const auto discover = [&](const app::Board& b) {
        const kp::time::Instant now = now_et();
        const std::string s = r4::board_stamp(now);
        const auto today = [&](const json::Value& rows) {
            app::Markets out;
            if (!rows.is_array()) return out;
            for (const auto& m : rows.arr()) {
                std::string key;
                if (const auto* e = m.find("event_ticker"); e && e->is_string() && !e->str().empty()) key = e->str();
                else if (const auto* t = m.find("ticker"); t && t->is_string()) key = t->str();
                if (key.find("-" + s) != std::string::npos) out.push_back(th::market_from_json(m));
            }
            return out;
        };
        app::Markets dm, hm;
        for (const auto& sn : b.daily_series) {
            const auto got = today(k.markets({{"event_ticker", kp::kalshi::P(sn + "-" + s + "H1600")}}));
            dm.insert(dm.end(), got.begin(), got.end());
        }
        const long long now_ts = wall_ns() / 1000000000LL;
        for (const auto& sn : b.hourly_series) {
            const auto got = today(k.markets({{"series_ticker", kp::kalshi::P(sn)},
                                              {"min_close_ts", kp::kalshi::P(now_ts)}}));
            hm.insert(hm.end(), got.begin(), got.end());
        }
        return std::make_pair(dm, hm);
    };

    // ONE Board PER UNDERLYING, in sorted(QUOTE_BOARDS) order.
    std::vector<app::Board> boards;
    for (const auto& [u, dh] : std::map<std::string, std::pair<std::vector<std::string>, std::vector<std::string>>>{
             {"SPX", {{"KXINX"}, {"KXINXU"}}}, {"NDX", {{"KXNASDAQ100"}, {"KXNASDAQ100U"}}}})
        boards.emplace_back(u, dh.first, dh.second);
    app::CycleIO io;
    io.discover = discover;
    {
        const auto now = now_et();
        const double m0 = mono_now();
        for (auto& b : boards) {
            auto [d, h] = b.discover(io, now, m0, r4::BOARD_REFRESH_S, true);
            b.daily_m = d;
            b.hourly_m = h;
        }
    }
    boards.erase(std::remove_if(boards.begin(), boards.end(),
                                [](const app::Board& b) { return b.daily_m.empty() && b.hourly_m.empty(); }),
                 boards.end());
    if (boards.empty()) { std::fprintf(stderr, "no markets for today's boards\n"); return 1; }
    app::Markets markets;
    for (const auto& b : boards) { const auto m = b.markets(); markets.insert(markets.end(), m.begin(), m.end()); }

    // THE OPTION STRIPS, one per underlying, against ONE Massive connection; strikes pinned from the
    // DAILY board only (the re-centre in the cycle passes daily_m for the same reason).
    std::vector<std::string> tickers;
    const kp::time::Instant t0 = now_et();
    const auto et0 = kp::time::to_et(t0);
    char day[16];
    std::snprintf(day, sizeof day, "%04d-%02d-%02d", et0.year, et0.month, et0.day);
    for (auto& b : boards) {
        const auto rs = mv::client::option_root_and_strikes(*rest, day, b.underlying);
        b.root = rs.root.value_or("");
        b.strikes = rs.strikes;
        b.expiry = r4::occ_expiry(t0);
        const auto bounds = th::boundaries(b.daily_m);
        OptD seed;
        if (!bounds.empty()) seed = (*std::min_element(bounds.begin(), bounds.end()) +
                                     *std::max_element(bounds.begin(), bounds.end())) / 2;
        b.tickers = kp::pricing::universe::desired_tickers(b.daily_m, b.strikes, b.root, b.expiry, seed).tickers;
        tickers.insert(tickers.end(), b.tickers.begin(), b.tickers.end());
    }
    if (tickers.size() > mv::client::QuoteCollector::MAX_TICKERS) {
        std::string s;
        for (const auto& b : boards) s += (s.empty() ? "" : ", ") + b.underlying + "/" + b.root + "=" + std::to_string(b.tickers.size());
        std::fprintf(stderr, "%zu option tickers over the %zu Massive per-connection cap (%s)\n",
                     tickers.size(), mv::client::QuoteCollector::MAX_TICKERS, s.c_str());
        return 1;
    }

    const json::Value wbudget = k.set_write_budget();
    const json::Value reads = k.set_read_budget(std::nullopt, kp::kalshi::Client::READ_RATE_CAP,
                                                kp::kalshi::Client::TIER_SAMPLES, kp::config::READ_SHARE_TRADING);
    const auto jget = [](const json::Value& o, const char* key) {
        const json::Value* v = o.find(key);
        return v ? *v : json::Value();
    };
    // f"{x}" of a value off a JSON record: str() of a string, repr() of anything else.
    const auto pyv = [](const json::Value& v) {
        if (v.is_string()) return v.str();
        return json::py_repr(v);
    };
    // f"{x or 'none'}"
    const auto or_none = [&](const json::Value& v) {
        return kp::kalshi::py_truthy(v) ? pyv(v) : std::string("none");
    };
    for (const auto& b : boards) {
        std::set<std::string> evs;
        for (const auto& m : b.hourly_m) evs.insert(m.event_ticker);
        std::printf("[run_04] %s: %zu markets (%zu daily + %zu hourly across %zu event(s)), strip %s %zu tickers\n",
                    b.underlying.c_str(), b.daily_m.size() + b.hourly_m.size(), b.daily_m.size(),
                    b.hourly_m.size(), evs.size(), b.root.c_str(), b.tickers.size());
    }
    std::printf("[run_04] %zu markets over %zu underlying(s), %zu/%zu option tickers | live=%s tiers=%s edge=%s\n",
                markets.size(), boards.size(), tickers.size(), mv::client::QuoteCollector::MAX_TICKERS,
                py_bool(args.live).c_str(), py_bool(args.tiers).c_str(), py_bool(args.edge).c_str());
    std::printf("[run_04] write budget: tier=%s capacity=%s refill=%s/s max_batch=%s tiers_seen=%s | reserve=%.0f tok\n",
                pyv(jget(wbudget, "usage_tier")).c_str(), pyv(jget(wbudget, "bucket_capacity")).c_str(),
                pyv(jget(wbudget, "refill_rate")).c_str(), pyv(jget(wbudget, "max_batch")).c_str(),
                or_none(jget(wbudget, "tiers_seen")).c_str(), kp::config::WRITE_RESERVE_TOKENS);
    const auto num = [&](const char* key) {
        const json::Value v = jget(reads, key);
        return v.is_number() ? v.num() : 0.0;
    };
    std::printf("[run_04] read budget: %.1f -> %.1f req/s (share %s, cap %.0f), tiers seen %s\n",
                num("req_per_s_before"), num("req_per_s_after"), r4::pct0(num("share")).c_str(),
                num("cap"), or_none(jget(reads, "tiers_seen")).c_str());
    std::fflush(stdout);

    // --- the per-underlying models ----------------------------------------------------------------
    kp::rvol::live::Fetch lean_fetch;
    lean_fetch.closes_ms = [&](const std::string& u, const std::string& d) {
        std::vector<std::pair<std::int64_t, double>> out;
        for (const auto& [t, v] : mv::index::closes_ms(*rest, u, d)) out.emplace_back(t, v);
        return out;
    };
    lean_fetch.second_bars = [&](const std::string& t, const std::string& s, const std::string& e) {
        std::vector<kp::rvol::live::Bar> out;
        for (const auto& row : mv::history::second_bars(*rest, t, s, e)) {
            kp::rvol::live::Bar b;
            if (const auto* x = row.find("t")) b.t = x->integer();
            if (const auto* c = row.find("c"); c && c->is_number()) b.c = c->num();
            out.push_back(b);
        }
        return out;
    };
    const kp::massive::futures::Date today{et0.year, et0.month, et0.day};
    const auto session_volume = [&](const std::string& tk, const kp::massive::futures::Date& d) {
        return mv::futures::session_volume(*rest, tk, d.year, d.month, d.day);
    };
    for (auto& b : boards) {
        b.em = std::make_unique<kp::strategy::edgemgr::EdgeManager>(b.underlying, std::nullopt, std::nullopt,
                                                                    args.edge, root);
        if (!args.edge) b.em->set_operator_flat(true);
        const std::string crepr = (b.em->curve() && !b.em->operator_flat())
            ? b.em->curve()->repr()
            : "<FlatCurve " + kp::pycompat::py_float_repr(kp::config::QUOTE_OFFSET) + ">";
        std::printf("[run_04] %s edge manager: available=%s curve=%s operator_flat=%s\n", b.underlying.c_str(),
                    py_bool(b.em->available()).c_str(), crepr.c_str(), py_bool(b.em->operator_flat()).c_str());
        if (args.edge && !b.em->available())
            std::printf("[run_04] *** %s edge manager UNAVAILABLE (%s) -- no %s market will be quotable. ***\n",
                        b.underlying.c_str(), b.em->reason().c_str(), b.underlying.c_str());

        b.lc = std::make_unique<kp::strategy::leanchain::LeanChain>(b.underlying, lean_fetch, root);
        try { b.lc->start(now_et()); } catch (const std::exception& e) {
            err("[lean:" + b.underlying + "] start failed (" + exc_line(e, 80) + ")");
        }
        std::printf("[run_04] %s lean: enabled=%s available=%s\n", b.underlying.c_str(),
                    py_bool(kp::config::LEAN_ENABLED).c_str(), py_bool(b.lc->ok()).c_str());

        if (!args.no_smooth_skew) b.sk = std::make_unique<kp::strategy::skewstate::SkewState>(b.underlying);
        std::printf("[run_04] %s skew: smoothed=%s", b.underlying.c_str(), py_bool(b.sk != nullptr).c_str());
        if (b.sk) std::printf(" window=%ss recompute=%ss half_life=%ss",
                              kp::pycompat::py_float_repr(b.sk->window_s()).c_str(),
                              kp::pycompat::py_float_repr(b.sk->recompute_s()).c_str(),
                              kp::pycompat::py_float_repr(b.sk->half_life_s()).c_str());
        std::printf("\n");

        if (!args.no_delta_adj) {
            const std::string u = b.underlying;
            b.ft = std::make_unique<kp::strategy::deltaadj::FuturesTicker>(
                u, [&](const std::string& contract) -> std::optional<kp::strategy::deltaadj::FuturesRow> {
                    json::Value page;
                    try { page = mv::futures::quote_page(*rest, contract); }
                    catch (const std::exception& e) {
                        throw kp::strategy::deltaadj::FetchError(kp::kalshi::py_type_name(e), e.what());
                    }
                    const json::Value* rs = page.find("results");
                    if (!rs || !rs->is_array() || rs->arr().empty()) return std::nullopt;
                    const json::Value& r = rs->arr()[0];
                    kp::strategy::deltaadj::FuturesRow fr;
                    if (const auto* x = r.find("bid_price"); x && x->is_number()) fr.bid_price = x->num();
                    if (const auto* x = r.find("ask_price"); x && x->is_number()) fr.ask_price = x->num();
                    if (const auto* x = r.find("timestamp"); x && x->is_number()) fr.timestamp = x->integer();
                    return fr;
                });
            b.ft->start(today, [&](const std::string& product, const kp::massive::futures::Date& d) {
                // Carry the Python class name through, as the reference's f"{type(e).__name__}" does.
                try {
                    return kp::massive::futures::front_month(product, d, session_volume);
                } catch (const kp::strategy::deltaadj::FetchError&) {
                    throw;
                } catch (const std::exception& e) {
                    throw kp::strategy::deltaadj::FetchError(kp::kalshi::py_type_name(e), e.what());
                }
            });
            if (b.ft->ok()) {
                b.da = std::make_unique<kp::strategy::deltaadj::DeltaAdjuster>(b.ft.get());
            } else {
                err("[run_04] *** " + b.underlying + " delta adjustment UNAVAILABLE ("
                    + b.ft->last_error().value_or("None") + ") -- its theo is the option snapshot's, re-priced every "
                    + kp::pycompat::py_float_repr(args.interval) + "s ***");
            }
        }
        std::printf("[run_04] %s delta-adj: %s\n", b.underlying.c_str(), b.da ? "on" : "OFF");
    }

    // --- THE FAST SPOT PATH: futures + index sockets and a per-underlying basis -----------------
    std::vector<std::string> us;
    for (const auto& b : boards) us.push_back(b.underlying);
    const std::string ws_dir = args.out_root + "/data/ws";
    std::unique_ptr<kp::storage::FeedTee> spot_tape, tee;
    std::unique_ptr<mv::streams::FuturesFeed> futures;
    std::unique_ptr<mv::streams::IndexFeed> idxfeed;
    if (!args.no_fast_spot) {
        if (!args.no_tee && kp::config::QUOTE_TEE_ENABLED) {
            kp::storage::FeedTee::Options o;
            o.ws_dir = ws_dir;
            o.channel = std::string(kp::config::SPOT_TEE_CHANNEL);
            json::Value ss = json::Value::make_object({});
            json::Value ua = json::Value::make_array({});
            for (const auto& u : us) ua.push(json::Value::make_string(u));
            ss.set("underlyings", ua);
            ss.set("feeds", json::parse(R"(["futures","index"])"));
            ss.set("records", json::Value::make_string("updates_only"));
            ss.set("live", json::Value::make_bool(args.live));
            ss.set("run", json::Value::make_string(stamp));
            o.subscribe_state = ss;
            spot_tape = std::make_unique<kp::storage::FeedTee>(o);
            spot_tape->start();
            if (!spot_tape->ok()) {
                err("[run_04] *** spot tape UNAVAILABLE (" + spot_tape->error.value_or("None") + ") ***");
                spot_tape.reset();
            }
        }
        mv::streams::Tee stee = nullptr;
        if (spot_tape) stee = [&](long long ns, const json::Value& m) { spot_tape->offer(ns, m); };
        futures = std::make_unique<mv::streams::FuturesFeed>(
            us, mkey, mv::net_ws_factory(), std::map<std::string, std::string>{},
            [&](const std::string& u, const std::string& d) -> std::optional<std::string> {
                const auto dd = kp::massive::futures::Date{std::stoi(d.substr(0, 4)), std::stoi(d.substr(5, 2)),
                                                           std::stoi(d.substr(8, 2))};
                return kp::massive::futures::front_month(kp::massive::futures::product_for(u), dd, session_volume);
            },
            day, stee);
        futures->start();
        idxfeed = std::make_unique<mv::streams::IndexFeed>(us, mkey, mv::net_ws_factory(), stee);
        idxfeed->start();
        std::this_thread::sleep_for(std::chrono::seconds(1));
        json::Value contracts = json::Value::make_object({});
        for (const auto& [u, c] : futures->contract()) contracts.set(u, json::Value::make_string(c));
        json::Value unds = json::Value::make_array({});
        for (const auto& [u, q] : idxfeed->snapshot()) unds.push(json::Value::make_string(u));   // map: sorted
        std::printf("[run_04] futures ws: ok=%s contracts=%s%s\n", py_bool(futures->ok()).c_str(),
                    json::py_repr(contracts).c_str(),
                    futures->last_error() ? (" ERR " + *futures->last_error()).c_str() : "");
        std::printf("[run_04] index ws:   ok=%s underlyings=%s%s\n", py_bool(idxfeed->ok()).c_str(),
                    json::py_repr(unds).c_str(),
                    idxfeed->last_error() ? (" ERR " + *idxfeed->last_error()).c_str() : "");
        if (!futures->ok() || !idxfeed->ok()) {
            err("[run_04] *** fast spot UNAVAILABLE -- falling back to REST index + delta-adjust ***");
            futures->stop();
            idxfeed->stop();
            futures.reset();
            idxfeed.reset();
        }
    }
    for (auto& b : boards)
        if (futures) b.trk = std::make_unique<kp::pricing::basis::BasisTracker>(b.underlying);

    // THE BASIS IS FOLDED ON THE INDEX TICK, in its own thread: the only instant both legs are current.
    std::atomic<bool> basis_stop{false};
    std::thread basis_thread;
    struct Joiner {
        std::atomic<bool>& stop; std::thread& t;
        ~Joiner() { stop = true; if (t.joinable()) t.join(); }
    } basis_joiner{basis_stop, basis_thread};
    if (futures) {
        basis_thread = std::thread([&] {
            std::map<std::string, long long> seen;
            while (!basis_stop.load()) {
                try {
                    for (const auto& [u, q] : idxfeed->snapshot()) {
                        const long long ts = q.ts_ns.value_or(-1);
                        auto it = seen.find(u);
                        if (it != seen.end() && it->second == ts) continue;
                        seen[u] = ts;
                        for (auto& b : boards)
                            if (b.underlying == u && b.trk) {
                                const auto m = futures->mid(u);
                                std::lock_guard<std::mutex> g(*b.trk_mu);
                                b.trk->observe(m.mid, q, mono_now());
                            }
                    }
                } catch (const std::exception& e) {
                    err("[basis] " + exc_line(e, 80));
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        });
    }
    auto apply_lock = std::make_shared<std::recursive_mutex>();

    // --- the option collector ------------------------------------------------------------------
    mv::client::QuoteCollector col(tickers, mkey, mv::net_ws_factory());
    col.start();
    col.wait_for_coverage(tickers.size() / 2, args.warmup);
    std::this_thread::sleep_for(std::chrono::seconds(2));

    kp::strategy::orders::RestingBook book;
    auto jrnl = kp::strategy::journal::open_journal(args.out_root, stamp, "quotes.jsonl");
    auto rjrnl = kp::strategy::journal::open_journal(args.out_root, stamp, "risk.jsonl");
    kp::strategy::risktape::RiskTape rtape(rjrnl.get());

    // THE KALSHI MARKET-DATA SOCKET, with its tape opened first so no frame arrives with nowhere to go.
    if (!args.no_tee && kp::config::QUOTE_TEE_ENABLED) {
        kp::storage::FeedTee::Options o;
        o.ws_dir = ws_dir;
        o.channel = std::string(kp::config::QUOTE_TEE_CHANNEL);
        json::Value ss = json::Value::make_object({});
        ss.set("channels", json::parse(R"(["orderbook_delta","fill","market_positions"])"));
        ss.set("n_markets", json::Value::make_int((long long)markets.size()));
        json::Value sa = json::Value::make_array({});
        std::set<std::string> ser;
        for (const auto& b : boards) for (const auto& s : b.series()) ser.insert(s);
        for (const auto& s : ser) sa.push(json::Value::make_string(s));
        ss.set("series", sa);
        ss.set("live", json::Value::make_bool(args.live));
        ss.set("run", json::Value::make_string(stamp));
        o.subscribe_state = ss;
        tee = std::make_unique<kp::storage::FeedTee>(o);
        tee->start();
        if (!tee->ok()) {
            err("[run_04] *** feed tape UNAVAILABLE (" + tee->error.value_or("None") + ") -- quoting without one ***");
            tee.reset();
        }
    }
    std::printf("[run_04] feed tape: %s\n", tee ? ("on dt=" + tee->dt + " channel=" + tee->channel + " -> " + ws_dir).c_str() : "OFF");

    std::unique_ptr<kp::kalshi::MarketFeed> mdfeed;
    if (!args.no_ws) {
        try {
            std::vector<std::string> mt;
            for (const auto& m : markets) mt.push_back(m.ticker);
            kp::kalshi::MarketFeed::TeeFn tf = nullptr;
            if (tee) tf = [&](std::int64_t ns, const json::Value& f) { tee->offer(ns, f); };
            mdfeed = std::make_unique<kp::kalshi::MarketFeed>(mt, kp::kalshi::WS_CHANNELS, tf);
            mdfeed->start(kenv.ws_base, [&] { return k.auth_headers("GET", "/trade-api/ws/v2"); });
            const double cov = mdfeed->wait_for_coverage(kp::config::WS_BOOK_MIN_COVERAGE, kp::config::WS_BOOK_WARMUP_S);
            const json::Value h = mdfeed->health();
            std::printf("[run_04] ws book: %s/%s markets (%s coverage) in %s commands, %s frames\n",
                        pyv(jget(h, "n_valid")).c_str(), pyv(jget(h, "n_wanted")).c_str(), r4::pct0(cov).c_str(),
                        pyv(jget(h, "commands")).c_str(), pyv(jget(h, "frames")).c_str());
            if (cov < kp::config::WS_BOOK_MIN_COVERAGE)
                err("[run_04] *** ws coverage " + r4::pct0(cov) + " below the floor -- cycles will fall back to REST ***");
        } catch (const std::exception& e) {
            mdfeed.reset();
            err("[run_04] *** ws book UNAVAILABLE (" + exc_line(e, 90) + ") -- polling /markets/orderbooks ***");
        }
    } else {
        std::printf("[run_04] ws book: OFF (--no-ws), polling /markets/orderbooks\n");
    }

    // STARTUP RECONCILIATION: orders already resting are CANCELLED by default -- in a live run.
    try {
        const auto events = kp::strategy::cancel_all::today_events();
        const auto pre = kp::strategy::cancel_all::resting_across(k, events);
        if (pre.empty()) {
            std::printf("[run_04] startup: nothing resting\n");
        } else {
            std::printf("[run_04] startup: %zu order(s) already resting across today's events\n", pre.size());
            if (args.adopt_resting) {
                err("[run_04] *** --adopt-resting: leaving them. ***");
            } else if (!args.live) {
                err("[run_04] *** dry run: NOT cancelling them. A --live run would have. ***");
            } else {
                const long left = kp::strategy::cancel_all::cancel_events(k, events, true, std::cout, std::cerr);
                std::printf("[run_04] startup: cancelled, %ld still resting\n", left);
                if (left) {
                    std::fprintf(stderr, "startup cancel left orders resting -- refusing to quote on top of them; run  cancel_all\n");
                    return 1;
                }
            }
        }
    } catch (const std::exception& e) {
        err("[run_04] *** could not read resting orders at startup (" + exc_line(e, 90)
            + ") -- proceeding BLIND to anything already resting ***");
    }

    // SEED THE POSITION MAP FROM REST: market_positions only reports changes.
    if (mdfeed) {
        try {
            const json::Value held = k.open_positions();
            const long n = mdfeed->seed_positions(held);
            std::printf("[run_04] positions at startup: %zu held, feed seeded (%ld known)\n",
                        held.is_object() ? held.obj().size() : (std::size_t)0, n);
        } catch (const std::exception& e) {
            err("[run_04] *** could not read positions at startup (" + exc_line(e, 80) + ") -- the inventory gate starts blind ***");
        }
    }

    kp::strategy::risklimit::SessionRisk risk(std::nullopt, args.out_root + "/var/HALT");
    risk.set_state_dir(args.out_root + "/var/risk");                 // config.RISK_STATE_DIR
    // THE LOSS LIMIT READS THE ACCOUNT: REST balance + REST positions (strict: a failed read is not
    // "flat"), polled every RISK_ACCOUNT_POLL_S, marked to the Kalshi mid in the cycle.
    kp::strategy::risklimit::AccountPoller acct([&] { return k.balance(); },
                                                [&] { return k.open_positions(true); });
    acct.start();
    const auto snap0 = acct.latest();
    std::string bal_line;
    if (snap0) {
        bal_line = "$" + kp::strategy::orders::fmt_thousands2(snap0->balance) + ", "
                   + std::to_string(snap0->positions.size()) + " positions";
    } else {
        bal_line = "UNREADABLE (" + acct.last_error().value_or("None") + ")";
    }
    char pl[32];
    std::snprintf(pl, sizeof pl, "%g", acct.interval_s());
    std::printf("[run_04] risk: loss limit $%s on ACCOUNT equity marked to Kalshi mid (poll %ss) | balance %s"
                " | halt file %s%s\n",
                kp::strategy::orders::fmt_thousands2(risk.limit().value_or(0)).c_str(), pl, bal_line.c_str(),
                risk.halt_path().c_str(), risk.halt_reason().empty() ? "" : " *** PRESENT AT STARTUP ***");

    // THE FAST GUARD, after the resting book exists and after the startup reconcile.
    std::unique_ptr<GuardFutures> gfut;
    std::vector<std::unique_ptr<GuardTracker>> gtrk;
    std::unique_ptr<kp::strategy::fastcancel::FastGuard> guard;
    const long max_batch = [&] {
        const json::Value* mb = wbudget.find("max_batch");
        return (mb && mb->is_number() && mb->integer() > 0) ? (long)mb->integer() : kp::kalshi::BATCH_FALLBACK;
    }();
    if (futures && !args.no_guard) {
        gfut = std::make_unique<GuardFutures>(futures.get());
        std::map<std::string, kp::strategy::fastcancel::SpotTracker*> tr;
        for (auto& b : boards) {
            gtrk.push_back(std::make_unique<GuardTracker>(&b));
            tr[b.underlying] = gtrk.back().get();
        }
        guard = std::make_unique<kp::strategy::fastcancel::FastGuard>(
            *gfut, tr, book, args.live ? &api : nullptr, args.live, apply_lock, std::nullopt, std::nullopt,
            [](const std::vector<kp::strategy::orders::Order>& cs, const kp::strategy::orders::ApplyResult& res) {
                std::set<std::string> t;
                for (const auto& c : cs) t.insert(c.ticker);
                std::string s;
                for (const auto& x : t) s += (s.empty() ? "" : ", ") + x;
                err("[guard] pulled " + std::to_string(res.cancelled + res.cancel_gone) + "/" + std::to_string(cs.size())
                    + " on a fast move: " + s.substr(0, 120));
            },
            max_batch);
        guard->start();
    }
    std::printf("[run_04] fast guard: %s\n", guard ? "on" : "OFF");
    std::fflush(stdout);

    // --- CycleIO, from the real clients ---------------------------------------------------------
    io.now = now_et;
    io.mono = mono_now;
    io.time_ns = wall_ns;
    io.perf = mono_now;
    io.has_mdfeed = mdfeed != nullptr;
    if (mdfeed) {
        io.drain_fills = [&] { return mdfeed->drain_fills(); };
        io.feed_alive = [&] { return mdfeed->alive(); };
        io.feed_health = [&] { return mdfeed->health(); };
        io.feed_pump_error = [&] { return mdfeed->pump_error(); };
        io.feed_books = [&](const std::vector<std::string>& t) { return mdfeed->books(t); };
        io.feed_positions = [&] { return mdfeed->positions(); };
        io.feed_entry = [&](const std::string& t) { return mdfeed->entry(t); };
    }
    io.col_health = [&] {
        const auto h = col.health();
        app::FeedHealth f;
        f.alive = h.alive;
        f.pump_error = h.pump_error;
        f.quote_age_s = h.quote_age_s;
        json::Value raw = json::Value::make_object({});
        raw.set("alive", json::Value::make_bool(h.alive));
        raw.set("pump_error", h.pump_error ? json::Value::make_string(*h.pump_error) : json::Value());
        raw.set("quote_age_s", h.quote_age_s ? json::Value::make_number(*h.quote_age_s) : json::Value());
        raw.set("n_symbols", json::Value::make_int((long long)h.n_symbols));
        raw.set("reconnects", json::Value::make_int(h.reconnects));
        f.raw = raw;
        return f;
    };
    io.col_reconnect = [&] { return col.reconnect(); };
    io.col_reconnects = [&] { return col.reconnects(); };
    io.col_snapshot = [&] { return col.snapshot(); };
    io.col_recenter = [&](const std::vector<std::string>& want) { return col.recenter(want); };
    io.index_quotes = [&](const std::vector<std::string>& u) {
        if (idxfeed) {
            auto got = idxfeed->snapshot(u);
            if (!got.empty()) return app::IndexQuotes(got.begin(), got.end());
        }
        try {
            auto got = mv::index::snapshot(*rest, u);
            return app::IndexQuotes(got.begin(), got.end());
        } catch (const std::exception& e) {
            err("[index] snapshot failed (" + exc_line(e, 80) + ")");
            return app::IndexQuotes{};
        }
    };
    io.rest_orderbooks = [&](const std::vector<std::string>& t) { return k.orderbooks(t, 10L); };
    io.has_futures = futures != nullptr;
    if (futures) {
        io.fut_mid = [&](const std::string& u) -> std::pair<OptD, OptD> {
            const auto m = futures->mid(u);
            return {m.mid, m.age_s};
        };
        io.fut_move_scale = [&](const std::string& u, double h, double s) { return futures->move_scale(u, h, s); };
        io.fut_health = [&] { const auto h = futures->health(); json::Value o = json::Value::make_object({});
                              o.set("name", json::Value::make_string(h.name)); o.set("ok", json::Value::make_bool(h.ok));
                              o.set("frames", json::Value::make_int(h.frames)); return o; };
        io.idx_health = [&] { const auto h = idxfeed->health(); json::Value o = json::Value::make_object({});
                              o.set("name", json::Value::make_string(h.name)); o.set("ok", json::Value::make_bool(h.ok));
                              o.set("frames", json::Value::make_int(h.frames)); return o; };
    }
    io.acct_latest = [&] { return acct.latest(); };
    io.bucket = &write_bucket;
    io.api = args.live ? &api : nullptr;
    if (tee) io.tee_detail = [&] { return tee->detail(); };
    if (spot_tape) io.spot_tape_detail = [&] { return spot_tape->detail(); };
    if (guard) io.guard_detail = [&] { return guard->detail().to_json(); };
    io.err = err;

    app::CycleState state;
    for (const auto& m : markets) state.tickers.push_back(m.ticker);
    state.max_batch = max_batch;
    state.risk = &risk;
    state.guard = guard.get();
    state.risktape = &rtape;
    state.apply_lock = apply_lock;
    state.stop = &STOP;
    const app::CycleArgs cargs{args.live, args.tiers, args.no_reprice_at_send};

    // --- the loop ---------------------------------------------------------------------------------
    const double deadline = mono_now() + args.minutes * 60.0;
    long n = 0;
    r4::RunStats st;
    long quiet = 0, cancelled = 0;
    std::vector<double> ms;
    while (mono_now() < deadline && !STOP.load()) {
        const double t_start = mono_now();
        json::Value r;
        try {
            r = app::cycle(state, io, boards, book, cargs, jrnl.get());
        } catch (const std::exception& e) {
            err("[cycle] " + exc_line(e, 120));
            r = json::Value::make_object({});
            r.set("error", json::Value::make_string(kp::kalshi::py_type_name(e)));
        }
        ++n;
        const auto geti = [&](const char* key) {
            const json::Value* v = r.find(key);
            return v && v->is_number() ? (long)v->integer() : 0L;
        };
        if (r.find("error")) {
            ++st.errors;
        } else {
            const json::Value* sk = r.find("skipped");
            if (sk && sk->is_string() && sk->str() == "no_spot") ++st.no_spot;
            st.placed += geti("placed");
            cancelled += geti("cancelled");
            if (const json::Value* m = r.find("ms"); m && m->is_number()) ms.push_back(m->num()); else ms.push_back(0.0);
            if (!geti("placed") && !geti("cancelled")) ++quiet;
        }
        if (n <= 3 || n % 10 == 0 || geti("placed") || geti("cancelled")) {
            std::printf("[%4ld] %s\n", n, json::dumps(r).c_str());
            std::fflush(stdout);
        }
        stop_wait(args.interval - (mono_now() - t_start));
    }

    // --- teardown, in the reference's order -------------------------------------------------------
    col.stop();
    if (mdfeed) {
        const json::Value h = mdfeed->health();
        std::printf("[run_04] ws book: %s frames, %s commands, %s gaps, %s resubscribes, %s/%s valid at exit\n",
                    pyv(jget(h, "frames")).c_str(), pyv(jget(h, "commands")).c_str(), pyv(jget(h, "gaps")).c_str(),
                    pyv(jget(h, "reconnects")).c_str(), pyv(jget(h, "n_valid")).c_str(), pyv(jget(h, "n_wanted")).c_str());
        mdfeed->stop();
    }
    if (tee) {
        tee->stop();
        std::printf("[run_04] feed tape: %ld frames, %ld segment(s), %.1f MB gz, %ld dropped%s\n", tee->frames,
                    tee->segments_closed, (double)tee->bytes_written / 1e6, tee->dropped.load(),
                    tee->error ? (" | ERROR " + *tee->error).c_str() : "");
    }
    if (guard) {
        guard->stop();
        std::printf("[run_04] fast guard: %s\n", json::dumps(guard->detail().to_json()).c_str());
    }
    basis_stop = true;
    if (basis_thread.joinable()) basis_thread.join();
    if (futures) futures->stop();
    if (idxfeed) idxfeed->stop();
    if (spot_tape) spot_tape->stop();
    for (auto& b : boards)
        if (b.ft) b.ft->stop();
    acct.stop();
    jrnl->close();
    std::printf("[run_04] risk tape: %s\n", json::dumps(rtape.detail().to_json()).c_str());
    rjrnl->close();
    std::sort(ms.begin(), ms.end());
    const long n_ok = std::max(1L, n - st.errors);
    std::printf("[run_04] %ld cycles | quiet %ld/%ld (%s) | placed %ld cancelled %ld errors %ld\n", n, quiet,
                n - st.errors, r4::pct0((double)quiet / (double)n_ok).c_str(), st.placed, cancelled, st.errors);
    if (!ms.empty())
        std::printf("[run_04] cycle ms: p50 %.0f p95 %.0f max %.0f\n", ms[ms.size() / 2],
                    ms[(std::size_t)((double)ms.size() * 0.95)], ms.back());
    {
        std::lock_guard<std::recursive_mutex> g(*apply_lock);
        std::printf("[run_04] %zu orders resting, %.0f contracts\n", book.all_orders().size(), book.contracts());
    }
    std::printf("[run_04] risk: %s\n", json::dumps(app::to_json(risk.summary())).c_str());
    st.cycles = n;
    const double expected = std::max(1.0, args.minutes * 60.0 / std::max(args.interval, 0.001));
    const auto v = r4::verdict(st, expected, r4::in_trading_session(now_et()));
    std::string reasons;
    for (std::size_t i = 0; i < v.reasons.size(); ++i) reasons += (i ? "; " : "") + v.reasons[i];
    std::printf("[run_04] VERDICT %s  cycles %ld/%ld errors %ld (%s) spot %s placed %ld quiet %s%s\n",
                v.verdict.c_str(), v.cycles, v.expected, v.errors, r4::pct0(v.error_rate).c_str(),
                r4::pct0(v.spot_rate).c_str(), v.placed, r4::pct0((double)quiet / (double)n_ok).c_str(),
                reasons.empty() ? "" : ("  -- " + reasons).c_str());
    std::fflush(stdout);
    if (STOP.load())
        std::printf("[run_04] stopped early: %s\n", risk.summary().halted.empty() ? "signal" : risk.summary().halted.c_str());
    int exit_code = args.no_gate ? 0 : v.exit_code;
    if (!risk.summary().halted.empty()) exit_code = 1;
    if (args.live) {
        try {
            const long left = kp::strategy::cancel_all::cancel_events(k, kp::strategy::cancel_all::today_events(),
                                                                      true, std::cout, std::cerr);
            std::printf("[exit] cancel-on-exit complete, %ld still resting\n", left);
            if (left) err("[exit] WARNING: run  cancel_all  NOW");
        } catch (const std::exception& e) {
            err("[exit] CANCEL FAILED (" + exc_line(e, 200) + ") -- run  cancel_all  NOW");
        }
    }
    std::fflush(stdout);
    return exit_code;
}

int main(int argc, char** argv) {
    const Args args = parse(argc, argv);
    try {
        return run(args);
    } catch (const std::exception& e) {
        // What the reference does with an exception nothing caught: a traceback, then exit 1.
        std::fflush(stdout);
        // The module-qualified name Python prints on the last traceback line.
        std::string t = kp::kalshi::py_type_name(e);
        if (t == "MassiveError") t = "massive.client.MassiveError";
        else if (t == "MassiveHistoryError") t = "massive.history.MassiveHistoryError";
        else if (t == "KalshiError") t = "kalshi.client.KalshiError";
        std::fprintf(stderr, "Traceback (most recent call last):\n  (C++: no frames)\n%s: %s\n",
                     t.c_str(), e.what());
        return 1;
    }
}
