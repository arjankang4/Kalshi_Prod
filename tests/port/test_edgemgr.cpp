// strategy/edgemgr against the reference: every record field on SPX and NDX at four times of day,
// the adverse gate across five move scales, the five no-R refusals, the operator override, the
// unknown-ticker record, the EDGE_RECOMPUTE_S cache on its exact boundary, and the unavailable manager.
#include <map>
#include <string>
#include <vector>

#include "kp/pricing/edge.hpp"
#include "kp/pricing/varcal.hpp"
#include "kp/strategy/edgemgr.hpp"
#include "vecio.hpp"

using namespace kp::test::vec;
namespace em = kp::strategy::edgemgr;
namespace th = kp::pricing::theo;

namespace {

bool rec_ok(const em::EdgeRecord& r, const std::vector<std::string>& w, std::size_t i) {
    // edge_micros edge risk_u scale_pos R delta vega theta w var_left mtc quotable source reason
    // floor_micros bonus_micros adverse_dollars move_scale
    return same(r.edge_micros, w[i]) && same(r.edge, w[i + 1]) && same(r.risk_u, w[i + 2])
        && same(r.scale_pos, w[i + 3]) && same(r.R, w[i + 4]) && same(r.delta, w[i + 5])
        && same(r.vega, w[i + 6]) && same(r.theta, w[i + 7]) && same(r.w, w[i + 8])
        && same(r.var_left, w[i + 9]) && same(r.minutes_to_close, w[i + 10])
        && (r.quotable ? "1" : "0") == w[i + 11] && r.source == ostr(w[i + 12])
        && r.reason == ostr(w[i + 13]) && same(r.floor_micros, w[i + 14])
        && same(r.bonus_micros, w[i + 15]) && same(r.adverse_dollars, w[i + 16])
        && same(r.move_scale, w[i + 17]);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "tests/data/golden";
    const std::string root = argc > 2 ? argv[2] : "";
    kp::time::init();
    Tally T("edgemgr");
    T.expect({"market", "rec", "summary", "noinput", "override", "unknown", "cache",
              "unavailable", "unavailable_rec"});

    std::vector<th::Market> markets;
    std::map<std::string, em::EdgeManager> mgrs;   // current run tag -> manager
    std::map<std::string, std::pair<kp::pricing::varcal::VarianceCalendar,
                                    kp::pricing::edge::Curve>> art;
    auto artifacts = [&](const std::string& u) -> auto& {
        auto it = art.find(u);
        if (it == art.end())
            it = art.emplace(u, std::make_pair(kp::pricing::varcal::VarianceCalendar::load(u, "default", root),
                                               kp::pricing::edge::Curve::load(u, root))).first;
        return it->second;
    };
    auto fresh = [&](const std::string& u) {
        auto& a = artifacts(u);
        return em::EdgeManager(u, a.first, a.second, std::nullopt, root);
    };
    const kp::time::Instant noon = kp::time::from_et(2026, 8, 20, 12, 0);
    std::optional<em::EdgeManager> cache_mgr;

    for (const auto& line : read(dir, "edgemgr")) {
        const auto w = words(line);
        const std::string& fn = w[0];
        if (fn == "market") {
            th::Market m;
            m.ticker = w[1];
            m.strike_type = ostr(w[2]);
            m.floor_strike = ohx(w[3]);
            m.cap_strike = ohx(w[4]);
            if (!none(w[5])) m.close_time_s = std::stoll(w[5]);
            markets.push_back(m);
            T.hit(fn, true, line);
        } else if (fn == "run") {
            const std::string tag = w[1];
            const std::string u = tag.substr(0, 3);
            auto mgr = fresh(u);
            mgr.refresh(markets, hx(w[3]), hx(w[4]), kp::time::Instant{std::stoll(w[2])}, 0.0, true,
                        ohx(w[5]));
            mgrs.erase(tag);
            mgrs.emplace(tag, std::move(mgr));
        } else if (fn == "rec") {
            const auto it = mgrs.find(w[1]);
            T.hit(fn, it != mgrs.end() && rec_ok(it->second.get(w[3]), w, 4), line);
        } else if (fn == "summary") {
            const auto it = mgrs.find(w[1]);
            if (it == mgrs.end()) { T.hit(fn, false, line); continue; }
            const em::EdgeSummary s = it->second.summary();
            T.hit(fn, std::to_string(s.n) == w[2] && std::to_string(s.n_priced) == w[3]
                      && std::to_string(s.refused) == w[4] && same(s.edge_min, w[5])
                      && same(s.edge_med, w[6]) && same(s.edge_max, w[7])
                      && (s.enabled ? "1" : "0") == w[8] && s.reason == ostr(w[9])
                      && (s.priced ? "1" : "0") == w[10], line);
        } else if (fn == "noinput") {
            auto mgr = fresh(w[1]);
            mgr.refresh(markets, ohx(w[2]), ohx(w[3]), noon, 0.0, true);
            const auto r = mgr.get("D0");
            T.hit(fn, (r.quotable ? "1" : "0") == w[4] && r.source == w[5] && r.reason == w[6]
                      && same(r.edge_micros, w[7]) && same(r.risk_u, w[8])
                      && same(r.floor_micros, w[9]) && same(r.bonus_micros, w[10]), line);
        } else if (fn == "override" || fn == "unknown") {
            auto mgr = fresh(w[1]);
            mgr.set_operator_flat(w[2] == "1");
            mgr.refresh(markets, std::nullopt, std::nullopt, noon, 0.0, true);
            if (fn == "override") {
                const auto r = mgr.get("D0");
                T.hit(fn, (r.quotable ? "1" : "0") == w[3] && r.source == w[4] && r.reason == w[5]
                          && same(r.edge_micros, w[6]), line);
            } else {
                const auto r = mgr.get("NOSUCHTICKER");
                T.hit(fn, r.reason == w[3] && (r.quotable ? "1" : "0") == w[4], line);
            }
        } else if (fn == "cache") {
            if (!cache_mgr) cache_mgr.emplace(fresh("SPX"));
            cache_mgr->refresh(markets, hx(w[3]), 0.12, kp::time::Instant{std::stoll(w[4])},
                               hx(w[1]), w[2] == "1");
            const auto r = cache_mgr->get("D0");
            T.hit(fn, same(r.edge_micros, w[5]) && same(r.R, w[6]) && same(r.delta, w[7]), line);
        } else if (fn == "unavailable") {
            em::EdgeManager mgr(w[1], std::nullopt, std::nullopt, std::nullopt, root);
            std::string reason = w[4];
            for (std::size_t i = 5; i < w.size(); ++i) reason += " " + w[i];
            T.hit(fn, (mgr.available() ? "1" : "0") == w[2] && (mgr.enabled() ? "1" : "0") == w[3]
                      && mgr.reason() == reason, line);
        } else if (fn == "unavailable_rec") {
            em::EdgeManager mgr(w[1], std::nullopt, std::nullopt, std::nullopt, root);
            mgr.refresh(markets, 7691.1, 0.12, noon, 0.0, true);
            const auto r = mgr.get("D0");
            std::string reason = w[4];
            for (std::size_t i = 5; i < w.size(); ++i) reason += " " + w[i];
            T.hit(fn, (r.quotable ? "1" : "0") == w[2] && r.source == w[3] && r.reason == reason, line);
        } else {
            T.hit("unknown_tag:" + fn, false, line);
        }
    }
    return T.report();
}
