// The two artifact readers, replayed against the REAL shipped artifacts.
//
// varcal.phi is swept second-by-second across whole sessions and across both DST changeovers,
// because the C++ has no <chrono> tzdb and reaches America/New_York through TZ + localtime_r --
// so timezone agreement with Python's ZoneInfo is part of what this test proves, not an
// assumption it rests on.
#include "../harness.hpp"
#include "kp/pricing/edge.hpp"
#include "kp/pricing/varcal.hpp"
#include "kp/time/et.hpp"
#include "boards.hpp"

#include <cmath>
#include <cstdio>
#include <memory>
#include <fstream>
#include <map>
#include <sstream>

using namespace kp::test;
namespace vc = kp::pricing::varcal;
namespace ed = kp::pricing::edge;

namespace {
struct Tally { long n = 0, bad = 0; std::string worst; };
void hit(Tally& t, bool ok, const std::string& w) { ++t.n; if (!ok) { ++t.bad; if (t.worst.empty()) t.worst = w; } }
}

int main(int argc, char** argv) {
    kp::time::init();
    const std::string dir = (argc > 1) ? argv[1] : "tests/data/golden";
    const std::string root = (argc > 2) ? argv[2] : KP_KALSHI_ROOT_DEFAULT;

    std::ifstream in(dir + "/varcal_edge.vec");
    if (!in) { check("open varcal_edge.vec", false, dir); return report("test_varcal_edge"); }
    std::string header, line;
    std::getline(in, header);

    std::map<std::string, std::unique_ptr<vc::VarianceCalendar>> cals;
    std::map<std::string, std::unique_ptr<ed::Curve>> curves;
    auto cal = [&](const std::string& u) -> vc::VarianceCalendar* {
        auto it = cals.find(u);
        if (it != cals.end()) return it->second.get();
        try { cals[u] = std::make_unique<vc::VarianceCalendar>(vc::VarianceCalendar::load(u, "default", root)); }
        catch (...) { cals[u] = nullptr; }
        return cals[u].get();
    };
    auto cur = [&](const std::string& u) -> ed::Curve* {
        auto it = curves.find(u);
        if (it != curves.end()) return it->second.get();
        try { curves[u] = std::make_unique<ed::Curve>(ed::Curve::load(u, root)); }
        catch (...) { curves[u] = nullptr; }
        return curves[u].get();
    };

    std::map<std::string, Tally> T;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::istringstream ss(line);
        std::string fn, u; ss >> fn >> u;

        if (fn == "cal_meta") {
            std::string os; long sess, ncum; ss >> os >> sess >> ncum;
            auto* c = cal(u);
            hit(T[fn], c && same_bits(c->overnight_share(), hx(os)) && c->sessions() == sess
                       && (long)c->cum().size() == ncum, line);
        } else if (fn == "phi") {
            long long ns; std::string w; ss >> ns >> w;
            auto* c = cal(u);
            hit(T[fn], c && same_bits(c->phi({(std::int64_t)ns}), hx(w)), line);
        } else if (fn == "remaining_ratio") {
            long long a, b; std::string w; ss >> a >> b >> w;
            auto* c = cal(u);
            hit(T[fn], c && same_opt(c->remaining_ratio({(std::int64_t)a}, {(std::int64_t)b}), w), line);
        } else if (fn == "curve_meta") {
            std::string a, b, c2; ss >> a >> b >> c2;
            auto* c = cur(u);
            hit(T[fn], c && same_bits(c->R_min(), hx(a)) && same_bits(c->R_p99(), hx(b))
                       && same_bits(c->R_max(), hx(c2)), line);
        } else if (fn == "curve" || fn == "curve_special") {
            std::string Rs, wd, wm, wsp, wru, wb;
            ss >> Rs >> wd >> wm >> wsp >> wru >> wb;
            std::optional<double> R;
            if (fn == "curve") R = hx(Rs);
            else if (Rs == "nan")  R = std::nan("");
            else if (Rs == "inf")  R = HUGE_VAL;
            else if (Rs == "-inf") R = -HUGE_VAL;
            auto* c = cur(u);
            if (!c) { hit(T[fn], false, "no curve"); continue; }
            const auto em = c->edge_micros(R);
            const bool em_ok = (wm == "None") ? !em.has_value()
                                              : (em.has_value() && *em == std::stol(wm));
            hit(T[fn], same_opt(c->edge_dollars(R), wd) && em_ok
                       && same_bits(c->scale_position(R), hx(wsp))
                       && same_bits(c->risk_u(R), hx(wru))
                       && same_opt(c->bonus_dollars(R), wb), line);
        } else if (fn == "flat") {
            // `u` held the R token for this row.
            std::string wd, wm, wsp, wru;
            ss >> wd >> wm >> wsp >> wru;
            const auto R = opt(u);
            ed::FlatCurve fc;
            hit(T[fn], same_opt(fc.edge_dollars(R), wd) && *fc.edge_micros(R) == std::stol(wm)
                       && same_bits(fc.scale_position(R), hx(wsp))
                       && same_bits(fc.risk_u(R), hx(wru)), line);
        }
    }

    section("pricing/varcal + pricing/edge golden vectors");
    for (const auto& [k, t] : T) {
        char b[400];
        if (t.bad == 0) { std::snprintf(b, sizeof b, "%s (%ld cases)", k.c_str(), t.n); check(b, true); }
        else { std::snprintf(b, sizeof b, "%ld/%ld mismatched; first: %s", t.bad, t.n, t.worst.c_str()); check(k, false, b); }
    }

    section("the refusals");
    if (auto* c = cur("SPX")) {
        // Above R_max we REFUSE rather than clamp: an R beyond anything a year produced is a
        // market we do not understand, not one to charge more for.
        check("R above R_max refuses", !c->edge_dollars(c->R_max() * 1.001).has_value());
        check("R at R_max still quotes", c->edge_dollars(c->R_max()).has_value());
        check("a refused R scores scale_position 1.0",
              c->scale_position(c->R_max() * 1.001) == 1.0);
        check("nan R refuses", !c->edge_dollars(std::nan("")).has_value());
    }
    return report("test_varcal_edge");
}
