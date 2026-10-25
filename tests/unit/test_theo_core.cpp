// Replay pricing/theo golden vectors -- the pure half: bracket algebra, microprice, boundaries.
//
// The quote_value cases come from 3,274 REAL Massive Q frames off captured boards, which is
// where the "absent bp means zero, not unknown" convention actually bites; the bracket_fair
// cases include 600 real market rows plus every strike_type and every refusal path.
#include "../harness.hpp"
#include "kp/pricing/theo_core.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace kp::test;
namespace th = kp::pricing::theo;

namespace {

struct Tally { long n = 0, bad = 0, nullness = 0; long long worst_ulp = -1; std::string worst; };

bool is_none(const std::string& s) { return s == "None"; }
double hx(const std::string& s) { return std::strtod(s.c_str(), nullptr); }
th::OptD opt(const std::string& s) { return is_none(s) ? th::MISSING : th::OptD(hx(s)); }

void rec(Tally& t, const std::string& in, th::OptD got, const std::string& want) {
    ++t.n;
    if (is_none(want) || !got) {
        if (is_none(want) != !got.has_value()) {
            ++t.bad; ++t.nullness;
            if (t.worst.empty())
                t.worst = in + " got=" + (got ? std::to_string(*got) : "MISSING") + " want=" + want;
        }
        return;
    }
    if (same_bits(*got, hx(want))) return;
    ++t.bad;
    const long long g = ulp_gap(*got, hx(want));
    if (g > t.worst_ulp) {
        t.worst_ulp = g;
        char b[256];
        std::snprintf(b, sizeof b, "%s got=%a want=%a ulp=%lld", in.c_str(), *got, hx(want), g);
        t.worst = b;
    }
}

// Mirrors the synthetic p_above the generator used.
th::OptD pa_ok(double k)   { return th::clamp(1.0 - (k - 7000.0) / 1000.0); }
th::OptD pa_none(double)   { return th::MISSING; }

}  // namespace

int main(int argc, char** argv) {
    const std::string dir = (argc > 1) ? argv[1] : "tests/data/golden";
    std::ifstream in(dir + "/theo_core.vec");
    if (!in) { check("open theo_core.vec", false, dir); return report("test_theo_core"); }
    std::string header;
    std::getline(in, header);
    check("header names pricing/theo.py", header.find("source=pricing/theo.py") != std::string::npos);

    std::map<std::string, Tally> T;
    std::string fn;
    while (in >> fn) {
        if (fn == "clamp") {
            std::string a, r; in >> a >> r;
            rec(T[fn], a, th::clamp(hx(a)), r);
        } else if (fn == "normalize_boundary") {
            std::string a, r; in >> a >> r;
            rec(T[fn], a, th::normalize_boundary(opt(a)), r);
        } else if (fn == "cbrt") {
            std::string a, r; in >> a >> r;
            rec(T[fn], a, th::cbrt_py(hx(a)), r);
        } else if (fn == "micro") {
            std::string a, b, c, d, r; in >> a >> b >> c >> d >> r;
            rec(T[fn], a + " " + b, th::micro(opt(a), opt(b), hx(c), hx(d)), r);
        } else if (fn == "quote_value") {
            std::string present, bp, ap, bs, as, r;
            in >> present >> bp >> ap >> bs >> as >> r;
            th::Quote q;
            q.present = (present == "1");
            q.bp = opt(bp); q.ap = opt(ap); q.bs = opt(bs); q.as = opt(as);
            rec(T[fn], bp + "/" + ap + " x " + bs + "/" + as, th::quote_value(q), r);
        } else if (fn == "bracket_fair") {
            std::string st, lo, hi, mode, r;
            in >> st >> lo >> hi >> mode >> r;
            const std::string stype = (st == "-") ? std::string() : st;
            auto [fair, det] = th::bracket_fair(
                stype, opt(lo), opt(hi),
                mode == "none" ? std::function<th::OptD(double)>(pa_none)
                               : std::function<th::OptD(double)>(pa_ok));
            rec(T[fn], st + " " + lo + " " + hi + " " + mode, fair, r);
        } else if (fn == "neighbors") {
            std::string a, rb, ra; in >> a >> rb >> ra;
            static std::vector<double> grid;
            if (grid.empty()) for (int i = 0; i < 200; ++i) grid.push_back(7000.0 + 5.0 * i);
            const th::Neighbors nb = th::neighbors(hx(a), grid);
            rec(T["neighbors.below"], a, nb.below, rb);
            rec(T["neighbors.above"], a, nb.above, ra);
        } else {
            std::string junk; std::getline(in, junk);
        }
    }

    section("pricing/theo golden vectors (pure half)");
    for (const auto& [k, t] : T) {
        char b[400];
        if (t.bad == 0) { std::snprintf(b, sizeof b, "%s (%ld cases)", k.c_str(), t.n); check(b, true); }
        else {
            std::snprintf(b, sizeof b, "%ld/%ld mismatched (%ld nullness); worst: %s",
                          t.bad, t.n, t.nullness, t.worst.c_str());
            check(k, false, b);
        }
    }

    section("the traps");
    // theo.py:175 cbrt is pow(x,1/3). If someone swaps in std::cbrt, this is the first to go.
    bits("cbrt(27) is pow, not std::cbrt", th::cbrt_py(27.0), 0x1.8000000000000p+1);
    // Kalshi's two spellings of one boundary collapse; a genuine fraction survives.
    bits("7574.9999 collapses to 7575", *th::normalize_boundary(7574.9999), 7575.0);
    bits("7691.76 keeps its fraction",  *th::normalize_boundary(7691.76),   7691.76);
    // Absent bid means zero, not unknown.
    { th::Quote q; q.present = true; q.ap = 0.35; q.as = 858.0;
      check("bid-absent quote still prices", th::quote_value(q).has_value()); }
    return report("test_theo_core");
}
