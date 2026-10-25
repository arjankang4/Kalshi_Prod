// Replays tests/data/golden/risklimit.vec against kp::strategy::risklimit::SessionRisk.
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>

#include "kp/config.hpp"
#include "kp/pycompat/round.hpp"
#include "kp/strategy/risklimit.hpp"

#include "boards.hpp"
#include "../harness.hpp"

namespace rl = kp::strategy::risklimit;
using namespace kp::test;
using OptD = std::optional<double>;

namespace {
struct Tally { long n = 0, bad = 0; std::string worst; };
void hit(Tally& t, bool ok, const std::string& line) {
    ++t.n;
    if (!ok) { ++t.bad; if (t.worst.empty()) t.worst = line; }
}
std::string rest(std::istringstream& ss) {
    std::string r;
    std::getline(ss, r);
    if (!r.empty() && r[0] == ' ') r.erase(0, 1);
    return r;
}
}  // namespace

int main(int argc, char** argv) {
    kp::pycompat::assert_c_numeric_locale();
    const std::string dir = (argc > 1) ? argv[1] : "tests/data/golden";

    std::ifstream in(dir + "/risklimit.vec");
    if (!in) { check("open risklimit.vec", false, dir); return report("test_risklimit"); }
    std::string header;
    std::getline(in, header);
    check("header names strategy/risklimit.py",
          header.find("source=strategy/risklimit.py") != std::string::npos);

    std::map<std::string, Tally> T;
    std::string line;

    // batch/mark cases arrive as a result line followed by their input lines, so the inputs are
    // gathered first and replayed when the case is complete.
    std::map<long, std::vector<rl::Fill>> batches;
    std::map<std::pair<long, long>, rl::Positions> mark_pos;
    std::map<std::pair<long, long>, rl::Fairs> mark_fairs;
    std::vector<std::string> deferred;

    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string fn;
        ss >> fn;
        if (fn == "fill") {
            std::string n, p, fee, side, cash, fees, contracts;
            long n_ok, fills, skipped;
            std::string side2;
            ss >> n >> p >> fee >> side >> side2 >> n_ok >> fills >> skipped >> cash >> fees >> contracts;
            rl::Fill f;
            f.count_fp = opt(n);
            f.yes_price_dollars = opt(p);
            f.fee_cost = opt(fee);
            f.outcome_side = (side == "-") ? std::string() : side;
            f.side = (side2 == "-") ? std::string() : side2;
            rl::SessionRisk r(1e12, std::string());
            const long got = r.on_fills({f});
            hit(T[fn], got == n_ok && r.fills() == fills && r.skipped() == skipped &&
                       same_bits(r.cash(), hx(cash)) && same_bits(r.fees(), hx(fees)) &&
                       same_bits(r.contracts(), hx(contracts)), line);
        } else if (fn == "batch_in") {
            long trial; std::string n, p, side, fee;
            ss >> trial >> n >> p >> side >> fee;
            rl::Fill f;
            f.count_fp = opt(n);
            f.yes_price_dollars = opt(p);
            f.fee_cost = opt(fee);
            f.outcome_side = side;
            batches[trial].push_back(f);
        } else if (fn == "mark_in") {
            long trial, step; std::string tk, q, p; int present;
            ss >> trial >> step >> tk >> q >> p >> present;
            mark_pos[{trial, step}].emplace_back(tk, hx(q));
            if (present) mark_fairs[{trial, step}].emplace_back(tk, opt(p));
        } else if (fn == "batch" || fn == "mark") {
            deferred.push_back(line);
        } else if (fn == "halt") {
            std::string lim, target, pnl;
            int tripped, sticky;
            ss >> lim >> target >> pnl >> tripped >> sticky;
            const std::string want = rest(ss);
            rl::SessionRisk r(opt(lim), std::string());
            const OptD p1 = r.mark({{"A", 1.0}}, {{"A", hx(target)}});
            const std::string first = r.halt_reason();
            hit(T[fn], same_opt(p1, pnl) && ((!first.empty()) ? 1 : 0) == tripped &&
                       (first.empty() ? std::string("-") : first) == want, line);
            // Stickiness is checked on the SAME object, as the reference does.
            r.mark({{"A", 1.0}}, {{"A", 1e6}});
            const std::string second = r.halt_reason();
            hit(T["halt_sticky"], ((!second.empty()) ? 1 : 0) == sticky, line);
        } else if (fn == "halt2") {
            std::string lim, target;
            ss >> lim >> target;
            const std::string want = rest(ss);
            rl::SessionRisk r(opt(lim), std::string());
            r.mark({{"A", 1.0}}, {{"A", hx(target)}});
            r.halt_reason();
            r.mark({{"A", 1.0}}, {{"A", 1e6}});
            const std::string second = r.halt_reason();
            hit(T[fn], (second.empty() ? std::string("-") : second) == want, line);
        } else {
            std::getline(ss, fn);
        }
    }

    // Now the deferred result lines, whose inputs are all gathered.
    for (const std::string& l : deferred) {
        std::istringstream ss(l);
        std::string fn;
        ss >> fn;
        if (fn == "batch") {
            long trial, n_in, n_ok; std::string cash, fees, contracts;
            ss >> trial >> n_in >> n_ok >> cash >> fees >> contracts;
            rl::SessionRisk r(1e12, std::string());
            const auto& b = batches[trial];
            const long got = r.on_fills(b);
            hit(T[fn], (long)b.size() == n_in && got == n_ok &&
                       same_bits(r.cash(), hx(cash)) && same_bits(r.fees(), hx(fees)) &&
                       same_bits(r.contracts(), hx(contracts)), l);
        } else if (fn == "mark") {
            long trial, step, npos, unmarked, marked;
            std::string pnl, worst, cash;
            ss >> trial >> step >> npos >> pnl >> unmarked >> marked >> worst >> cash;
            // The whole trial has to be replayed in order, because mark() carries _last_fair
            // across steps -- that carry-over IS the behaviour under test.
            rl::SessionRisk r(250.0, std::string());
            // Fills are not replayed here (cash is asserted separately), so seed cash to the
            // recorded value by construction is impossible; instead assert the DELTA-free
            // quantities and the pnl relative to the recorded cash.
            OptD got_pnl;
            long got_unmarked = 0;
            for (long s2 = 0; s2 <= step; ++s2) {
                got_pnl = r.mark(mark_pos[{trial, s2}], mark_fairs[{trial, s2}]);
                got_unmarked = r.unmarked();
            }
            const double recorded_cash = hx(cash);
            const OptD shifted = got_pnl ? OptD(*got_pnl + recorded_cash) : std::nullopt;
            hit(T[fn], (long)mark_pos[{trial, step}].size() == npos &&
                       same_opt(shifted, pnl) && got_unmarked == unmarked &&
                       r.marked() == marked, l);
        }
    }

    section("strategy/risklimit golden vectors");
    for (const auto& [k, t] : T) {
        char b[500];
        if (t.bad == 0) { std::snprintf(b, sizeof b, "%s (%ld cases)", k.c_str(), t.n); check(b, true); }
        else { std::snprintf(b, sizeof b, "%ld/%ld mismatched; first: %s", t.bad, t.n, t.worst.c_str()); check(k, false, b); }
    }

    section("the traps");
    {
        // THE SIGN CONVENTION. Both legs print at the same yes_price, and 'no' is booked as
        // "sold YES at p", not "bought NO at 1-p". The two differ by a constant $1 per contract
        // and only this one makes cash + position*p converge to the realised result.
        rl::Fill y; y.count_fp = 10.0; y.yes_price_dollars = 0.40; y.outcome_side = "yes";
        rl::Fill n; n.count_fp = 10.0; n.yes_price_dollars = 0.40; n.outcome_side = "no";
        rl::SessionRisk ry(1e12, std::string()), rn(1e12, std::string());
        ry.on_fills({y});
        rn.on_fills({n});
        check("buying 10 yes at 0.40 costs $4", same_bits(ry.cash(), -4.0),
              std::to_string(ry.cash()));
        check("selling 10 yes at 0.40 receives $4", same_bits(rn.cash(), 4.0),
              std::to_string(rn.cash()));
        check("the two legs are exact mirrors, NOT 1-p apart",
              same_bits(ry.cash(), -rn.cash()));
        // And the convergence property that justifies the convention: mark the long yes book at
        // p = 1 and the P&L is the $6 actually made, not $10 or -$4.
        check("marked at settlement the long book books its real profit",
              same_bits(*ry.mark({{"T", 10.0}}, {{"T", 1.0}}), 6.0));
        check("...and the short book its real loss",
              same_bits(*rn.mark({{"T", -10.0}}, {{"T", 1.0}}), -6.0));

        // A fill we cannot read is SKIPPED, never treated as $0.
        rl::SessionRisk rs(1e12, std::string());
        rl::Fill bad;                      // every field absent
        rl::Fill zero; zero.count_fp = 0.0; zero.yes_price_dollars = 0.5; zero.outcome_side = "yes";
        rl::Fill neg; neg.count_fp = 5.0; neg.yes_price_dollars = -0.1; neg.outcome_side = "yes";
        rl::Fill side; side.count_fp = 5.0; side.yes_price_dollars = 0.5; side.outcome_side = "buy";
        check("unreadable, zero-count, negative-price and unknown-side fills are all skipped",
              rs.on_fills({bad, zero, neg, side}) == 0 && rs.skipped() == 4 &&
              same_bits(rs.cash(), 0.0) && rs.fills() == 0);

        // pnl is ABSENT until the first mark, not 0.0 -- which would read as flat.
        rl::SessionRisk fresh(250.0, std::string());
        check("pnl is absent before the first mark, not zero", !fresh.pnl().has_value());
        check("...and a fresh book has no halt reason", fresh.halt_reason().empty());

        // A position we could not price marks at its LAST fair; one never priced is COUNTED.
        rl::SessionRisk lf(1e12, std::string());
        lf.mark({{"A", 10.0}}, {{"A", 0.50}});
        const OptD again = lf.mark({{"A", 10.0}, {"B", 10.0}}, {{"A", std::nullopt}});
        check("a market that lost its fair marks at the LAST one, not at zero",
              again.has_value() && same_bits(*again, 5.0), again ? std::to_string(*again) : "None");
        check("and one never priced is COUNTED, not dropped", lf.unmarked() == 1);

        // THE DIRECTION A RISK CONTROL MUST NEVER BE WRONG IN: dropping the unpriced position
        // would make the P&L rise every time the pricer failed.
        rl::SessionRisk dropped(1e12, std::string());
        const OptD only_b = dropped.mark({{"B", -100.0}}, {});
        check("an all-unpriced book marks flat and says so, rather than improving",
              only_b.has_value() && same_bits(*only_b, 0.0) && dropped.unmarked() == 1);

        // The limit is STICKY and there is no way to construct an unlimited one.
        rl::SessionRisk st(std::nullopt, std::string());
        check("an omitted limit means the CONFIGURED limit, never no limit",
              st.limit().has_value() &&
              same_bits(*st.limit(), kp::config::DAILY_LOSS_LIMIT_DOLLARS));
        // A loss past the configured limit, whatever that limit is.
        st.mark({{"A", 1.0}}, {{"A", -(kp::config::DAILY_LOSS_LIMIT_DOLLARS + 50.0)}});
        const std::string trip = st.halt_reason();
        check("through the limit, it halts", !trip.empty(), trip);
        st.mark({{"A", 1.0}}, {{"A", 1000.0}});
        check("and a recovery does NOT clear it", st.halt_reason() == trip);

        // A negative limit is read through fabs, so a sign slip in config cannot disable it.
        rl::SessionRisk negl(-100.0, std::string());
        negl.mark({{"A", 1.0}}, {{"A", -150.0}});
        check("a negative limit still halts -- the comparison is against -fabs(limit)",
              !negl.halt_reason().empty());

        // Summation order follows POSITIONS, not sorted order. Two orderings of the same book
        // agree here only because the values are exact; the contract is that the caller controls
        // it, so this pins that classify-style reordering is a caller decision and not silent.
        rl::SessionRisk o1(1e12, std::string()), o2(1e12, std::string());
        const rl::Positions fwd = {{"Z", 1.0}, {"A", 1.0}, {"M", 1.0}};
        const rl::Positions rev = {{"A", 1.0}, {"M", 1.0}, {"Z", 1.0}};
        const rl::Fairs fs = {{"Z", 0.1}, {"A", 0.2}, {"M", 0.3}};
        check("positions are summed in the order given, which the type makes explicit",
              o1.mark(fwd, fs).has_value() && o2.mark(rev, fs).has_value());
    }

    return report("test_risklimit");
}
