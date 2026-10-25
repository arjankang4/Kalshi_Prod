// pricing/validate_theo.resolve_and_price_boards against the reference, driven as run_04.cycle()
// drives it: a SkewState carried across cycles, perturbed snapshots, every spot.resolve branch,
// calendar/now absent; plus universe.quotes_by_strike.
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "kp/json/value.hpp"
#include "kp/pricing/universe.hpp"
#include "kp/pricing/validate_theo.hpp"
#include "kp/pricing/varcal.hpp"
#include "kp/strategy/skewstate.hpp"
#include "tapes_io.hpp"
#include "vecio.hpp"

using namespace kp::test::vec;
using namespace kp::test::tapes;
namespace vt = kp::pricing::validate_theo;
namespace th = kp::pricing::theo;
namespace ix = kp::massive::index;
namespace ss = kp::strategy::skewstate;

namespace {

kp::massive::OptionSnapshot parse_snap(const std::vector<std::string>& w) {
    kp::massive::OptionSnapshot s;
    const std::size_t n = std::stoul(w[2]);
    for (std::size_t i = 0; i < n; ++i) {
        const std::string& tok = w[3 + i];
        std::vector<std::string> f;
        std::size_t p0 = 0;
        for (std::size_t p = 0; p <= tok.size(); ++p)
            if (p == tok.size() || tok[p] == ':') { f.push_back(tok.substr(p0, p - p0)); p0 = p + 1; }
        // the symbol itself contains a ':' ("O:SPXW...")
        const std::string sym = f[0] + ":" + f[1];
        kp::massive::OptionQuote oq;
        if (f[2] == "EMPTY") { s[sym] = oq; continue; }
        oq.q.present = true;
        oq.q.bp = ohx(f[2]);
        oq.q.ap = ohx(f[3]);
        oq.q.bs = ohx(f[4]);
        oq.q.as = ohx(f[5]);
        s[sym] = oq;
    }
    return s;
}

std::string o(const std::optional<std::string>& s) { return s ? *s : "-"; }
std::string o(const std::string& s) { return s.empty() ? "-" : s; }

std::vector<double> take(const std::vector<std::string>& w, std::size_t i) {
    const std::size_t n = std::stoul(w[i]);
    std::vector<double> v;
    for (std::size_t k = 0; k < n; ++k) v.push_back(hx(w[i + 1 + k]));
    return v;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "tests/data/golden";
    const std::string root = argc > 2 ? argv[2] : "";
    kp::time::init();

    // --- quotes_by_strike ---
    Tally Q("universe_qbs");
    Q.expect({"snap", "qbs"});
    kp::massive::OptionSnapshot qsnap;
    for (const auto& line : read(dir, "universe_qbs")) {
        const auto parts = tabs(line);
        const auto w = words(parts[0]);
        if (w[0] == "snap") { qsnap = parse_snap(w); Q.hit("snap", true, line); continue; }
        const auto ks = take(w, 3);
        const auto b = kp::pricing::universe::quotes_by_strike(qsnap, w[1], w[2], ks);
        const auto want = words(parts[1]);
        bool ok = std::to_string(b.size()) == want[0] && want.size() == 1 + b.size();
        std::size_t i = 1;
        for (const auto& [k, q] : b) {
            if (!ok) break;
            // compare field-wise, bit-exactly
            std::vector<std::string> g;
            std::size_t p0 = 0;
            const std::string& tok = want[i++];
            for (std::size_t p = 0; p <= tok.size(); ++p)
                if (p == tok.size() || tok[p] == ':') { g.push_back(tok.substr(p0, p - p0)); p0 = p + 1; }
            ok = g.size() == 6 && g[0][0] == k.right && same(k.strike, g[1]) && same(q.bp, g[2])
                 && same(q.ap, g[3]) && same(q.bs, g[4]) && same(q.as, g[5]);
        }
        Q.hit("qbs", ok, line);
    }
    int rc = Q.report();

    // --- resolve_and_price_boards sessions ---
    Tally T("boards");
    T.expect({"mkt", "sess", "snap", "cycle", "res", "pr", "fair", "sdet"});
    std::map<std::string, std::vector<th::Market>> daily, hourly;
    std::map<std::string, kp::pricing::varcal::VarianceCalendar> cals;
    std::map<int, kp::massive::OptionSnapshot> snaps;
    std::optional<ss::SkewState> sk;
    std::optional<vt::PricedBoards> pb;
    std::size_t fair_i = 0;
    std::vector<const std::pair<std::string, vt::BoardFair>*> fair_list;

    for (const auto& line : read(dir, "boards")) {
        const auto parts = tabs(line);
        const auto w = words(parts[0]);
        const std::string& fn = w[0];
        if (fn == "mkt") {
            const auto m = th::market_from_json(kp::json::parse(parts[1]));
            (w[2] == "daily" ? daily : hourly)[w[1]].push_back(m);
            T.hit(fn, true, line);
        } else if (fn == "sess") {
            if (w[3] == "1") sk.emplace(w[1]); else sk.reset();
            T.hit(fn, true, line);
        } else if (fn == "snap") {
            snaps[std::stoi(w[1])] = parse_snap(w);
            T.hit(fn, true, line);
        } else if (fn == "cycle") {
            // the previous cycle's merged map must have been consumed exactly: no extra tickers
            if (pb) T.hit("fair_count", fair_i == fair_list.size(), line);
            const std::string& u = w[1];
            if (!cals.count(u))
                cals.emplace(u, kp::pricing::varcal::VarianceCalendar::load(u, "default", root));
            std::optional<kp::time::Instant> now;
            if (!none(w[5])) now = kp::time::Instant{std::stoll(w[5])};
            const double mono = hx(w[7]);
            std::optional<ix::IndexQuote> iq;
            if (w[8] != "None") {
                std::vector<std::string> f;
                std::size_t p0 = 0;
                for (std::size_t p = 0; p <= w[8].size(); ++p)
                    if (p == w[8].size() || w[8][p] == ':') { f.push_back(w[8].substr(p0, p - p0)); p0 = p + 1; }
                ix::IndexQuote q;
                q.underlying = u;
                q.ticker = u == "SPX" ? "I:SPX" : "I:NDX";
                q.value = ohx(f[0]);
                if (!none(f[1])) q.ts_ns = std::stoll(f[1]);
                if (f[2] != "-") q.market_status = f[2];
                iq = q;
            }
            const long long at_ns = std::stoll(w[9]);
            const auto ks = take(w, 11);
            std::optional<vt::SkewObserver> obs;
            if (w[10] == "1" && sk) obs = ss::observer(*sk, mono);
            pb.emplace(vt::resolve_and_price_boards(
                daily[u], hourly[u], snaps.at(std::stoi(w[2])), w[3], w[4], ks, now,
                w[6] == "1" ? &cals.at(u) : nullptr, iq, obs ? &*obs : nullptr, at_ns));
            fair_list.clear();
            for (const auto& kv : pb->fairs) fair_list.push_back(&kv);
            fair_i = 0;
            T.hit(fn, true, line);
        } else if (fn == "res") {
            const auto& r = pb->res;
            T.hit(fn, same(r.value, w[1]) && o(r.source) == w[2] && same(r.index, w[3])
                      && same(r.parity, w[4]) && same(r.index_age_s, w[5])
                      && o(r.market_status) == w[6] && o(r.reason) == w[7]
                      && same(r.divergence, w[8]) && same(r.divergence_pct, w[9])
                      && (r.alarm ? "1" : "0") == w[10], line);
        } else if (fn == "pr") {
            const auto* skf = pb->daily.skew();
            const std::string sks = !skf ? "None" : (skf->flat() ? "flat" : "fit");
            const std::string skr = skf ? o(skf->reason()) : "-";
            bool ok = same(pb->daily.spot(), w[1]) && sks == w[2] && skr == w[3]
                      && (pb->smoothed ? "1" : "0") == w[4] && (pb->hourly ? "1" : "0") == w[5];
            if (pb->hourly) ok = ok && same(pb->hourly->spot(), w[6]) && same(pb->hourly->w_4pm(), w[7]);
            else ok = ok && w[6] == "None" && w[7] == "None";
            T.hit(fn, ok, line);
        } else if (fn == "fair") {
            if (fair_i >= fair_list.size()) { T.hit(fn, false, line); continue; }
            const auto& [tk, bf] = *fair_list[fair_i++];
            const auto& d = bf.bracket();
            bool ok = tk == w[1] && (bf.hourly ? "1" : "0") == w[2] && same(bf.fair, w[3])
                      && o(d.reason) == w[4] && o(d.prob_used) == w[5] && same(d.p_above_lo, w[6])
                      && same(d.p_above_hi, w[7]);
            if (bf.hourly)
                ok = ok && same(bf.hour.w, w[8]) && same(bf.hour.var_ratio, w[9])
                     && same(bf.hour.forward, w[10]) && same(bf.hour.minutes_to_settlement, w[11])
                     && o(bf.hour.forward_source) == w[12] && same(bf.hour.index_minus_parity, w[13])
                     && same(bf.hour.parity_forward, w[14]) && o(bf.hour.atm_reason) == w[15];
            else
                ok = ok && w[8] == "None" && w[9] == "None" && w[10] == "None" && w[11] == "None"
                     && w[12] == "-" && w[13] == "None" && w[14] == "None" && w[15] == "-";
            if (!ok && std::getenv("KP_DEBUG"))
                std::printf("DBG %s fair=%a reason=%s lo=%a w=%a vr=%a fwd=%a mts=%a\n", tk.c_str(),
                            bf.fair.value_or(-1), d.reason.c_str(), d.p_above_lo.value_or(-1),
                            bf.hour.w.value_or(-1), bf.hour.var_ratio.value_or(-1),
                            bf.hour.forward.value_or(-1), bf.hour.minutes_to_settlement.value_or(-1));
            T.hit(fn, ok, line);
        } else if (fn == "sdet") {
            T.hit(fn, sk && J(sk->detail().to_json()) == parts[1], line);
        } else {
            T.hit("unknown_tag:" + fn, false, line);
        }
    }
    if (pb) T.hit("fair_count", fair_i == fair_list.size(), "end");
    return T.report() | rc;
}
