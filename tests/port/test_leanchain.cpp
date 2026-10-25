// strategy/leanchain against the reference, end to end: a real LeanChain started through an
// injected backfill of real 2026-08-21 bars, fed a 5-second index poll through observe(), and read
// at ~65 checkpoints against the real captured SPXW board and KXINXU market list -- flat BS, the
// fitted smile with the lean disabled, and a refused smile -- plus every row refusal, an empty
// board, a missing forward, a closes_ms outage and an unavailable underlying.
//
// After the replay, the three rules strategy/test_leanchain.py pins are asserted directly on the
// C++ output (they are also implied by the vectors, but a vector cannot say WHY a row differs).
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "boards.hpp"
#include "kp/pricing/atmvol.hpp"
#include "kp/pricing/bs.hpp"
#include "kp/pricing/skew.hpp"
#include "kp/strategy/leanchain.hpp"
#include "vecio.hpp"

using namespace kp::test::vec;
namespace LC = kp::strategy::leanchain;
namespace LV = kp::rvol::live;
namespace th = kp::pricing::theo;
using kp::time::Instant;

namespace {

bool same_s(const std::optional<std::string>& g, const std::string& w) {
    return (g ? *g : std::string("-")) == w;
}

bool meta_ok(const LC::Meta& m, const std::vector<std::string>& w) {
    // meta tag reason span vleft w_impl sigma_impl forward horizon minutes_left sigma_rv en sm rvf..
    bool ok = same_s(m.lean_reason, w[2]) && same(m.buffer_span_s, w[3])
           && same(m.variance_left, w[4]) && same(m.w_impl, w[5]) && same(m.sigma_impl, w[6])
           && same(m.forward, w[7]) && same_s(m.rv_horizon, w[8]) && same(m.minutes_left, w[9])
           && same(m.sigma_rv, w[10]) && (m.enabled ? "1" : "0") == w[11]
           && (m.smiled ? "1" : "0") == w[12];
    if (!ok) return false;
    if (!m.rv_features) return w.size() == 14 && w[13] == "None";
    if (w.size() != 13 + m.rv_features->size()) return false;
    for (std::size_t i = 0; i < m.rv_features->size(); ++i) {
        const auto& [k, v] = (*m.rv_features)[i];
        const auto eq = w[13 + i].find('=');
        if (w[13 + i].substr(0, eq) != k || !same(v, w[13 + i].substr(eq + 1))) return false;
    }
    return true;
}

bool row_ok(const LC::Row& r, const std::vector<std::string>& w) {
    // row tag tk p_rv shadow p_ref sigma_rv sigma_impl w_rv w_impl enabled reason horizon minutes
    return same(r.p_rv, w[3]) && same(r.p_rv_shadow, w[4]) && same(r.p_ref, w[5])
        && same(r.sigma_rv, w[6]) && same(r.sigma_impl, w[7]) && same(r.w_rv, w[8])
        && same(r.w_impl, w[9]) && (r.enabled ? "1" : "0") == w[10]
        && (r.reason.empty() ? std::string("-") : r.reason) == w[11]
        && same_s(r.rv_horizon, w[12]) && same(r.minutes, w[13]);
}

struct Data {
    std::vector<std::pair<std::int64_t, double>> closes;
    std::vector<LV::Bar> bars;
    std::string mode;
};

}  // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "tests/data/golden";
    const std::string root = argc > 2 ? argv[2] : "";
    kp::time::init();
    Tally T("leanchain");
    T.expect({"market", "smarket", "closes", "bars", "start", "observe", "chain", "meta", "row", "end",
              "unavailable", "unavailable_chain", "rule1_4pm", "rule2_own_settlement",
              "rule3_raw_implied", "disabled_is_inert", "flat_smile_is_bs"});

    const auto boards = kp::test::load_boards(dir + "/skew_boards.vec");
    if (boards.empty()) { std::printf("FATAL: skew_boards.vec missing\n"); return 1; }
    const auto& board = boards[0].board;
    const auto& strikes = boards[0].strikes;
    const auto afit = kp::pricing::atmvol::fit(board, strikes);
    const auto real_fit = kp::pricing::skew::fit(board, strikes, afit.forward, afit.w);
    const auto flat_fit = kp::pricing::skew::fit(board, strikes, std::nullopt, std::nullopt);
    const kp::pricing::skew::Board empty_board;
    const std::vector<double> no_strikes;

    std::vector<th::Market> markets, sweep;
    std::map<std::string, Data> data;
    std::map<std::string, std::unique_ptr<LC::LeanChain>> chains;
    std::vector<std::pair<std::int64_t, double>>* fill_closes = nullptr;
    std::vector<LV::Bar>* fill_bars = nullptr;

    LC::Chain cur;
    std::string cur_tag;
    std::vector<std::string> row_order;
    // For the rule checks: the last full A/B/C triple at a mid-session SPX checkpoint.
    std::map<char, LC::Chain> last_abc;
    double last_fwd = 0.0;

    for (const auto& line : read(dir, "leanchain")) {
        const auto w = words(line);
        const std::string& fn = w[0];
        if (fn == "pt") {
            fill_closes->emplace_back(std::stoll(w[1]), hx(w[2]));
        } else if (fn == "bar") {
            fill_bars->push_back({std::stoll(w[1]), ohx(w[2])});
        } else if (fn == "market") {
            th::Market m;
            m.ticker = w[1];
            m.strike_type = ostr(w[2]);
            m.floor_strike = ohx(w[3]);
            m.cap_strike = ohx(w[4]);
            if (!none(w[5])) m.close_time_s = std::stoll(w[5]);
            markets.push_back(m);
            T.hit(fn, true, line);
        } else if (fn == "smarket") {
            th::Market m;
            m.ticker = w[1];
            m.strike_type = ostr(w[2]);
            m.floor_strike = ohx(w[3]);
            m.close_time_s = std::stoll(w[5]);
            sweep.push_back(m);
            T.hit(fn, true, line);
        } else if (fn == "closes") {
            fill_closes = &data[w[1]].closes;
            T.hit(fn, true, line);
        } else if (fn == "bars") {
            fill_bars = &data[w[1]].bars;
            T.hit(fn, true, line);
        } else if (fn == "start") {
            Data& d = data.at(w[1]);
            d.mode = w[3];
            LV::Fetch fetch;
            fetch.closes_ms = [&d](const std::string&, const std::string&) {
                if (d.mode == "closes") throw std::runtime_error("Fail: closes_ms down");
                return d.closes;
            };
            fetch.second_bars = [&d](const std::string&, const std::string&, const std::string&) {
                return d.bars;
            };
            const std::string u = w[1].substr(0, w[1].find('.'));
            auto lc = std::make_unique<LC::LeanChain>(u, fetch, root);
            const bool ok = lc->ok() && lc->start(Instant{std::stoll(w[2])});
            T.hit(fn, (ok ? "1" : "0") == w[4], line);
            chains[w[1]] = std::move(lc);
        } else if (fn == "observe") {
            auto& lc = *chains.at(w[1]);
            if (w[2] == "none") {
                lc.observe(nullptr);
            } else {
                kp::massive::index::IndexQuote q;
                q.ts_ns = std::stoll(w[3]);
                q.value = hx(w[4]);
                lc.observe(&q);
            }
            T.hit(fn, true, line);
        } else if (fn == "chain") {
            // chain tag now mono fwd enabled skew board
            const std::string scen = w[1].substr(0, w[1].find('#'));
            auto& lc = *chains.at(scen);
            std::optional<bool> en;
            if (w[5] != "None") en = (w[5] == "1");
            const kp::pricing::skew::SkewFit* sf =
                w[6] == "real" ? &real_fit : w[6] == "flat" ? &flat_fit : nullptr;
            const bool real = w[7] == "real";
            cur = lc.chain(w[8] == "main" ? markets : sweep, real ? board : empty_board,
                           real ? strikes : no_strikes,
                           Instant{std::stoll(w[2])}, hx(w[3]), ohx(w[4]), en, sf);
            cur_tag = w[1];
            row_order.clear();
            T.hit(fn, true, line);
            if (scen == "SPX.ok" && real && w[8] == "main" && ohx(w[4]) && lc.calendar().phi(Instant{std::stoll(w[2])}) < 0.5) {
                last_abc[w[1].back()] = cur;
                last_fwd = *ohx(w[4]);
            }
        } else if (fn == "meta") {
            T.hit(fn, w[1] == cur_tag && meta_ok(cur.meta, w), line);
        } else if (fn == "row") {
            const LC::Row* r = cur.rows.find(w[2]);
            row_order.push_back(w[2]);
            T.hit(fn, w[1] == cur_tag && r && row_ok(*r, w), line);
        } else if (fn == "end") {
            bool ok = w[1] == cur_tag && std::to_string(cur.rows.size()) == w[2]
                   && same(cur.first_sigma_rv(), w[3]) && row_order.size() == cur.rows.size();
            std::size_t i = 0;
            for (const auto& kv : cur.rows) ok = ok && kv.first == row_order[i++];
            T.hit(fn, ok, line);
        } else if (fn == "unavailable") {
            LC::LeanChain lc(w[1], LV::Fetch{}, root);
            T.hit(fn, (lc.ok() ? "1" : "0") == w[2] && (!lc.reason().empty() ? "1" : "0") == w[3],
                  line);
            const auto c = lc.chain(markets, board, strikes, kp::time::from_et(2026, 8, 21, 12, 0),
                                    0.0, 7700.0);
            T.hit("unavailable_chain",
                  c.rows.empty() && c.meta.lean_reason && !c.meta.lean_reason->empty()
                      && c.meta.buffer_span_s == 0.0 && c.meta.enabled, line);
        } else if (fn == "unavailable_chain") {
            // checked alongside `unavailable`
        } else {
            T.hit("unknown:" + fn, false, line);
        }
    }

    // ---- the rules of strategy/test_leanchain.py, on the C++ output --------------------------
    const LC::Chain& A = last_abc.at('A');
    const LC::Chain& B = last_abc.at('B');
    const LC::Chain& C = last_abc.at('C');
    bool r1 = true, r2 = true, r3 = true, inert = true, flat = true;
    int n_hourly = 0, n_4pm = 0;
    std::map<long long, double> w_by_close;   // one strike per settlement: w must rise with it
    for (const auto& m : markets) {
        const LC::Row* a = A.rows.find(m.ticker);
        // The real KXINX/KXINXU rows only; the synthetic ones exist to reach refusals.
        if (!a || !m.close_time_s || m.ticker.rfind("SYN-", 0) == 0) continue;
        const int hour = kp::time::to_et(Instant{*m.close_time_s * 1000000000LL}).hour;
        if (hour >= 16) {
            ++n_4pm;
            r1 = r1 && a->reason == "4pm_board_implied_only" && !a->p_rv && !a->p_rv_shadow;
            continue;
        }
        if (!a->reason.empty() || !a->w_rv) continue;
        ++n_hourly;
        w_by_close.emplace(*m.close_time_s, *a->w_rv);
        // Rule 3: the reference IS the implied chain -- no premium centring, so with no smile
        // p_ref is exactly flat BS at the market's own implied w, and p_rv the same at w_rv.
        if (m.floor_strike && m.strike_type != "between") {
            r3 = r3 && a->p_ref == kp::pricing::bs::p_above(last_fwd, *m.floor_strike, *a->w_impl)
                    && a->p_rv == kp::pricing::bs::p_above(last_fwd, *m.floor_strike, *a->w_rv);
        }
        const LC::Row* b = B.rows.find(m.ticker);
        const LC::Row* c = C.rows.find(m.ticker);
        inert = inert && b && !b->p_rv && b->p_rv_shadow.has_value();
        flat = flat && c && c->p_rv_shadow == a->p_rv_shadow && c->p_ref == a->p_ref;
    }
    double prev = 0.0;
    for (const auto& [close, wv] : w_by_close) { r2 = r2 && wv > prev; prev = wv; }
    T.hit("rule1_4pm", r1 && n_4pm > 0, "the 4pm board is never leaned");
    T.hit("rule2_own_settlement", r2 && w_by_close.size() >= 3,
          "w_rv rises with each market's own settlement");
    T.hit("rule3_raw_implied", r3 && n_hourly > 0, "p_ref present wherever p_rv is");
    T.hit("disabled_is_inert", inert, "enabled=false: p_rv None, shadow still recorded");
    T.hit("flat_smile_is_bs", flat, "a refused smile prices exactly as no smile");
    return T.report();
}
