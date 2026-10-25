#include "kp/pricing/universe.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

#include "kp/config.hpp"
#include "kp/pycompat/round.hpp"

namespace kp::pricing::universe {

skew::Board quotes_by_strike(const kp::massive::OptionSnapshot& snapshot, const std::string& root,
                             const std::string& expiry, const std::vector<double>& strikes) {
    skew::Board out;
    for (double k : strikes) {
        for (char right : {'C', 'P'}) {
            const auto it = snapshot.find(occ_ticker(root, expiry, right, k));
            if (it != snapshot.end() && it->second.q.present) out[{right, k}] = it->second.q;
        }
    }
    return out;
}

std::string occ_ticker(const std::string& root, const std::string& expiry_yymmdd,
                       char right, double strike) {
    // Python: f"O:{root}{expiry}{right}{round(strike * 1000):08d}". py_round_i, not lround --
    // round() is half-to-even, and a strike ending .0005 is not hypothetical on an NDX grid.
    char buf[32];
    std::snprintf(buf, sizeof buf, "%08lld", (long long)kp::pycompat::py_round_i(strike * 1000.0));
    return "O:" + root + expiry_yymmdd + std::string(1, right) + buf;
}

std::set<double> bracketing_strikes(const std::vector<theo::Market>& markets,
                                    const std::vector<double>& strikes) {
    std::set<double> needed;
    for (double b : theo::boundaries(markets)) {
        const theo::Neighbors n = theo::neighbors(b, strikes);
        if (!n.below) {
            char buf[128];
            std::snprintf(buf, sizeof buf, "no listed strikes bracketing boundary %g", b);
            throw std::runtime_error(buf);
        }
        needed.insert(*n.below);
        // Python does `needed.update((lo, hi))` after checking only `lo is None`, so a boundary
        // above the highest listed strike inserts `hi = None` -- which in Python is a None IN the
        // set, and it then reaches sorted() and blows up on the float/None comparison. Absent is
        // simply not inserted here; the guard above is the one that fires in practice, because a
        // grid that brackets from below on a symmetric board also brackets from above.
        if (n.above) needed.insert(*n.above);
    }
    return needed;
}

std::set<double> parity_band(const std::vector<double>& strikes, OptD centre, OptD band_pct) {
    const double bp = band_pct ? *band_pct : kp::config::QUOTE_BAND_PCT;
    if (!centre) return {};
    const double lo = *centre * (1.0 - bp), hi = *centre * (1.0 + bp);
    std::set<double> out;
    for (double s : strikes) if (lo <= s && s <= hi) out.insert(s);
    return out;
}

Desired desired_tickers(const std::vector<theo::Market>& markets,
                        const std::vector<double>& strikes,
                        const std::string& root, const std::string& expiry,
                        OptD spot, OptD band_pct, std::optional<int> cap) {
    const int c = cap ? *cap : MAX_TICKERS;
    const std::set<double> pinned = bracketing_strikes(markets, strikes);
    const std::set<double> band = parity_band(strikes, spot, band_pct);

    std::set<double> want = pinned;
    want.insert(band.begin(), band.end());

    if ((int)want.size() * 2 > c) {
        // Trim the band from the OUTSIDE IN. The pinned set is never sacrificed: losing a
        // bracketing strike silently unprices a market, whereas a narrower band only weakens the
        // spot estimate.
        const double centre = spot ? *spot
                                   : (*pinned.begin() + *pinned.rbegin()) / 2.0;
        std::vector<double> extra;
        for (double s : band) if (!pinned.count(s)) extra.push_back(s);
        // Python sorts by distance alone over a set, so an exact tie is resolved by CPython's set
        // iteration order. Tie-break on the strike so this is deterministic; see the header for
        // why the two cannot disagree here.
        std::stable_sort(extra.begin(), extra.end(), [&](double a, double b) {
            const double da = std::fabs(a - centre), db = std::fabs(b - centre);
            if (da != db) return da < db;
            return a < b;
        });
        const int room = std::max(0, c / 2 - (int)pinned.size());
        want = pinned;
        for (int i = 0; i < room && i < (int)extra.size(); ++i) want.insert(extra[(std::size_t)i]);
    }

    Desired out;
    out.strikes.assign(want.begin(), want.end());
    out.tickers.reserve(out.strikes.size() * 2);
    for (double k : out.strikes)
        for (char r : {'C', 'P'}) out.tickers.push_back(occ_ticker(root, expiry, r, k));
    return out;
}

}  // namespace kp::pricing::universe
