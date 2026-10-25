// Replay pricing/greeks golden vectors against the REAL variance calendar.
//
// Swept minute-by-minute across a whole session, because theta is a FINITE one-minute difference
// through a piecewise-linear calendar rather than an analytic derivative -- and the places that
// matter are exactly where the calendar is least smooth (the 15:59 MOC bucket carries ~3x a flat
// minute). The last two minutes are included on purpose: MIN_MINUTES_TO_CLOSE refuses to QUOTE
// them, but the Greeks must still be computed the same way there, and theta really does go to
// -0.49992.
#include "../harness.hpp"
#include "kp/pricing/greeks.hpp"
#include "kp/pricing/varcal.hpp"
#include "kp/time/et.hpp"
#include "boards.hpp"

#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>

using namespace kp::test;
namespace gk = kp::pricing::greeks;
namespace vc = kp::pricing::varcal;

namespace {
struct Tally { long n = 0, bad = 0; std::string worst; };
void hit(Tally& t, bool ok, const std::string& w) { ++t.n; if (!ok) { ++t.bad; if (t.worst.empty()) t.worst = w; } }
kp::time::Instant at(long long ns) { return {(std::int64_t)ns}; }
}

int main(int argc, char** argv) {
    kp::time::init();
    const std::string dir = (argc > 1) ? argv[1] : "tests/data/golden";
    const std::string root = (argc > 2) ? argv[2] : KP_KALSHI_ROOT_DEFAULT;

    std::unique_ptr<vc::VarianceCalendar> cal;
    try { cal = std::make_unique<vc::VarianceCalendar>(vc::VarianceCalendar::load("SPX", "default", root)); }
    catch (const std::exception& e) { check("load the SPX variance calendar", false, e.what()); return report("test_greeks"); }
    check("loaded the SPX variance calendar", true);

    std::ifstream in(dir + "/greeks.vec");
    if (!in) { check("open greeks.vec", false, dir); return report("test_greeks"); }
    std::string header, line;
    std::getline(in, header);

    std::map<std::string, Tally> T;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::istringstream ss(line);
        std::string fn; ss >> fn;

        if (fn == "var_left") {
            long long a, b; int fl; std::string w;
            ss >> a >> b >> fl >> w;
            hit(T[fn], same_opt(gk::var_left(*cal, at(a), at(b), fl != 0), w), line);
        } else if (fn == "total_vol") {
            std::string sig, vl, w; ss >> sig >> vl >> w;
            hit(T[fn], same_opt(gk::total_vol(hx(sig), opt(vl)), w), line);
        } else if (fn == "delta") {
            std::string F, K, wv, w; ss >> F >> K >> wv >> w;
            hit(T[fn], same_opt(gk::delta_per_point(hx(F), hx(K), opt(wv)), w), line);
        } else if (fn == "vega") {
            std::string F, K, wv, vl, w; ss >> F >> K >> wv >> vl >> w;
            hit(T[fn], same_opt(gk::vega_per_volpoint(hx(F), hx(K), opt(wv), opt(vl)), w), line);
        } else if (fn == "theta") {
            std::string F, K, sig; long long n0, n1; std::string w;
            ss >> F >> K >> sig >> n0 >> n1 >> w;
            hit(T[fn], same_opt(gk::theta_per_minute(hx(F), hx(K), hx(sig), at(n0), at(n1), *cal), w),
                line);
        } else if (fn == "one_sided_greeks") {
            std::string g, a, b, c, d; ss >> g >> a >> b >> c >> d;
            const auto o = gk::one_sided_greeks(opt(g));
            hit(T[fn], same_opt(o.get(kp::pricing::theo::Prob::less), a)
                       && same_opt(o.get(kp::pricing::theo::Prob::less_or_equal), b)
                       && same_opt(o.get(kp::pricing::theo::Prob::greater), c)
                       && same_opt(o.get(kp::pricing::theo::Prob::greater_or_equal), d), line);
        } else if (fn == "market_greeks") {
            std::string st, lo, hi; long long n0, n1;
            std::string F, sig, wd, wv, wt, wr, reason;
            ss >> st >> lo >> hi >> n0 >> n1 >> F >> sig >> wd >> wv >> wt >> wr >> reason;
            const std::string stype = (st == "-") ? std::string() : st;
            const auto g = gk::market_greeks(stype, opt(lo), opt(hi), hx(F), hx(sig),
                                             at(n0), at(n1), *cal);
            const std::string gr = g.reason.empty() ? "-" : g.reason;
            hit(T[fn], same_opt(g.delta, wd) && same_opt(g.vega, wv) && same_opt(g.theta, wt)
                       && same_opt(g.R, wr) && gr == reason, line);
        }
    }

    section("pricing/greeks golden vectors");
    for (const auto& [k, t] : T) {
        char b[400];
        if (t.bad == 0) { std::snprintf(b, sizeof b, "%s (%ld cases)", k.c_str(), t.n); check(b, true); }
        else { std::snprintf(b, sizeof b, "%ld/%ld mismatched; first: %s", t.bad, t.n, t.worst.c_str()); check(k, false, b); }
    }

    section("the properties that cost this repo something");
    {
        // bracket_greek must NOT clamp. On a real board the clamp turned a `between` delta of
        // -0.004156 into +0.000000 -- a market reading as risk-free when it was most exposed.
        // A `between` is delta(lo) - delta(hi), so it goes negative whenever the CAP is nearer
        // the money than the floor -- which is half the board. A constant g_at would cancel, so
        // this varies with the boundary the way delta_per_point actually does.
        const auto g = gk::bracket_greek("between", 7375.0, 7400.0,
                                         [](double k) { return gk::OptD(k < 7390.0 ? 0.10 : 0.30); });
        check("a between bracket keeps a negative Greek", g.has_value() && *g < 0.0,
              g ? std::to_string(*g) : "MISSING");
    }
    {
        // one_sided_greeks maps `less` to -g, NOT 1-g. Reusing the probability mapping read
        // +0.985 on a real board where the true delta was -0.015.
        const auto o = gk::one_sided_greeks(0.015);
        bits("less maps to -g, not 1-g", *o.get(kp::pricing::theo::Prob::less), -0.015);
    }
    return report("test_greeks");
}
