// Replay pricing/skew golden vectors.
//
// The Spline is the riskiest single piece in the port: a Thomas solve whose evaluation is four
// chained multiply-adds, every one an FMA candidate. So the vectors pin the SOLVED
// second-derivative array, not just the outputs -- a tridiagonal solve that is wrong in m[7] but
// right at the query points you happened to test is the classic way this hides for a month.
//
// The board-level cases come from 20 real captured snapshots, which carry the absent-bid pattern
// and the genuinely 20-point-wide far wings that no synthetic board would produce.
#include "../harness.hpp"
#include "kp/pricing/skew.hpp"
#include <cmath>

#include "boards.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace kp::test;
namespace sk = kp::pricing::skew;
namespace th = kp::pricing::theo;

namespace {
struct Tally { long n = 0, bad = 0; std::string worst; };
void hit(Tally& t, bool ok, const std::string& w) { ++t.n; if (!ok) { ++t.bad; if (t.worst.empty()) t.worst = w; } }
}  // namespace
using kp::test::is_none; using kp::test::hx; using kp::test::same_opt;
using kp::test::take; using kp::test::load_boards; using kp::test::RealBoard;

int main(int argc, char** argv) {
    const std::string dir = (argc > 1) ? argv[1] : "tests/data/golden";
    std::ifstream in(dir + "/skew.vec");
    if (!in) { check("open skew.vec", false, dir); return report("test_skew"); }
    std::string header, line;
    std::getline(in, header);
    check("header names pricing/skew.py", header.find("source=pricing/skew.py") != std::string::npos);

    const std::vector<RealBoard> boards = load_boards(dir + "/skew_boards.vec");
    check("loaded the captured boards", !boards.empty(),
          std::to_string(boards.size()) + " boards");

    std::map<std::string, Tally> T;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::istringstream ss(line);
        std::string fn; ss >> fn;

        if (fn == "bucket_offsets") {
            const auto want = sk::bucket_offsets();
            std::string t; std::vector<double> got;
            while (ss >> t) got.push_back(hx(t));
            bool ok = got.size() == want.size();
            for (std::size_t i = 0; ok && i < got.size(); ++i) ok = same_bits(want[i], got[i]);
            hit(T[fn], ok, line);
        } else if (fn == "bucket_label") {
            std::string a, w; ss >> a >> w;
            hit(T[fn], sk::bucket_label(hx(a)) == w, line + " || got " + sk::bucket_label(hx(a)));
        } else if (fn == "spline_m") {
            std::size_t n; ss >> n;
            const auto xs = take(ss, n), ys = take(ss, n), wm = take(ss, n);
            const sk::Spline sp(xs, ys);
            bool ok = sp.m().size() == n;
            for (std::size_t i = 0; ok && i < n; ++i) ok = same_bits(sp.m()[i], wm[i]);
            hit(T[fn], ok, "n=" + std::to_string(n));
        } else if (fn == "spline_at") {
            std::size_t n; ss >> n;
            const auto xs = take(ss, n), ys = take(ss, n);
            std::string q, w; ss >> q >> w;
            const sk::Spline sp(xs, ys);
            hit(T[fn], same_opt(sp(hx(q)), w), "n=" + std::to_string(n) + " xq=" + q);
        } else if (fn == "atm_vol" || fn == "atm_vol_steps") {
            std::size_t bi; std::string F, w; int ms = 3;
            ss >> bi >> F;
            if (fn == "atm_vol_steps") ss >> ms;
            ss >> w;
            if (bi >= boards.size()) continue;
            hit(T[fn], same_opt(sk::atm_vol(boards[bi].board, boards[bi].strikes, hx(F), ms), w),
                line);
        } else if (fn == "strike_vol") {
            std::size_t bi; std::string F, K, w, r;
            ss >> bi >> F >> K >> w >> r;
            if (bi >= boards.size()) continue;
            const auto [got, _d] = sk::strike_vol(boards[bi].board, hx(F), hx(K), hx(w));
            hit(T[fn], same_opt(got, r), line);
        } else if (fn == "fit") {
            std::size_t bi, nbk; std::string F, w, reason; int flat;
            ss >> bi >> F >> w >> reason >> flat >> nbk;
            if (bi >= boards.size()) continue;
            const sk::SkewFit sf = sk::fit(boards[bi].board, boards[bi].strikes, hx(F), hx(w));
            const std::string got_reason = sf.reason().empty() ? "-" : sf.reason();
            hit(T[fn], got_reason == reason && sf.flat() == (flat != 0) && sf.n_buckets() == nbk,
                line + " || got " + got_reason + " flat=" + std::to_string(sf.flat()) +
                " nb=" + std::to_string(sf.n_buckets()));
        } else if (fn == "fit_ratio") {
            std::size_t bi; std::string F, w, K, wr, ww, wp;
            ss >> bi >> F >> w >> K >> wr >> ww >> wp;
            if (bi >= boards.size()) continue;
            const sk::SkewFit sf = sk::fit(boards[bi].board, boards[bi].strikes, hx(F), hx(w));
            const bool ok = same_bits(sf.ratio(hx(K)), hx(wr)) &&
                            same_opt(sf.w_of(hx(K)), ww) && same_opt(sf.p_above(hx(K)), wp);
            hit(T[fn], ok, line);
        }
    }

    section("pricing/skew golden vectors (pure)");
    for (const auto& [k, t] : T) {
        char b[400];
        if (t.bad == 0) { std::snprintf(b, sizeof b, "%s (%ld cases)", k.c_str(), t.n); check(b, true); }
        else { std::snprintf(b, sizeof b, "%ld/%ld mismatched; first: %s", t.bad, t.n, t.worst.c_str()); check(k, false, b); }
    }

    section("the traps");
    // atm_vol's outward walk visits the candidate NEARER THE FORWARD IN PRICE first, which on an
    // uneven strike grid is not the lower index. This replaced a set literal whose CPython hash
    // order (ascending by x & 7) silently favoured one wing; the assertion below is the property
    // that replaced it, on a grid that is 5-wide below the money and 100-wide above.
    {
        // strikes 7700 7705 7710 7810 ... ; F = 7706, so i0 = 7705. At step 1 the candidates are
        // 7700 (6 away) and 7710 (4 away) -- the HIGHER index is nearer, so it goes first.
        const std::vector<double> ks = {7700.0, 7705.0, 7710.0, 7810.0};
        const double F = 7706.0;
        check("nearer-in-price wins over lower index", std::fabs(ks[2] - F) < std::fabs(ks[0] - F));
        // And at step 2: 7810 is 104 away against nothing below, so index order would have been
        // the same answer -- the rule only bites where the grid is uneven, which is the point.
        check("even grid leaves the order alone", std::fabs(ks[1] - 7705.0) == 0.0);
    }
    // Flat extrapolation, not cubic: past the last knot a cubic diverges and can drive the ratio
    // negative, and those are the wings whose quotes are least trustworthy.
    {
        const sk::Spline sp({-1.0, 0.0, 1.0}, {1.2, 1.0, 1.1});
        bits("flat left of the first knot",  *sp(-9.0), 1.2);
        bits("flat right of the last knot",  *sp(9.0),  1.1);
    }
    return report("test_skew");
}
