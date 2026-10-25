// Replays tests/data/golden/hourly.vec against kp::pricing::hourly::HourlyPricer.
//
// The boards come from skew_boards.vec by index, exactly as the generator drove them. The
// variance calendars are the REAL shipped artifacts, read from the reference tree -- so this
// suite is also a second, independent check that the C++ TZ+localtime_r path lands on the same
// ET instants that Python's ZoneInfo does.
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>

#include "kp/pricing/hourly.hpp"
#include "kp/pycompat/round.hpp"
#include "kp/time/et.hpp"

#include "boards.hpp"
#include "../harness.hpp"

namespace hr = kp::pricing::hourly;
namespace vc = kp::pricing::varcal;
namespace sk = kp::pricing::skew;
namespace th = kp::pricing::theo;
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

std::string skew_tag(const sk::SkewFit* f) {
    if (f == nullptr) return "None";
    return f->flat() ? "flat" : "ok";
}

}  // namespace

int main(int argc, char** argv) {
    kp::pycompat::assert_c_numeric_locale();
    kp::time::init();
    const std::string dir = (argc > 1) ? argv[1] : "tests/data/golden";
    const std::string root = (argc > 2) ? argv[2] : KP_KALSHI_ROOT_DEFAULT;

    const std::vector<RealBoard> boards = load_boards(dir + "/skew_boards.vec");
    check("loaded the captured boards", !boards.empty(),
          std::to_string(boards.size()) + " boards");

    std::map<std::string, std::unique_ptr<vc::VarianceCalendar>> cals;
    auto cal = [&](const std::string& u) -> vc::VarianceCalendar* {
        auto it = cals.find(u);
        if (it != cals.end()) return it->second.get();
        try {
            cals[u] = std::make_unique<vc::VarianceCalendar>(
                vc::VarianceCalendar::load(u, "default", root));
        } catch (...) { cals[u] = nullptr; }
        return cals[u].get();
    };

    std::ifstream in(dir + "/hourly.vec");
    if (!in) { check("open hourly.vec", false, dir); return report("test_hourly"); }
    std::string header;
    std::getline(in, header);
    check("header names pricing/hourly.py",
          header.find("source=pricing/hourly.py") != std::string::npos);

    // One pricer per (underlying, board, now, skew-mode). The reference reuses one object across
    // every settlement and strike of a case, and the lazy skew fit must therefore be built once:
    // rebuilding it per line would still be numerically identical here, but it would stop this
    // suite from exercising the cache at all.
    std::map<std::string, std::unique_ptr<hr::HourlyPricer>> cache;
    auto pricer_for = [&](const std::string& u, int bi, const std::string& now_tok,
                          const std::string& mode) -> hr::HourlyPricer* {
        const std::string key = u + "|" + std::to_string(bi) + "|" + now_tok + "|" + mode;
        auto it = cache.find(key);
        if (it != cache.end()) return it->second.get();
        vc::VarianceCalendar* c = cal(u);
        if (c == nullptr) { cache[key] = nullptr; return nullptr; }
        const RealBoard& b = boards.at((std::size_t)bi);
        auto p = std::make_unique<hr::HourlyPricer>(b.board, b.strikes, at(now_tok), *c);
        if (mode == "inst" && p->fit().ok()) {
            // The generator installs a curve refitted at w_atm * 1.05, deliberately different
            // from the lazy one so the two branches of p_above_for cannot coincide.
            p->use_skew(sk::fit(b.board, b.strikes, p->fit().forward, *p->fit().w * 1.05));
        }
        cache[key] = std::move(p);
        return cache[key].get();
    };

    std::map<std::string, Tally> T;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string fn;
        ss >> fn;
        if (fn == "meta") {
            std::string u, now, spot, w4, sig, exp4, reason;
            int bi;
            ss >> u >> bi >> now >> spot >> w4 >> sig >> exp4 >> reason;
            hr::HourlyPricer* p = pricer_for(u, bi, now, "lazy");
            if (p == nullptr) continue;
            const std::string got_reason = p->fit().reason.empty() ? "-" : p->fit().reason;
            hit(T[fn], same_opt(p->spot(), spot) && same_opt(p->w_4pm(), w4) &&
                       same_opt(p->sigma_annualized(), sig) &&
                       p->expiry_4pm_at().ns == std::strtoll(exp4.c_str(), nullptr, 10) &&
                       got_reason == reason, line);
        } else if (fn == "w") {
            std::string u, now, st, w, ratio, reason;
            int bi;
            ss >> u >> bi >> now >> st >> w >> ratio >> reason;
            hr::HourlyPricer* p = pricer_for(u, bi, now, "lazy");
            if (p == nullptr) continue;
            const std::optional<kp::time::Instant> s =
                is_none(st) ? std::nullopt : std::optional<kp::time::Instant>(at(st));
            const hr::HorizonVol& hv = p->total_vol_to(s);
            const std::string got_reason = hv.reason.empty() ? "-" : hv.reason;
            hit(T[fn], same_opt(hv.w, w) && same_opt(hv.ratio, ratio) &&
                       got_reason == reason, line);
        } else if (fn == "sigma") {
            std::string u, now, want;
            int bi;
            ss >> u >> bi >> now >> want;
            hr::HourlyPricer* p = pricer_for(u, bi, now, "lazy");
            if (p == nullptr) continue;
            hit(T[fn], same_opt(p->sigma_annualized(), want), line);
        } else if (fn == "skew") {
            std::string u, now, lazy_tag, alt_tag, alt_w;
            int bi;
            ss >> u >> bi >> now >> lazy_tag >> alt_tag >> alt_w;
            hr::HourlyPricer* p = pricer_for(u, bi, now, "lazy");
            hr::HourlyPricer* q = pricer_for(u, bi, now, "inst");
            if (p == nullptr || q == nullptr) continue;
            hit(T[fn], skew_tag(p->skew()) == lazy_tag && skew_tag(q->skew()) == alt_tag &&
                       (is_none(alt_w) ? q->skew() == nullptr
                                       : same_opt(q->skew()->w_atm(), alt_w)), line);
        } else if (fn == "p_above") {
            std::string mode, u, now, st, k, want;
            int bi;
            ss >> mode >> u >> bi >> now >> st >> k >> want;
            hr::HourlyPricer* p = pricer_for(u, bi, now, mode);
            if (p == nullptr) continue;
            hit(T[fn + ":" + mode], same_opt(p->p_above_for(at(st))(hx(k)), want), line);
        } else if (fn == "mf") {
            std::string now, styp, lo, hi, st, fair, reason, w, ratio, mins, iso;
            ss >> now >> styp >> lo >> hi >> st >> fair >> reason >> w >> ratio >> mins >> iso;
            std::string err;
            std::getline(ss, err);
            if (!err.empty() && err[0] == ' ') err.erase(0, 1);
            hr::HourlyPricer* p = pricer_for("SPX", 0, now, "lazy");
            if (p == nullptr) continue;
            th::Market m;
            m.strike_type = (styp == "-") ? std::string() : styp;
            m.floor_strike = opt(lo);
            m.cap_strike = opt(hi);
            if (!is_none(st)) m.close_time_s = std::strtoll(st.c_str(), nullptr, 10) / 1000000000LL;
            const auto [got, d] = p->market_fair(m);
            const std::string got_reason = d.bracket.reason.empty() ? "-" : d.bracket.reason;
            const std::string got_err = d.bracket.error.empty() ? "-" : d.bracket.error;
            const std::string got_iso = d.settlement ? hr::isoformat_utc(*d.settlement) : "None";
            hit(T[fn], same_opt(got, fair) && got_reason == reason && same_opt(d.w, w) &&
                       same_opt(d.var_ratio, ratio) && same_opt(d.minutes_to_settlement, mins) &&
                       got_iso == iso && got_err == err, line);
        } else {
            std::getline(ss, fn);
        }
    }

    section("pricing/hourly golden vectors");
    for (const auto& [k, t] : T) {
        char b[500];
        if (t.bad == 0) { std::snprintf(b, sizeof b, "%s (%ld cases)", k.c_str(), t.n); check(b, true); }
        else { std::snprintf(b, sizeof b, "%ld/%ld mismatched; first: %s", t.bad, t.n, t.worst.c_str()); check(k, false, b); }
    }

    section("the traps");
    {
        vc::VarianceCalendar* c = cal("SPX");
        const RealBoard& b = boards.at(0);
        if (c != nullptr) {
            const kp::time::Instant now = kp::time::from_et(2026, 8, 20, 11, 17, 3);
            hr::HourlyPricer p(b.board, b.strikes, now, *c);

            // THE INVARIANT THAT TIES THIS MODEL TO THE DAILY ONE. At settlement == 16:00 the
            // variance ratio is exactly 1.0 and the horizon vol is w_4pm with no arithmetic
            // applied -- not "within a tolerance", bit-for-bit. A calendar that quietly
            // renormalised, or a pow(1.0, 0.5) that was not the identity, shows up here and
            // nowhere else, because every other settlement has no independently-known answer.
            const kp::time::Instant close = kp::time::from_et(2026, 8, 20, 16, 0, 0);
            const hr::HorizonVol& at4 = p.total_vol_to(close);
            check("at 16:00 the variance ratio is exactly 1.0",
                  at4.ratio.has_value() && same_bits(*at4.ratio, 1.0),
                  at4.ratio ? std::to_string(*at4.ratio) : "None");
            check("...so the horizon vol IS w_4pm, bit for bit",
                  at4.w.has_value() && p.w_4pm().has_value() && same_bits(*at4.w, *p.w_4pm()));

            // Less variance left means less total vol, monotonically, and always less than the
            // 16:00 number. A sign or reciprocal slip in remaining_ratio survives the vectors
            // (they would simply be regenerated wrong) but not this.
            double prev = -1.0;
            bool rising = true, under_4pm = true;
            for (int hh = 12; hh <= 16; ++hh) {
                const auto& hv = p.total_vol_to(kp::time::from_et(2026, 8, 20, hh, 0, 0));
                if (!hv.w) { rising = false; break; }
                if (*hv.w <= prev) rising = false;
                if (hh < 16 && p.w_4pm() && *hv.w >= *p.w_4pm()) under_4pm = false;
                prev = *hv.w;
            }
            check("horizon vol rises with the settlement hour", rising);
            check("and every intraday horizon is strictly under the 16:00 vol", under_4pm);

            // Each refusal reason is reachable and distinct -- a port that collapsed them would
            // still price identically and would make every unpriced board say the same thing.
            check("a settlement already past reads 'expired'",
                  p.total_vol_to(kp::time::from_et(2026, 8, 20, 10, 0, 0)).reason == "expired");
            check("no close_time reads 'no_close_time'",
                  p.total_vol_to(std::nullopt).reason == "no_close_time");

            // The reference caches under the None key too; absence must not be a cache miss
            // every time, and must not collide with a real instant.
            check("the None key caches separately from a real instant",
                  p.total_vol_to(std::nullopt).reason == "no_close_time" &&
                  p.total_vol_to(close).reason.empty());

            // p_above off a horizon vol is a probability, decreasing in strike.
            {
                auto f = p.p_above_for(kp::time::from_et(2026, 8, 20, 15, 0, 0));
                bool ok = true, decreasing = true;
                double last = 2.0;
                for (double k : b.strikes) {
                    const auto v = f(k);
                    if (!v) continue;
                    if (*v < 0.0 || *v > 1.0) ok = false;
                    if (*v > last) decreasing = false;
                    last = *v;
                }
                check("hourly digitals stay in [0, 1]", ok);
                check("and are non-increasing in strike after the PAVA projection", decreasing);
            }

            // The 16:00 ET expiry is 20:00 UTC in August (EDT, UTC-4), which is the one place
            // the port's TZ handling could silently differ from Python's ZoneInfo.
            check("expiry_4pm on an August date is 20:00 UTC",
                  hr::isoformat_utc(p.expiry_4pm_at()) == "2026-08-20T20:00:00+00:00",
                  hr::isoformat_utc(p.expiry_4pm_at()));

            // The error string embeds Python's isoformat, which renders +00:00 and never "Z".
            {
                th::Market m;
                m.strike_type = "greater";
                m.floor_strike = 7700.0;
                m.close_time_s = kp::time::from_et(2026, 8, 20, 10, 0, 0).ns / 1000000000LL;
                const auto [fair, d] = p.market_fair(m);
                check("an expired market prices to nothing and says why",
                      !fair.has_value() && d.bracket.reason == "expired");
                check("and the error carries the isoformat, +00:00 not Z",
                      d.bracket.error == "no vol for settlement 2026-08-20T14:00:00+00:00: expired",
                      d.bracket.error);
            }
        }
    }

    return report("test_hourly");
}
