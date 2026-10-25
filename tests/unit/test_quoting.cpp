// Replay strategy/quoting golden vectors, then the worked examples that define the ladder.
//
// The sweep is over the whole tradeable cent grid rather than a random sample, because `decide`
// branches on comparisons between theo, the touches and the tick grid -- and it is exactly the
// boundary cases (edge equal to the bar, target equal to the touch, a one-sided book) that a
// uniform sample visits rarely and a grid visits every time.
#include "kp/config.hpp"
#include "../harness.hpp"
#include "kp/strategy/quoting.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace kp::test;
namespace q = kp::strategy::quoting;

namespace {

struct Tally { long n = 0, bad = 0; std::string worst; };

bool is_none(const std::string& s) { return s == "None"; }
double hx(const std::string& s) { return std::strtod(s.c_str(), nullptr); }
q::OptD opt(const std::string& s) { return is_none(s) ? q::NONE : q::OptD(hx(s)); }

void hit(Tally& t, bool ok, const std::string& what) {
    ++t.n;
    if (ok) return;
    ++t.bad;
    if (t.worst.empty()) t.worst = what;
}

bool same_opt(q::OptD got, const std::string& want) {
    if (is_none(want) || !got) return is_none(want) == !got.has_value();
    return same_bits(*got, hx(want));
}

std::string show(q::OptD v) {
    if (!v) return "None";
    char b[40]; std::snprintf(b, sizeof b, "%a", *v); return b;
}

q::Tighten tighten_of(const std::string& mode) {
    if (mode == "default") return q::Tighten{};
    if (mode == "mid")     return q::Tighten::cap_at_mid();
    if (mode == "half")    return q::Tighten::fraction(0.5);
    return q::Tighten::fraction(1.0 / 3.0);          // "third"
}

}  // namespace

int main(int argc, char** argv) {
    const std::string dir = (argc > 1) ? argv[1] : "tests/data/golden";
    std::ifstream in(dir + "/quoting.vec");
    if (!in) { check("open quoting.vec", false, dir); return report("test_quoting"); }
    std::string header, line;
    std::getline(in, header);
    check("header names strategy/quoting.py",
          header.find("source=strategy/quoting.py") != std::string::npos);

    std::map<std::string, Tally> T;
    // From config, never hardcoded. A hard-coded copy here can silently drift from the
    // reference's counts, and the only thing that would catch it is the rung `count` field in the
    // ladder vectors. Deriving it means a future change to the counts fails as a config mismatch
    // rather than as thousands of unexplained ladder diffs.
    std::vector<std::string> counts;
    for (auto c : kp::config::QUOTE_TIER_COUNTS) counts.emplace_back(c);

    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::istringstream ss(line);
        std::string fn; ss >> fn;

        if (fn == "edge_micros") {
            std::string side, a, b; long want;
            ss >> side >> a >> b >> want;
            hit(T[fn], q::edge_micros(side, hx(a), hx(b)) == want, line);
        } else if (fn == "tradeable") {
            std::string a; int want; ss >> a >> want;
            hit(T[fn], q::tradeable(hx(a)) == (want != 0), line);
        } else if (fn == "tier_offsets") {
            std::string a; int w1, w2; ss >> a >> w1 >> w2;
            const auto got = q::tier_offsets(hx(a));
            hit(T[fn], got.size() == 2 && got[0] == w1 && got[1] == w2, line);
        } else if (fn == "tier_offsets_none") {
            int w1, w2; ss >> w1 >> w2;
            const auto got = q::tier_offsets(q::NONE);
            hit(T[fn], got.size() == 2 && got[0] == w1 && got[1] == w2, line);
        } else if (fn == "decide" || fn == "decide2") {
            std::string side, th, bb, ba, offs, bars, mode, kind, px, ed, reason;
            if (fn == "decide") {
                ss >> side >> th >> bb >> ba >> mode >> kind >> px >> ed >> reason;
                offs = "None"; bars = "None";
            } else {
                ss >> side >> th >> bb >> ba >> offs >> bars >> mode >> kind >> px >> ed >> reason;
            }
            std::optional<long> bar;
            if (!is_none(bars)) bar = std::stol(bars);
            const q::Action a = q::decide(side, opt(th), opt(bb), opt(ba), opt(offs), bar,
                                          tighten_of(mode));
            const bool ok = a.kind == kind && same_opt(a.price, px) &&
                            same_opt(a.edge, ed) && a.reason == reason;
            hit(T[fn], ok, line + "  ||  got " + a.kind + " " + show(a.price) + " " +
                              show(a.edge) + " " + a.reason);
        } else if (fn == "ladder") {
            std::string side, th, bb, ba, sp, tes, bars;
            int n;
            ss >> side >> th >> bb >> ba >> sp >> tes >> bars >> n;
            std::optional<long> bar;
            if (!is_none(bars)) bar = std::stol(bars);
            const auto got = q::decide_ladder(side, opt(th), opt(bb), opt(ba), counts, q::NONE,
                                              bar, opt(sp), tes == "1");
            bool ok = ((int)got.size() == n);
            for (int i = 0; ok && i < n; ++i) {
                std::string tok; ss >> tok;
                // index:price:count:edge:reason:min_edge
                std::vector<std::string> f;
                std::size_t p0 = 0;
                for (std::size_t p = 0; p <= tok.size(); ++p)
                    if (p == tok.size() || tok[p] == ':') { f.push_back(tok.substr(p0, p - p0)); p0 = p + 1; }
                if (f.size() != 6) { ok = false; break; }
                const auto& r = got[(std::size_t)i];
                const bool me = is_none(f[5]) ? !r.min_edge.has_value()
                                              : (r.min_edge.has_value() &&
                                                 *r.min_edge == std::stol(f[5]));
                ok = r.index == std::stoi(f[0]) && same_bits(r.price, hx(f[1])) &&
                     r.count == f[2] && same_opt(r.edge, f[3]) && r.reason == f[4] && me;
            }
            hit(T[fn], ok, line);
        } else if (fn == "rung_min_edge") {
            std::string bars, sp; int rank; long want;
            ss >> rank >> bars >> sp >> want;
            std::optional<long> bar;
            if (!is_none(bars)) bar = std::stol(bars);
            hit(T[fn], q::rung_min_edge_micros(rank, bar, opt(sp)) == want, line);
        } else if (fn == "should_pull") {
            std::string side, th, rp; int alone, want;
            ss >> side >> th >> rp >> alone >> want;
            hit(T[fn], q::should_pull(side, hx(th), hx(rp), alone != 0) == (want != 0), line);
        } else if (fn == "should_improve") {
            std::string side, th, rp, px, ed; int want;
            ss >> side >> th >> rp >> px >> ed >> want;
            q::Action a; a.price = opt(px); a.edge = opt(ed);
            hit(T[fn], q::should_improve(side, hx(th), hx(rp), a) == (want != 0), line);
        }
    }

    section("strategy/quoting golden vectors");
    for (const auto& [k, t] : T) {
        char b[600];
        if (t.bad == 0) { std::snprintf(b, sizeof b, "%s (%ld cases)", k.c_str(), t.n); check(b, true); }
        else {
            std::snprintf(b, sizeof b, "%ld/%ld mismatched; first: %s", t.bad, t.n, t.worst.c_str());
            check(k, false, b);
        }
    }

    section("the worked examples, asserted directly");
    // The tighten cap, with QUOTE_TIGHTEN_FRACTION = 0.5: book 8 at 20, theo 18, edge 0.8 ->
    // naive target 17, capped at 8 + (20-8)*0.5 = 14.
    {
        const q::Action a = q::decide("bid", 0.18, 0.08, 0.20, 0.008);
        bits("wide-book bid caps at 0.14, not 0.17", *a.price, 0.14);
    }
    // A one-sided book is ONE_SIDED_WIDTH ticks wide, not infinitely wide: uncapped this would
    // post 0.09.
    {
        const q::Action a = q::decide("bid", 0.10, 0.04, q::NONE);
        check("one-sided book engages the tighten cap", a.price.has_value() && *a.price < 0.09,
              "price=" + show(a.price));
    }
    // tier_offsets at exactly 0.75: round(2.5) and round(4.5) are 2 and 4 in Python, 3 and 5
    // under std::round.
    {
        const auto t = q::tier_offsets(0.75);
        eq<int>("tier_offsets(0.75)[0] is 2, not 3", t[0], 2);
        eq<int>("tier_offsets(0.75)[1] is 4, not 5", t[1], 4);
    }
    // A worthless bracket must never emit the untradeable -0.01 the empty-side branch would give.
    {
        const q::Action a = q::decide("bid", 0.0, q::NONE, q::NONE);
        check("theo 0.00 on an empty book does not quote -0.01",
              !a.price.has_value() || *a.price >= kp::config::MIN_PRICE, "price=" + show(a.price));
    }
    return report("test_quoting");
}
