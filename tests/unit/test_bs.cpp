// Replay pricing/bs golden vectors. Bit-identical or it fails.
//
// bs_price is the single most load-bearing function in the port: implied_w bisects on it for
// ~42 halvings, so a 1-ulp disagreement flips a late branch and moves the root, and that root
// is every skew fit and every ATM vol.
#include "../harness.hpp"
#include "kp/pricing/bs.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>

using namespace kp::test;
namespace bs = kp::pricing::bs;

namespace {

struct Tally {
    long n = 0, bad = 0, nullness = 0;
    long long worst_ulp = -1;
    std::string worst;
};

double hx(const std::string& s) { return std::strtod(s.c_str(), nullptr); }
bool is_none(const std::string& s) { return s == "None"; }

void rec(Tally& t, const std::string& in, bs::OptD got, const std::string& want) {
    ++t.n;
    if (is_none(want) || !got) {
        // MISSING is control flow, not a number: present-vs-absent is its own failure class.
        if (is_none(want) != !got.has_value()) {
            ++t.bad; ++t.nullness;
            if (t.worst.empty())
                t.worst = in + " got=" + (got ? std::to_string(*got) : "MISSING") +
                          " want=" + want;
        }
        return;
    }
    const double w = hx(want);
    if (same_bits(*got, w)) return;
    ++t.bad;
    const long long g = ulp_gap(*got, w);
    if (g > t.worst_ulp) {
        t.worst_ulp = g;
        char b[256];
        std::snprintf(b, sizeof b, "%s got=%a want=%a ulp=%lld", in.c_str(), *got, w, g);
        t.worst = b;
    }
}

}  // namespace

int main(int argc, char** argv) {
    const std::string dir = (argc > 1) ? argv[1] : "tests/data/golden";
    std::ifstream in(dir + "/bs.vec");
    if (!in) { check("open bs.vec", false, dir); return report("test_bs"); }
    std::string header;
    std::getline(in, header);
    check("header names pricing/bs.py", header.find("source=pricing/bs.py") != std::string::npos);

    std::map<std::string, Tally> T;
    std::string fn;
    while (in >> fn) {
        std::string a, b, c, d, r;
        if (fn == "norm_cdf" || fn == "norm_pdf") {
            in >> a >> r;
            const double x = hx(a);
            rec(T[fn], a, fn == "norm_cdf" ? bs::norm_cdf(x) : bs::norm_pdf(x), r);
        } else if (fn == "bs_price") {
            in >> a >> b >> c >> d >> r;
            rec(T[fn], a + " " + b + " " + c + " " + d,
                bs::bs_price(hx(a), hx(b), hx(c), d[0]), r);
        } else if (fn == "vega_w") {
            in >> a >> b >> c >> r;
            rec(T[fn], a + " " + b + " " + c, bs::vega_w(hx(a), hx(b), hx(c)), r);
        } else if (fn == "p_above") {
            in >> a >> b >> c >> r;
            rec(T[fn], a + " " + b + " " + c, bs::p_above(hx(a), hx(b), hx(c)), r);
        } else if (fn == "implied_w") {
            in >> a >> b >> c >> d >> r;
            bs::OptD px = is_none(c) ? bs::MISSING : bs::OptD(hx(c));
            rec(T[fn], a + " " + b + " " + c + " " + d,
                bs::implied_w(hx(a), hx(b), px, d[0]), r);
        } else {
            std::getline(in, a);
        }
    }

    section("pricing/bs golden vectors");
    for (const auto& [k, t] : T) {
        char b[400];
        if (t.bad == 0) {
            std::snprintf(b, sizeof b, "%s (%ld cases)", k.c_str(), t.n);
            check(b, true);
        } else {
            std::snprintf(b, sizeof b, "%ld/%ld mismatched (%ld nullness); worst: %s",
                          t.bad, t.n, t.nullness, t.worst.c_str());
            check(k, false, b);
        }
    }
    return report("test_bs");
}
