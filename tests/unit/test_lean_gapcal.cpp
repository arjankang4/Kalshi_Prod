// Replays tests/data/golden/lean_gapcal.vec against kp::pricing::lean and kp::pricing::gapcal.
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>

#include "kp/pricing/gapcal.hpp"
#include "kp/pricing/lean.hpp"
#include "kp/pycompat/round.hpp"
#include "kp/time/et.hpp"

#include "boards.hpp"
#include "../harness.hpp"

namespace ln = kp::pricing::lean;
namespace gc = kp::pricing::gapcal;
using namespace kp::test;

namespace {
struct Tally { long n = 0, bad = 0; std::string worst; };
void hit(Tally& t, bool ok, const std::string& line) {
    ++t.n;
    if (!ok) { ++t.bad; if (t.worst.empty()) t.worst = line; }
}
kp::time::Instant at(const std::string& s) {
    return kp::time::Instant{std::strtoll(s.c_str(), nullptr, 10)};
}
}  // namespace

int main(int argc, char** argv) {
    kp::pycompat::assert_c_numeric_locale();
    kp::time::init();
    const std::string dir = (argc > 1) ? argv[1] : "tests/data/golden";
    const std::string root = (argc > 2) ? argv[2] : KP_KALSHI_ROOT_DEFAULT;

    std::map<std::string, std::unique_ptr<gc::GapCalendar>> cals;
    auto cal = [&](const std::string& u) -> gc::GapCalendar* {
        auto it = cals.find(u);
        if (it != cals.end()) return it->second.get();
        try {
            cals[u] = std::make_unique<gc::GapCalendar>(gc::GapCalendar::load(u, "default", root));
        } catch (...) { cals[u] = nullptr; }
        return cals[u].get();
    };

    std::ifstream in(dir + "/lean_gapcal.vec");
    if (!in) { check("open lean_gapcal.vec", false, dir); return report("test_lean_gapcal"); }
    std::string header;
    std::getline(in, header);
    check("header names pricing/gapcal.py",
          header.find("source=pricing/gapcal.py") != std::string::npos);

    std::map<std::string, Tally> T;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string fn;
        ss >> fn;
        if (fn == "lean") {
            std::string side, pi, pr, cl, df, ag, d_, u_, cents, reason, got_side;
            long micros;
            ss >> side >> pi >> pr >> cl >> df >> ag >> micros >> d_ >> u_ >> cents
               >> reason >> got_side;
            const auto [gm, det] = ln::edge_adjust(opt(pi), opt(pr), side,
                                                   opt(cl), opt(df), opt(ag));
            const std::string gr = det.lean_reason.empty() ? "-" : det.lean_reason;
            hit(T[fn], gm == micros && same_opt(det.lean_d, d_) && same_opt(det.lean_u, u_) &&
                       same_bits(det.lean_cents, hx(cents)) && gr == reason &&
                       det.lean_side == got_side, line);
        } else if (fn == "gap_meta") {
            std::string u, c4;
            long ncum, nsh;
            ss >> u >> ncum >> nsh >> c4;
            gc::GapCalendar* c = cal(u);
            if (c == nullptr) { check("load gap calendar " + u, false); continue; }
            hit(T[fn], (long)c->cum().size() == ncum && (long)c->shares().size() == nsh &&
                       same_bits(c->cum().back(), hx(c4)), line);
        } else if (fn == "phi_gap") {
            std::string u, t, mins, want;
            ss >> u >> t >> mins >> want;
            gc::GapCalendar* c = cal(u);
            if (c == nullptr) continue;
            hit(T[fn], same_opt(c->minutes_into_gap(at(t)), mins) &&
                       same_bits(c->phi_gap(at(t)), hx(want)), line);
        } else if (fn == "share_between") {
            std::string u, t0, t1, want;
            ss >> u >> t0 >> t1 >> want;
            gc::GapCalendar* c = cal(u);
            if (c == nullptr) continue;
            hit(T[fn], same_bits(c->share_between(at(t0), at(t1)), hx(want)), line);
        } else {
            std::getline(ss, fn);
        }
    }

    section("pricing/lean + pricing/gapcal golden vectors");
    for (const auto& [k, t] : T) {
        char b[500];
        if (t.bad == 0) { std::snprintf(b, sizeof b, "%s (%ld cases)", k.c_str(), t.n); check(b, true); }
        else { std::snprintf(b, sizeof b, "%ld/%ld mismatched; first: %s", t.bad, t.n, t.worst.c_str()); check(k, false, b); }
    }

    section("the traps");
    {
        // THE ASYMMETRY AND THE SIGN, asserted as arithmetic rather than replayed. d > 0 means
        // realized says MORE likely, so we want to buy yes: the bid is aggressive (tightens,
        // negative), the ask defensive (widens, positive). A port that mirrored the sign would
        // lean into exactly the trades the model says are dangerous.
        const auto [bid_m, bid_d] = ln::edge_adjust(0.40, 0.50, "bid");   // d = +10, past the clamp: u = 1
        const auto [ask_m, ask_d] = ln::edge_adjust(0.40, 0.50, "ask");
        check("realized above implied makes the BID aggressive",
              bid_d.lean_reason == "aggressive" && bid_m < 0, std::to_string(bid_m));
        check("and the ASK defensive",
              ask_d.lean_reason == "defensive" && ask_m > 0, std::to_string(ask_m));
        // LEAN_DEFENSIVE_CENTS / LEAN_AGGRESSIVE_CENTS at u = 1: +1.5c and -0.75c.
        check("at the clamp the defensive leg is exactly +1.5c", ask_m == 15000,
              std::to_string(ask_m));
        check("and the aggressive leg exactly -0.75c", bid_m == -7500, std::to_string(bid_m));
        check("defensive is TWICE aggressive -- the legs are asymmetric by design",
              ask_m == -2 * bid_m);

        const auto [b2, d2] = ln::edge_adjust(0.50, 0.40, "bid");         // d = -10
        const auto [a2, e2] = ln::edge_adjust(0.50, 0.40, "ask");
        check("the sign mirrors: realized BELOW implied flips which side is which",
              d2.lean_reason == "defensive" && e2.lean_reason == "aggressive" &&
              b2 == 15000 && a2 == -7500);

        // Beyond the clamp nothing more happens -- u saturates at 1.
        const auto [b3, _d3] = ln::edge_adjust(0.10, 0.90, "ask");
        check("past the clamp the lean stops growing", b3 == ask_m);

        // Agreement is a no-op, and absence is a no-op with a different reason. A caller adds
        // this unconditionally, so both must return exactly 0.
        check("perfect agreement adds nothing",
              ln::edge_adjust(0.4, 0.4, "bid").first == 0 &&
              ln::edge_adjust(0.4, 0.4, "bid").second.lean_reason == "no_disagreement");
        check("a missing chain adds nothing",
              ln::edge_adjust(std::nullopt, 0.4, "bid").first == 0 &&
              ln::edge_adjust(std::nullopt, 0.4, "bid").second.lean_reason == "no_chain");

        gc::GapCalendar* c = cal("SPX");
        if (c != nullptr) {
            // The gap runs 16:00 -> 09:30 and is exactly 1050 minutes of wall clock.
            const kp::time::Instant close = kp::time::from_et(2026, 8, 20, 16, 0, 0);
            const kp::time::Instant open = kp::time::from_et(2026, 8, 21, 9, 30, 0);
            check("phi_gap is 0 at the 16:00 close", same_bits(c->phi_gap(close), 0.0));
            check("and 1 at the 09:30 open", same_bits(c->phi_gap(open), 1.0));
            check("the gap is exactly 1050 minutes",
                  c->minutes_into_gap(open).has_value() &&
                  same_bits(*c->minutes_into_gap(open), 1050.0));

            // Inside RTH the gap is over, so phi_gap saturates rather than interpolating.
            check("inside RTH phi_gap reads 1.0, not a bucket",
                  same_bits(c->phi_gap(kp::time::from_et(2026, 8, 21, 12, 0, 0)), 1.0));
            check("and minutes_into_gap refuses the instant",
                  !c->minutes_into_gap(kp::time::from_et(2026, 8, 21, 12, 0, 0)).has_value());

            // Monotone and non-negative -- share_between clamps at zero, so a reversed pair is
            // 0.0 rather than negative.
            check("share_between is non-negative on a reversed pair",
                  same_bits(c->share_between(open, close), 0.0));
            double prev = -1.0;
            bool mono = true;
            for (int m = 0; m <= 1050; m += 7) {
                const double v = c->phi_gap(kp::time::Instant{close.ns + (long long)m * 60000000000LL});
                if (v < prev) mono = false;
                prev = v;
            }
            check("phi_gap is non-decreasing across the whole gap", mono);

            // THE DST DAYS, pinned deliberately, because they come out the OPPOSITE way from
            // the intuitive reading. The reference's `et - anchor` is a NAIVE subtraction --
            // both sides are aware and share config.ET, and Python ignores a common tzinfo --
            // so it measures wall clock, not elapsed time.
            //
            // Spring forward: 10:00 ET is 480 + 600 = 1080 WALL minutes past the previous 16:00,
            // beyond the 1050 window, so it is refused even though only 1020 real minutes have
            // passed. An implementation using elapsed seconds returns 1020 and accepts it.
            const auto spring = c->minutes_into_gap(kp::time::from_et(2026, 3, 8, 10, 0, 0));
            check("spring forward: 10:00 is 1080 WALL minutes out and is refused",
                  !spring.has_value(), spring ? std::to_string(*spring) : "None");
            // Fall back: the mirror image. 09:00 ET is 1020 wall minutes out, inside the window,
            // and prices off the calendar -- where elapsed seconds would give 1080 and refuse it.
            const auto fall = c->minutes_into_gap(kp::time::from_et(2026, 11, 1, 9, 0, 0));
            check("fall back: 09:00 is 1020 WALL minutes out and IS inside the gap",
                  fall.has_value() && same_bits(*fall, 1020.0),
                  fall ? std::to_string(*fall) : "None");
            check("...and reads the calendar there, not a saturated 1.0",
                  std::fabs(c->phi_gap(kp::time::from_et(2026, 11, 1, 9, 0, 0)) - 0.938676789681238)
                      < 1e-12);
        }
    }

    return report("test_lean_gapcal");
}
