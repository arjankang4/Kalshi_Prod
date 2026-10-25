// Replays tests/data/golden/band.vec against kp::strategy::band.
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>

#include "kp/config.hpp"
#include "kp/pycompat/round.hpp"
#include "kp/strategy/band.hpp"

#include "boards.hpp"
#include "../harness.hpp"

namespace bd = kp::strategy::band;
namespace th = kp::pricing::theo;
using namespace kp::test;
using OptD = std::optional<double>;

namespace {
struct Tally { long n = 0, bad = 0; std::string worst; };
void hit(Tally& t, bool ok, const std::string& line) {
    ++t.n;
    if (!ok) { ++t.bad; if (t.worst.empty()) t.worst = line; }
}
th::Market mk(const std::string& styp, const std::string& lo, const std::string& hi,
              const std::string& ticker = "T") {
    th::Market m;
    m.ticker = ticker;
    m.strike_type = (styp == "-") ? std::string() : styp;
    m.floor_strike = opt(lo);
    m.cap_strike = opt(hi);
    return m;
}
std::string join(const std::vector<std::string>& v) {
    std::string s;
    for (std::size_t i = 0; i < v.size(); ++i) { if (i) s += ","; s += v[i]; }
    return s;
}
std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> v;
    std::string cur;
    for (char c : s) { if (c == ',') { v.push_back(cur); cur.clear(); } else cur += c; }
    if (!cur.empty()) v.push_back(cur);
    return v;
}
}  // namespace

int main(int argc, char** argv) {
    kp::pycompat::assert_c_numeric_locale();
    const std::string dir = (argc > 1) ? argv[1] : "tests/data/golden";

    std::ifstream in(dir + "/band.vec");
    if (!in) { check("open band.vec", false, dir); return report("test_band"); }
    std::string header;
    std::getline(in, header);
    check("header names strategy/band.py",
          header.find("source=strategy/band.py") != std::string::npos);

    std::map<std::string, Tally> T;
    std::string line;
    // classify cases arrive one market per line, so the board is rebuilt once and reused.
    std::vector<th::Market> board;
    std::map<std::string, bd::Classified> cl0, cl1;
    bool built = false;
    auto build_board = [&]() {
        const double F = 7691.1, w = 0.00244;
        std::map<std::string, OptD> em;
        std::set<std::string> incs;
        int i = 0;
        for (int off = -200; off <= 200; off += 5, ++i) {
            th::Market m = mk("greater", "", "", "K" + std::to_string(i));
            m.floor_strike = F + off;
            m.cap_strike = std::nullopt;
            board.push_back(m);
            em[m.ticker] = (i % 7) ? OptD(w) : std::nullopt;
            if (i % 3 == 0) incs.insert(m.ticker);
        }
        cl0 = bd::classify(board, F, em, {}).first;
        cl1 = bd::classify(board, F, em, incs).first;
        built = true;
    };

    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string fn;
        ss >> fn;
        if (fn == "strike") {
            std::string st, lo, hi, want;
            ss >> st >> lo >> hi >> want;
            hit(T[fn], same_opt(bd::market_strike(mk(st, lo, hi)), want), line);
        } else if (fn == "distance") {
            std::string st, lo, hi, fwd, want;
            ss >> st >> lo >> hi >> fwd >> want;
            hit(T[fn], same_opt(bd::distance(mk(st, lo, hi), opt(fwd)), want), line);
        } else if (fn == "width") {
            std::string fwd, w, sig, want;
            ss >> fwd >> w >> sig >> want;
            hit(T[fn], same_opt(bd::band_width(opt(fwd), opt(w), opt(sig)), want), line);
        } else if (fn == "in_band") {
            std::string st, lo, hi, fwd, w, sig, xm, reason, d_, hw, lim, sg, bf;
            int inc, ok;
            ss >> st >> lo >> hi >> fwd >> w >> sig >> inc >> xm >> ok >> reason
               >> d_ >> hw >> lim >> sg >> bf;
            const auto [got, det] = bd::in_band(mk(st, lo, hi), opt(fwd), opt(w), opt(sig),
                                                inc != 0, opt(xm));
            const std::string gr = det.reason.empty() ? "-" : det.reason;
            hit(T[fn], (got ? 1 : 0) == ok && gr == reason && same_opt(det.distance, d_) &&
                       same_opt(det.half_width, hw) && same_opt(det.limit, lim) &&
                       same_opt(det.sigmas, sg) && same_opt(det.band_frac, bf), line);
        } else if (fn == "counts") {
            int inb; std::string want;
            ss >> inb >> want;
            hit(T[fn], join(bd::counts_for(inb != 0)) == want, line);
        } else if (fn == "counts_custom") {
            int inb; std::string given, want;
            ss >> inb >> given >> want;
            const std::vector<std::string> c = split(given);
            hit(T[fn], join(bd::counts_for(inb != 0, &c)) == want, line);
        } else if (fn == "poslimit") {
            int inb; std::string want;
            ss >> inb >> want;
            hit(T[fn], same_bits(bd::position_limit(inb != 0), hx(want)), line);
        } else if (fn == "poslimit_lot") {
            int inb, lot; std::string want;
            ss >> inb >> lot >> want;
            hit(T[fn], same_bits(bd::position_limit(inb != 0, lot), hx(want)), line);
        } else if (fn == "classify_summary") {
            long nincs, n, nb, nw; std::string sig, xm;
            ss >> nincs >> n >> nb >> nw >> sig >> xm;
            if (!built) build_board();
            const double F = 7691.1, w = 0.00244;
            std::map<std::string, OptD> em;
            std::set<std::string> incs;
            for (std::size_t i = 0; i < board.size(); ++i) {
                em[board[i].ticker] = (i % 7) ? OptD(w) : std::nullopt;
                if (nincs > 0 && i % 3 == 0) incs.insert(board[i].ticker);
            }
            const auto [_c, s] = bd::classify(board, F, em, incs);
            hit(T[fn], s.n == n && s.band == nb && s.wing == nw &&
                       same_bits(s.sigma, hx(sig)) && same_bits(s.exit_mult, hx(xm)), line);
        } else if (fn == "classify") {
            long nincs; std::string tk, counts, pl, reason;
            int inb;
            ss >> nincs >> tk >> inb >> counts >> pl >> reason;
            if (!built) build_board();
            const auto& c = (nincs > 0 ? cl1 : cl0).at(tk);
            const std::string gr = c.detail.reason.empty() ? "-" : c.detail.reason;
            hit(T[fn], (c.inband ? 1 : 0) == inb && join(c.counts) == counts &&
                       same_bits(c.position_limit, hx(pl)) && gr == reason, line);
        } else if (fn == "resolve") {
            std::string kind, tk, want;
            ss >> kind >> tk >> want;
            const std::map<std::string, std::vector<std::string>> per = {
                {"A", {"1.00"}}, {"B", {}}, {"C", {"2.00", "3.00"}}};
            const std::vector<std::string> glob = {"7.00", "8.00"};
            std::vector<std::string> got;
            if (kind == "map") got = bd::resolve_counts(&per, nullptr, tk);
            else if (kind == "glob") got = bd::resolve_counts(nullptr, &glob, tk);
            else got = bd::resolve_counts(nullptr, nullptr, tk);
            hit(T[fn + ":" + kind], join(got) == want, line);
        } else {
            std::getline(ss, fn);
        }
    }

    section("strategy/band golden vectors");
    for (const auto& [k, t] : T) {
        char b[500];
        if (t.bad == 0) { std::snprintf(b, sizeof b, "%s (%ld cases)", k.c_str(), t.n); check(b, true); }
        else { std::snprintf(b, sizeof b, "%ld/%ld mismatched; first: %s", t.bad, t.n, t.worst.c_str()); check(k, false, b); }
    }

    section("the traps");
    {
        const double F = 7691.1, w = 0.00244;
        const OptD half = bd::band_width(F, w);
        check("one sigma is 18.8 index points on the 2026-08-21 board",
              half.has_value() && std::fabs(*half / kp::config::BAND_SIGMA - 18.766) < 0.01,
              half ? std::to_string(*half / kp::config::BAND_SIGMA) : "None");

        // THE MEASUREMENT THAT JUSTIFIES SIGMA OVER STRIKES. +-3 strikes is a completely
        // different band on the two boards of the SAME underlying, which is the entire argument
        // for this module existing.
        const double one_sigma = *half / kp::config::BAND_SIGMA;
        check("+-3 strikes on the 25-wide daily board is 3.99 sigma",
              std::fabs(75.0 / one_sigma - 3.99) < 0.02);
        check("+-3 strikes on the 5-wide hourly board is 0.80 sigma",
              std::fabs(15.0 / one_sigma - 0.80) < 0.02);

        // HYSTERESIS, from both sides. A market just outside the entry boundary is OUT when new
        // and IN when incumbent -- that difference is the whole point of BAND_EXIT_MULT, and a
        // port that ignored the flag would still pass every non-incumbent vector.
        th::Market m;
        m.ticker = "X";
        m.strike_type = "greater";
        m.floor_strike = F + *half * 1.1;
        static_assert(kp::config::BAND_EXIT_MULT > 1.1, "the entry-side probe below must sit inside the exit boundary");
        check("just outside the entry boundary, a NEW market gets one rung",
              !bd::in_band(m, F, w, std::nullopt, false).first);
        check("...but an INCUMBENT keeps its three",
              bd::in_band(m, F, w, std::nullopt, true).first);
        m.floor_strike = F + *half * kp::config::BAND_EXIT_MULT * 1.05;   // past the exit boundary too
        check("past the exit boundary even an incumbent is dropped",
              !bd::in_band(m, F, w, std::nullopt, true).first);

        // FAILS CLOSED. Depth is the expensive choice and an unknown does not earn it.
        m.floor_strike = F;
        check("no w means one rung, not three",
              !bd::in_band(m, F, std::nullopt, std::nullopt, false).first &&
              bd::in_band(m, F, std::nullopt, std::nullopt, false).second.reason == "no_width");
        th::Market bad;
        bad.strike_type = "unhandled";
        check("an unpriceable strike type means one rung",
              !bd::in_band(bad, F, w, std::nullopt, false).first &&
              bd::in_band(bad, F, w, std::nullopt, false).second.reason == "no_strike");
        check("...and an incumbent flag cannot rescue it",
              !bd::in_band(bad, F, w, std::nullopt, true).first);

        // A `between` market measures from the bracket MIDPOINT, not from either boundary.
        th::Market b;
        b.strike_type = "between";
        b.floor_strike = 7675.0;
        b.cap_strike = 7699.9999;
        const OptD ks = bd::market_strike(b);
        check("a between market's moneyness is the bracket midpoint",
              ks.has_value() && std::fabs(*ks - 7687.5) < 1e-9,
              ks ? std::to_string(*ks) : "None");
        // 7699.9999 normalises to 7700 first, so the midpoint is 7687.5 and not 7687.49995.
        check("...measured AFTER boundary normalisation collapses 7699.9999 to 7700",
              ks.has_value() && same_bits(*ks, 0.5 * (7675.0 + 7700.0)));

        // The wing rung is the SAME SIZE as a band rung -- it is a slice of one tuple, not a
        // second tuple, because the lot is the unit the inventory limits are counted in.
        check("the wing rung is the band's first rung, not a smaller one",
              bd::counts_for(false).size() == 1 &&
              bd::counts_for(false)[0] == bd::counts_for(true)[0]);
        check("in-band is the full ladder",
              bd::counts_for(true).size() == kp::config::QUOTE_TIER_COUNTS.size());
        // LOT_SIZE x POSITION_LIMIT_LOTS_{BAND,WING}: 10 x 4 and 10 x 2.
        check("and the band buys four lots of inventory against the wing's two",
              same_bits(bd::position_limit(true), 40.0) &&
              same_bits(bd::position_limit(false), 20.0));
    }

    return report("test_band");
}
