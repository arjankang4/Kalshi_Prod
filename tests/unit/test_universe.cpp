// Replays tests/data/golden/universe.vec against kp::pricing::universe.
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "kp/pricing/universe.hpp"
#include "kp/pycompat/round.hpp"

#include "boards.hpp"
#include "../harness.hpp"

namespace un = kp::pricing::universe;
namespace th = kp::pricing::theo;
using namespace kp::test;

namespace {

struct Tally { long n = 0, bad = 0; std::string worst; };
void hit(Tally& t, bool ok, const std::string& line) {
    ++t.n;
    if (!ok) { ++t.bad; if (t.worst.empty()) t.worst = line; }
}

// The same synthetic boards gen_universe builds, rebuilt from (n, step) so the vector does not
// have to carry 700 market rows per case.
std::vector<th::Market> board(int n, double step) {
    std::vector<th::Market> ms;
    ms.reserve((std::size_t)n);
    for (int i = 0; i < n; ++i) {
        th::Market m;
        m.ticker = "T" + std::to_string(i);
        m.strike_type = "between";
        m.floor_strike = 7000.0 + step * i;
        m.cap_strike = 7000.0 + step * (i + 1);
        ms.push_back(m);
    }
    return ms;
}

std::vector<double> grid_for(int n, double step) {
    std::vector<double> ks;
    const int m = (int)((step * n + 1000.0) / 5.0);
    ks.reserve((std::size_t)m);
    for (int i = 0; i < m; ++i) ks.push_back(7000.0 - 500.0 + 5.0 * i);
    return ks;
}

bool same_list(const std::vector<double>& got, std::istringstream& ss, int n) {
    if ((int)got.size() != n) return false;
    for (int i = 0; i < n; ++i) {
        std::string t; ss >> t;
        if (!same_bits(got[(std::size_t)i], hx(t))) return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    kp::pycompat::assert_c_numeric_locale();
    const std::string dir = (argc > 1) ? argv[1] : "tests/data/golden";

    std::ifstream in(dir + "/universe.vec");
    if (!in) { check("open universe.vec", false, dir); return report("test_universe"); }
    std::string header;
    std::getline(in, header);
    check("header names pricing/universe.py",
          header.find("source=pricing/universe.py") != std::string::npos);

    const std::vector<double> band_grid = [] {
        std::vector<double> g;
        for (int i = 0; i < 300; ++i) g.push_back(7000.0 + 5.0 * i);
        return g;
    }();

    std::map<std::string, Tally> T;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string fn;
        ss >> fn;
        if (fn == "occ") {
            std::string root, exp, r, k, want;
            ss >> root >> exp >> r >> k >> want;
            hit(T[fn], un::occ_ticker(root, exp, r[0], hx(k)) == want, line);
        } else if (fn == "parity_band") {
            std::string centre, bp; int n;
            ss >> centre >> bp >> n;
            const auto b = un::parity_band(band_grid, opt(centre), opt(bp));
            const std::vector<double> got(b.begin(), b.end());
            hit(T[fn], same_list(got, ss, n), line);
        } else if (fn == "boundaries") {
            std::string step; int n, cnt;
            ss >> n >> step >> cnt;
            hit(T[fn], same_list(th::boundaries(board(n, hx(step))), ss, cnt), line);
        } else if (fn == "desired") {
            std::string step, spot, bp; int n, cap, cnt;
            ss >> n >> step >> cap >> spot >> bp >> cnt;
            const double st = hx(step);
            const auto d = un::desired_tickers(board(n, st), grid_for(n, st), "SPXW", "260819",
                                               opt(spot), opt(bp), cap);
            bool ok = same_list(d.strikes, ss, cnt);
            // The ticker list must be C-then-P per strike, strikes ascending -- the order reaches
            // the subscribe frame, and Massive acks per ticker.
            if (ok) {
                ok = d.tickers.size() == d.strikes.size() * 2;
                for (std::size_t i = 0; ok && i < d.strikes.size(); ++i)
                    ok = d.tickers[2 * i] == un::occ_ticker("SPXW", "260819", 'C', d.strikes[i]) &&
                         d.tickers[2 * i + 1] == un::occ_ticker("SPXW", "260819", 'P', d.strikes[i]);
            }
            hit(T[fn], ok, line);
        } else {
            std::getline(ss, fn);
        }
    }

    section("pricing/universe golden vectors");
    for (const auto& [k, t] : T) {
        char b[400];
        if (t.bad == 0) { std::snprintf(b, sizeof b, "%s (%ld cases)", k.c_str(), t.n); check(b, true); }
        else { std::snprintf(b, sizeof b, "%ld/%ld mismatched; first: %s", t.bad, t.n, t.worst.c_str()); check(k, false, b); }
    }

    section("the worked examples, asserted directly");
    {
        check("the docstring's own ticker",
              un::occ_ticker("SPXW", "260819", 'C', 7700.0) == "O:SPXW260819C07700000",
              un::occ_ticker("SPXW", "260819", 'C', 7700.0));
        // Eight digits, zero-padded -- a 1000-strike would otherwise shift every later field.
        check("a low strike still pads to 8",
              un::occ_ticker("SPXW", "260819", 'P', 5.0) == "O:SPXW260819P00005000");
        // round() is half-to-even, and a .0005 strike lands EXACTLY on .5 after *1000 (verified:
        // 7700.0005 * 1000 == 7700000.5 with no representation slack). std::lround is
        // half-away-from-zero and would take that to 7700001 -- a contract that does not exist.
        //
        // Both .0005 and .0015 are exact halves, and they round in OPPOSITE directions, which is
        // what makes this half-to-even rather than a truncation: ...000.5 -> ...000 (down, to the
        // even) and ...001.5 -> ...002 (up, to the even). A shim that got only the first case
        // right would look correct on the strike anyone tries by hand.
        check("half-to-even rounds .0005 DOWN to the even",
              un::occ_ticker("SPXW", "260819", 'C', 7700.0005) == "O:SPXW260819C07700000",
              un::occ_ticker("SPXW", "260819", 'C', 7700.0005));
        check("and .0015 UP to the even",
              un::occ_ticker("SPXW", "260819", 'C', 7700.0015) == "O:SPXW260819C07700002",
              un::occ_ticker("SPXW", "260819", 'C', 7700.0015));

        // A boundary off the end of the grid is a market we cannot price. It must throw rather
        // than return a short set -- a silently unpriced market looks exactly like a quiet one.
        std::vector<th::Market> ms(1);
        ms[0].strike_type = "between";
        ms[0].floor_strike = 99999.0;
        ms[0].cap_strike = 99999.0;
        bool threw = false;
        try { un::bracketing_strikes(ms, {7000.0, 7005.0}); } catch (const std::exception&) { threw = true; }
        check("an unbracketable boundary throws", threw);

        // The band is a PERCENT, not a strike count. 2% of 7700 is +-154, so on a 5-wide grid
        // that is 61 strikes; the same count on a 29,400-level NDX grid would be a quarter of the
        // coverage. This is the property that makes the band travel between boards.
        const auto b = un::parity_band(band_grid, 7700.0, 0.02);
        check("band low edge", *b.begin() >= 7700.0 * 0.98);
        check("band high edge", *b.rbegin() <= 7700.0 * 1.02);
    }
    return report("test_universe");
}
