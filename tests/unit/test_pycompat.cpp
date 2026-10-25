// Replay the golden vectors for the Python-semantics shims.
//
// The bar is bit-identical, not "close": every mismatch here is a price that would differ
// from the Python reference. The ULP column in a failure exists to tell you WHICH kind of bug
// you have -- a wall of ulp=1 is a formula-association or contraction problem, a wall of large
// gaps is a missing shim -- not to suggest any of them are acceptable.
#include "../harness.hpp"
#include "kp/pycompat/round.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace kp::test;
namespace pc = kp::pycompat;

namespace {

struct Tally {
    long n = 0, bad = 0;
    double worst_got = 0, worst_want = 0, worst_in = 0;
    long long worst_ulp = -1;
};

double hx(const std::string& s) { return std::strtod(s.c_str(), nullptr); }

void record(Tally& t, double in, double got, double want) {
    ++t.n;
    if (same_bits(got, want)) return;
    ++t.bad;
    const long long g = ulp_gap(got, want);
    if (g > t.worst_ulp) { t.worst_ulp = g; t.worst_in = in; t.worst_got = got; t.worst_want = want; }
}

void report_tallies(const std::map<std::string, Tally>& tallies) {
    for (const auto& [fn, t] : tallies) {
        char buf[240];
        if (t.bad == 0) {
            std::snprintf(buf, sizeof buf, "%s (%ld cases)", fn.c_str(), t.n);
            check(buf, true);
        } else {
            std::snprintf(buf, sizeof buf,
                          "%s: %ld/%ld mismatched; worst in=%.17g (%a) got=%.17g want=%.17g ulp=%lld",
                          fn.c_str(), t.bad, t.n, t.worst_in, t.worst_in,
                          t.worst_got, t.worst_want, t.worst_ulp);
            check(fn, false, buf);
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    pc::assert_c_numeric_locale();
    const std::string dir = (argc > 1) ? argv[1] : "tests/data/golden";

    section("pyround.vec -- round, round(x,n), floor_tick, ceil_tick");
    {
        std::ifstream in(dir + "/pyround.vec");
        if (!in) { check("open pyround.vec", false, dir); return report("test_pycompat"); }
        std::string line;
        std::getline(in, line);                     // header
        check("header names the reference source", line.find("source=strategy/quoting.py") != std::string::npos);

        std::map<std::string, Tally> tallies;
        std::string fn, a, r;
        while (in >> fn >> a >> r) {
            const double x = hx(a), want = hx(r);
            double got;
            if      (fn == "round")      got = pc::py_round(x);
            else if (fn == "round9")     got = pc::py_round_nd(x, 9);
            else if (fn == "round4")     got = pc::py_round_nd(x, 4);
            else if (fn == "floor_tick") got = pc::floor_tick(x);
            else if (fn == "ceil_tick")  got = pc::ceil_tick(x);
            else continue;
            record(tallies[fn], x, got, want);
        }
        report_tallies(tallies);
    }

    section("median.vec -- statistics.median, mean-of-two-middles on even n");
    {
        std::ifstream in(dir + "/median.vec");
        if (!in) { check("open median.vec", false, dir); return report("test_pycompat"); }
        std::string line;
        std::getline(in, line);
        Tally t;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            std::istringstream ss(line);
            std::string tok;
            ss >> tok;                              // "median"
            std::vector<std::string> toks;
            while (ss >> tok) toks.push_back(tok);
            if (toks.size() < 2) continue;
            std::vector<double> v;
            v.reserve(toks.size() - 1);
            for (std::size_t i = 0; i + 1 < toks.size(); ++i) v.push_back(hx(toks[i]));
            record(t, (double)v.size(), pc::py_median(v), hx(toks.back()));
        }
        std::map<std::string, Tally> m{{"median", t}};
        report_tallies(m);
    }

    section("the traps these shims exist for");
    // If someone replaces py_round with std::round, this is the case that breaks first:
    // strategy/quoting.py:320 tier_offsets at scale_pos == 0.25.
    {
        const double u = 0.25;
        const long long rung2 = pc::py_round_i(2.0 + (4.0 - 2.0) * u);
        eq<long long>("tier_offsets rung 2 at scale_pos=0.25 is 2, not 3", rung2, 2);
    }
    // If someone drops the inner round(_, 9) from floor_tick, these are the cases that break:
    // without it, 0.29/0.01 is 28.999999999999996 and floors to 28, putting the price a full
    // tick low. Expectations taken from the Python, NOT from the literal -- note 0.47 and 0.57
    // legitimately come back as 0.47000000000000003 and 0.5700000000000001.
    bits("floor_tick(0.29) matches Python exactly", pc::floor_tick(0.29), 0x1.28f5c28f5c28fp-2);
    bits("floor_tick(0.47) matches Python exactly", pc::floor_tick(0.47), 0x1.e147ae147ae15p-2);
    bits("floor_tick(0.57) matches Python exactly", pc::floor_tick(0.57), 0x1.23d70a3d70a3ep-1);
    bits("floor_tick(0.58) matches Python exactly", pc::floor_tick(0.58), 0x1.28f5c28f5c28fp-1);
    bits("floor_tick(0.59) matches Python exactly", pc::floor_tick(0.59), 0x1.2e147ae147ae1p-1);

    // Python's math.ceil returns an int, so a tiny negative rounds up to +0.0, not -0.0.
    bits("ceil_tick(-0.0068) is +0.0, not -0.0", pc::ceil_tick(-0.0068040472864632218), 0.0);
    return report("test_pycompat");
}
