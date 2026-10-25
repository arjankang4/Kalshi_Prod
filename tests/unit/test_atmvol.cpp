// Replay pricing/atmvol golden vectors against the 20 real captured boards.
//
// The interesting failure modes here are ORDERING, not arithmetic. cs.sort() is a lexicographic
// sort over 5-tuples whose first element is |C - P|, a difference of two cent-grid prices, so
// ties are the norm rather than the exception -- and the tie-break picks the anchor strike, hence
// the forward, hence every theo on the board. The nearest-25 cut is a stable sort whose keys tie
// for every pair straddling the centre.
#include "../harness.hpp"
#include "kp/pricing/atmvol.hpp"
#include "boards.hpp"

#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>

using namespace kp::test;
namespace av = kp::pricing::atmvol;

namespace {
struct Tally { long n = 0, bad = 0; std::string worst; };
void hit(Tally& t, bool ok, const std::string& w) { ++t.n; if (!ok) { ++t.bad; if (t.worst.empty()) t.worst = w; } }
}

int main(int argc, char** argv) {
    const std::string dir = (argc > 1) ? argv[1] : "tests/data/golden";
    const auto boards = kp::test::load_boards(dir + "/skew_boards.vec");
    check("loaded the captured boards", !boards.empty(), std::to_string(boards.size()) + " boards");

    std::ifstream in(dir + "/atmvol.vec");
    if (!in) { check("open atmvol.vec", false, dir); return report("test_atmvol"); }
    std::string header, line;
    std::getline(in, header);
    check("header names pricing/atmvol.py",
          header.find("source=pricing/atmvol.py") != std::string::npos);

    std::map<std::string, Tally> T;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::istringstream ss(line);
        std::string fn; ss >> fn;
        std::size_t bi; ss >> bi;
        if (bi >= boards.size()) continue;
        const auto& B = boards[bi];

        if (fn == "two_sided") {
            std::string r, k; int w1, w2; ss >> r >> k >> w1 >> w2;
            auto it = B.board.find({r[0], hx(k)});
            const kp::pricing::theo::Quote& q =
                (it == B.board.end()) ? kp::test::empty_quote() : it->second;
            hit(T[fn], av::two_sided(q) == (w1 != 0) && av::two_sided(q, false) == (w2 != 0), line);
        } else if (fn == "combos") {
            std::size_t n; ss >> n;
            const auto got = av::combos(B.board, B.strikes);
            bool ok = got.size() == n;
            for (std::size_t i = 0; ok && i < n; ++i) {
                std::string tok; ss >> tok;
                const auto f = kp::test::split(tok, ':');
                ok = f.size() == 5 && same_bits(got[i].abs_cp, hx(f[0]))
                     && same_bits(got[i].strike, hx(f[1])) && same_bits(got[i].implied_s, hx(f[2]))
                     && same_bits(got[i].c, hx(f[3])) && same_bits(got[i].p, hx(f[4]));
            }
            hit(T[fn], ok, "board " + std::to_string(bi));
        } else if (fn == "grid_step") {
            std::string c, w; ss >> c >> w;
            hit(T[fn], same_bits(av::grid_step(B.strikes, hx(c)), hx(w)), line);
        } else if (fn == "parity_forward") {
            std::string F, reason, ak, nf, acp; int np;
            ss >> F >> reason >> ak >> nf >> np >> acp;
            const auto [got, d] = av::parity_forward(B.board, B.strikes);
            const std::string gr = d.reason.empty() ? "-" : d.reason;
            hit(T[fn], kp::test::same_opt(got, F) && gr == reason
                       && kp::test::same_opt(d.anchor_strike, ak)
                       && kp::test::same_opt(d.naive_forward, nf) && d.n_parity == np
                       && kp::test::same_opt(d.anchor_cp, acp), line);
        } else if (fn == "atm_total_vol") {
            std::string F, w, reason, cp, disp, wa; int k, nv;
            ss >> F >> k >> w >> reason >> nv >> cp >> disp >> wa;
            const auto [got, d] = av::atm_total_vol(B.board, B.strikes, hx(F), k);
            const std::string gr = d.reason.empty() ? "-" : d.reason;
            hit(T[fn], kp::test::same_opt(got, w) && gr == reason && d.n_vol == nv
                       && kp::test::same_opt(d.cp_spread, cp)
                       && kp::test::same_opt(d.dispersion, disp)
                       && kp::test::same_opt(d.w_anchor, wa), line);
        } else if (fn == "fit") {
            int gate, okw; std::string idxf, F, w, reason, src, cp, disp, imp;
            ss >> gate >> idxf >> F >> w >> reason >> src >> okw >> cp >> disp >> imp;
            const auto a = av::fit(B.board, B.strikes, gate != 0, kp::test::opt(idxf));
            const std::string gr = a.reason.empty() ? "-" : a.reason;
            const std::string gs = a.forward_source.empty() ? "-" : a.forward_source;
            hit(T[fn], kp::test::same_opt(a.forward, F) && kp::test::same_opt(a.w, w)
                       && gr == reason && gs == src && a.ok() == (okw != 0)
                       && kp::test::same_opt(a.cp_spread, cp)
                       && kp::test::same_opt(a.dispersion, disp)
                       && kp::test::same_opt(a.index_minus_parity, imp),
                line + " || got F=" + (a.forward ? "y" : "n") + " reason=" + gr + " src=" + gs);
        }
    }

    section("pricing/atmvol golden vectors");
    for (const auto& [k, t] : T) {
        char b[400];
        if (t.bad == 0) { std::snprintf(b, sizeof b, "%s (%ld cases)", k.c_str(), t.n); check(b, true); }
        else { std::snprintf(b, sizeof b, "%ld/%ld mismatched; first: %s", t.bad, t.n, t.worst.c_str()); check(k, false, b); }
    }
    return report("test_atmvol");
}
