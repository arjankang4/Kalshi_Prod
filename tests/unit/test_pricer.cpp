// Replays tests/data/golden/pricer.vec against kp::pricing::pricer::Pricer.
//
// The boards come from skew_boards.vec by index, exactly as the generator drove them -- a Pricer
// case would otherwise have to carry ~300 option legs inline.
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "kp/pricing/pricer.hpp"
#include "kp/pycompat/round.hpp"

#include "boards.hpp"
#include "../harness.hpp"

namespace pz = kp::pricing::pricer;
namespace th = kp::pricing::theo;
using namespace kp::test;

namespace {

// Two optionals hold the same value, bit for bit, or are both absent.
bool same_opt_pair(const std::optional<double>& a, const std::optional<double>& b) {
    if (a.has_value() != b.has_value()) return false;
    return !a.has_value() || same_bits(*a, *b);
}

struct Tally { long n = 0, bad = 0; std::string worst; };
void hit(Tally& t, bool ok, const std::string& line) {
    ++t.n;
    if (!ok) { ++t.bad; if (t.worst.empty()) t.worst = line; }
}

}  // namespace

int main(int argc, char** argv) {
    kp::pycompat::assert_c_numeric_locale();
    const std::string dir = (argc > 1) ? argv[1] : "tests/data/golden";

    const std::vector<RealBoard> boards = load_boards(dir + "/skew_boards.vec");
    check("loaded the captured boards", !boards.empty(),
          std::to_string(boards.size()) + " boards");

    std::ifstream in(dir + "/pricer.vec");
    if (!in) { check("open pricer.vec", false, dir); return report("test_pricer"); }
    std::string header;
    std::getline(in, header);
    check("header names pricing/theo.py", header.find("source=pricing/theo.py") != std::string::npos);

    // One Pricer per (board, spot) so the lazy skew fit is built once, as it is in Python where
    // the same object is reused across every strike of a case.
    std::map<std::string, pz::Pricer> cache;
    auto pricer_for = [&](int bi, const std::string& spot_tok) -> pz::Pricer& {
        const std::string key = std::to_string(bi) + "|" + spot_tok;
        const auto it = cache.find(key);
        if (it != cache.end()) return it->second;
        const RealBoard& b = boards.at((std::size_t)bi);
        return cache.emplace(key, pz::Pricer(b.board, b.strikes, opt(spot_tok))).first->second;
    };

    std::map<std::string, Tally> T;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string fn;
        ss >> fn;
        if (fn == "parity") {
            int bi, strict;
            std::string width, want;
            ss >> bi >> strict >> width >> want;
            const RealBoard& b = boards.at((std::size_t)bi);
            const pz::Pricer p(b.board, b.strikes);
            hit(T[fn], same_opt(p.parity_spot(hx(width), strict != 0), want), line);
        } else if (fn == "spot") {
            int bi;
            std::string sp, want;
            ss >> bi >> sp >> want;
            hit(T[fn], same_opt(pricer_for(bi, sp).spot(), want), line);
        } else if (fn == "implied") {
            int bi;
            std::string sp, k, w, wc, wp, leg;
            ss >> bi >> sp >> k >> w >> wc >> wp >> leg;
            const auto [got, d] = pricer_for(bi, sp).implied_w_at(hx(k));
            const std::string got_leg = d.w_leg ? std::string(1, d.w_leg) : "-";
            hit(T[fn], same_opt(got, w) && same_opt(d.w_call, wc) && same_opt(d.w_put, wp) &&
                       got_leg == leg, line);
        } else if (fn == "digital_delta") {
            int bi;
            std::string sp, k, want;
            ss >> bi >> sp >> k >> want;
            hit(T[fn], same_opt(pricer_for(bi, sp).digital_above_delta(hx(k)), want), line);
        } else if (fn == "digital") {
            int bi;
            std::string sp, k, want;
            ss >> bi >> sp >> k >> want;
            hit(T[fn], same_opt(pricer_for(bi, sp).digital_above(hx(k)), want), line);
        } else if (fn == "market_fair") {
            std::string styp, lo, hi, want;
            ss >> styp >> lo >> hi >> want;
            th::Market m;
            m.strike_type = (styp == "-") ? std::string() : styp;
            m.floor_strike = opt(lo);
            m.cap_strike = opt(hi);
            hit(T[fn], same_opt(pricer_for(0, "None").market_fair(m).first, want), line);
        } else {
            std::getline(ss, fn);
        }
    }

    section("pricing/theo Pricer golden vectors");
    for (const auto& [k, t] : T) {
        char b[400];
        if (t.bad == 0) { std::snprintf(b, sizeof b, "%s (%ld cases)", k.c_str(), t.n); check(b, true); }
        else { std::snprintf(b, sizeof b, "%ld/%ld mismatched; first: %s", t.bad, t.n, t.worst.c_str()); check(k, false, b); }
    }

    section("the traps");
    {
        const RealBoard& b = boards.at(0);
        const pz::Pricer p(b.board, b.strikes);

        // strict IS the safety property, and both halves of that claim are asserted: the guard
        // must EXCLUDE the bad leg, and the unguarded form must actually be wrong without it.
        //
        // On the captured board, stripping a bid moves the loose answer by only 0.08 points --
        // the real 2026-08-19 failure needed the stripped strike to WIN the anchor, which needs
        // both its legs to read ~0.00. So the catastrophe is reproduced on a synthetic board
        // built to exactly that shape rather than asserted vaguely on one that cannot show it.
        {
            auto q = [](double bp, double ap, double bs, double as) {
                kp::pricing::theo::Quote x;
                x.present = true;
                x.bp = bp; x.ap = ap; x.bs = bs; x.as = as;
                return x;
            };
            const std::vector<double> ks = {7000.0, 7690.0, 7695.0};
            kp::pricing::skew::Board bd;
            // 7000: a deep-ITM call worth ~692 whose BID HAS GONE -- Massive omits bp/bs
            // entirely -- beside a worthless put. quote_value reads the call as 0.00, so
            // |C - P| is ~0 at this strike and it implies S = 7000, its own strike.
            kp::pricing::theo::Quote itm_call;
            itm_call.present = true;
            itm_call.ap = 700.0;
            itm_call.as = 5.0;                       // bp / bs absent
            bd[kp::pricing::skew::LegKey{'C', 7000.0}] = itm_call;
            bd[kp::pricing::skew::LegKey{'P', 7000.0}] = q(0.0, 0.0, 10.0, 10.0);
            // Two honest near-money strikes, both implying S = 7692.
            bd[kp::pricing::skew::LegKey{'C', 7690.0}] = q(4.9, 5.1, 10.0, 10.0);
            bd[kp::pricing::skew::LegKey{'P', 7690.0}] = q(2.9, 3.1, 10.0, 10.0);
            bd[kp::pricing::skew::LegKey{'C', 7695.0}] = q(2.4, 2.6, 10.0, 10.0);
            bd[kp::pricing::skew::LegKey{'P', 7695.0}] = q(5.4, 5.6, 10.0, 10.0);

            const pz::Pricer pw(bd, ks);
            const auto strict_v = pw.parity_spot(60.0, true);
            const auto loose_v = pw.parity_spot(60.0, false);
            check("strict parity ignores the bidless ITM leg",
                  strict_v.has_value() && std::fabs(*strict_v - 7692.0) < 1.0,
                  strict_v ? std::to_string(*strict_v) : "None");
            // THIS is the bug the guard exists for: without it the anchor collapses onto the
            // stripped strike and the whole board reprices off a forward 692 points out.
            check("without it the anchor collapses to the stripped strike",
                  loose_v.has_value() && std::fabs(*loose_v - 7000.0) < 1.0,
                  loose_v ? std::to_string(*loose_v) : "None");
        }

        // And on the captured board the guard is a no-op on a clean leg but not on a stripped
        // one -- i.e. strict is genuinely reading a different set, not just the same numbers.
        {
            kp::pricing::skew::Board qb = b.board;
            const double k_itm = b.strikes.front();
            auto it = qb.find(kp::pricing::skew::LegKey{'C', k_itm});
            if (it != qb.end()) {
                it->second.bp = std::nullopt;
                it->second.bs = std::nullopt;
                const pz::Pricer pw(qb, b.strikes);
                const auto clean = p.parity_spot(60.0, true);
                check("stripping a bid does not move the STRICT answer at all",
                      same_opt_pair(pw.parity_spot(60.0, true), clean));
                check("but it does move the loose one",
                      !same_opt_pair(pw.parity_spot(60.0, false), clean));
            }
        }

        // The OTM leg is the one used, on both sides of the money.
        if (p.spot()) {
            const double F = *p.spot();
            double above = 0, below = 0;
            for (double k : b.strikes) { if (k >= F && !above) above = k; if (k < F) below = k; }
            if (above) {
                const auto [w, d] = p.implied_w_at(above);
                check("at or above the forward the CALL is the OTM leg",
                      !w.has_value() || d.w_leg == 'C');
            }
            if (below) {
                const auto [w, d] = p.implied_w_at(below);
                check("below the forward the PUT is the OTM leg",
                      !w.has_value() || d.w_leg == 'P');
            }
        }

        // A boundary outside the listed grid has no bracketing strikes, so no interpolated vol.
        check("off the end of the grid does not price",
              !p.digital_above_delta(b.strikes.front() - 1000.0).has_value());
    }
    return report("test_pricer");
}
