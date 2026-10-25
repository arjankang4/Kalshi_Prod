// Replays tests/data/golden/spot.vec against kp::pricing::spot.
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "kp/config.hpp"
#include "kp/pricing/spot.hpp"
#include "kp/pycompat/round.hpp"

#include "boards.hpp"
#include "../harness.hpp"

namespace sp = kp::pricing::spot;
namespace ix = kp::massive::index;
using namespace kp::test;

namespace {

struct Tally { long n = 0, bad = 0; std::string worst; };

void hit(Tally& t, bool ok, const std::string& line) {
    ++t.n;
    if (!ok) { ++t.bad; if (t.worst.empty()) t.worst = line; }
}

// The generator writes "-" for an empty/None string, so it survives whitespace splitting.
std::string dash(const std::string& s) { return s == "-" ? std::string() : s; }

}  // namespace

int main(int argc, char** argv) {
    kp::pycompat::assert_c_numeric_locale();
    const std::string dir = (argc > 1) ? argv[1] : "tests/data/golden";

    std::ifstream in(dir + "/spot.vec");
    if (!in) { check("open spot.vec", false, dir); return report("test_spot"); }
    std::string header;
    std::getline(in, header);
    check("header names pricing/spot.py", header.find("source=pricing/spot.py") != std::string::npos);

    // The generator patched IndexQuote.age_s to a FIXED instant so resolve() is reproducible.
    // Same instant here, passed through the `at_ns` seam.
    const long long NOW_NS = 1756200000000000000LL;

    std::map<std::string, Tally> T;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string fn;
        ss >> fn;
        if (fn == "resolve") {
            std::string par, iqs, ms, v, src, idx, pty, age, status, reason, div, divp;
            int alarm;
            ss >> par >> iqs >> ms >> v >> src >> idx >> pty >> age >> status >> reason
               >> div >> divp >> alarm;
            std::optional<ix::IndexQuote> iq;
            if (!is_none(iqs)) {
                const std::vector<std::string> f = split(iqs, ':');
                ix::IndexQuote k;
                k.underlying = "SPX";
                k.ticker = "I:SPX";
                k.value = opt(f[0]);
                if (!is_none(f[1])) k.ts_ns = std::stoll(f[1]);
                if (f[2] != "-") k.market_status = f[2];
                iq = k;
            }
            const sp::SpotResolution r = sp::resolve(opt(par), iq, opt(ms), NOW_NS);
            const bool ok =
                same_opt(r.value, v) && r.source == dash(src) &&
                same_opt(r.index, idx) && same_opt(r.parity, pty) &&
                same_opt(r.index_age_s, age) &&
                (r.market_status.value_or("") == dash(status)) &&
                r.reason == dash(reason) &&
                same_opt(r.divergence, div) && same_opt(r.divergence_pct, divp) &&
                r.alarm == (alarm != 0);
            hit(T[fn], ok, line);
        } else if (fn == "index_age") {
            std::string ts, at, ms, want_age;
            int want_fresh;
            ss >> ts >> at >> ms >> want_age >> want_fresh;
            ix::IndexQuote k;
            if (!is_none(ts)) k.ts_ns = std::stoll(ts);
            const long long at_ns = std::stoll(at);
            hit(T[fn], same_opt(k.age_s(at_ns), want_age) &&
                       k.is_fresh(hx(ms), at_ns) == (want_fresh != 0), line);
        } else {
            std::getline(ss, fn);
        }
    }

    section("pricing/spot golden vectors");
    for (const auto& [k, t] : T) {
        char b[400];
        if (t.bad == 0) { std::snprintf(b, sizeof b, "%s (%ld cases)", k.c_str(), t.n); check(b, true); }
        else { std::snprintf(b, sizeof b, "%ld/%ld mismatched; first: %s", t.bad, t.n, t.worst.c_str()); check(k, false, b); }
    }

    section("the worked examples, asserted directly");
    {
        ix::IndexQuote fresh;
        fresh.value = 7712.5;
        fresh.ts_ns = NOW_NS;
        fresh.market_status = "open";
        // The index wins when it is fresh, and parity is STILL recorded -- it is the cross-check,
        // and a resolution that dropped it would throw away the only health signal on either feed.
        const auto a = sp::resolve(7700.0, fresh, sp::MISSING, NOW_NS);
        check("fresh index wins", a.source == "index" && a.value == 7712.5);
        bits("parity still recorded", *a.parity, 7700.0);
        bits("divergence is index - parity", *a.divergence, 12.5);

        // 12.5 on 7700 is 0.162%, past the INDEX_PARITY_ALARM_PCT (0.1%) alarm.
        check("divergence past the cut alarms", a.alarm);

        // Stale falls back to parity and says how stale, to one decimal.
        ix::IndexQuote stale = fresh;
        stale.ts_ns = NOW_NS - 30LL * ix::NS;
        const auto b = sp::resolve(7700.0, stale, sp::MISSING, NOW_NS);
        check("stale index falls back to parity", b.source == "parity" && b.value == 7700.0);
        check("and says how stale", b.reason == "index_stale_30.0s", b.reason);

        // A vendor stamp in the FUTURE is not fresh. Treating it as fresh would hide a clock skew
        // that also corrupts every other age check in the process.
        ix::IndexQuote future = fresh;
        future.ts_ns = NOW_NS + 5LL * ix::NS;
        const auto c = sp::resolve(7700.0, future, sp::MISSING, NOW_NS);
        check("a timestamp in the future is not fresh", c.reason == "index_timestamp_in_future");

        // No index and no parity is not a spot, and must not be zero.
        const auto d = sp::resolve(sp::MISSING, std::nullopt, sp::MISSING, NOW_NS);
        check("no index and no parity does not resolve",
              !d.value.has_value() && d.source.empty() &&
              d.reason == "no_index_quote+no_parity", d.reason);

        // parity == 0.0 leaves the divergence ABSENT rather than dividing by zero. The Python
        // guards on `parity > 0`, not on truthiness, and the two differ nowhere else.
        const auto e = sp::resolve(0.0, fresh, sp::MISSING, NOW_NS);
        check("a zero parity divides nothing", !e.divergence.has_value() && !e.alarm);
    }
    return report("test_spot");
}
