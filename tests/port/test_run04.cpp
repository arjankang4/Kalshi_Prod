// The pure half of run_04: verdict (every outcome and reason string), _still_open, _sigma on the
// real SPX/NDX calendars, and the config clock helpers across DST and the session edges.
#include <map>
#include <string>
#include <vector>

#include "kp/app/run04.hpp"
#include "vecio.hpp"

using namespace kp::test::vec;
namespace r4 = kp::app::run04;

namespace {
std::vector<std::string> tabs(const std::string& s) {
    std::vector<std::string> out;
    std::size_t p0 = 0;
    for (std::size_t p = 0; p <= s.size(); ++p)
        if (p == s.size() || s[p] == '\t') { out.push_back(s.substr(p0, p - p0)); p0 = p + 1; }
    return out;
}
}  // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "tests/data/golden";
    const std::string root = argc > 2 ? argv[2] : "";
    kp::time::init();
    Tally T("run04");
    T.expect({"verdict", "still_open", "sigma", "sigma_none", "clock"});
    std::map<std::string, kp::pricing::varcal::VarianceCalendar> cals;
    for (const auto& line : read(dir, "run04")) {
        const auto f = tabs(line);
        const std::string& fn = f[0];
        if (fn == "verdict") {
            r4::RunStats st{std::stol(f[1]), std::stol(f[2]), std::stol(f[3]), std::stol(f[4])};
            const auto d = r4::verdict(st, hx(f[5]), f[6] == "1");
            std::string reasons;
            for (std::size_t i = 0; i < d.reasons.size(); ++i) reasons += (i ? "|" : "") + d.reasons[i];
            T.hit(fn, d.verdict == f[7] && std::to_string(d.exit_code) == f[8]
                      && std::to_string(d.expected) == f[9] && same(d.error_rate, f[10])
                      && same(d.spot_rate, f[11]) && reasons == f[12]
                      && r4::pct0(d.error_rate) == f[13] && r4::pct0(d.spot_rate) == f[14], line);
        } else if (fn == "still_open") {
            kp::pricing::theo::Market m;
            m.ticker = "T";
            if (f[2] != "None") m.close_time = f[2];
            T.hit(fn, std::to_string(r4::still_open({m}, std::stoll(f[1])).size()) == f[3], line);
        } else if (fn == "sigma" || fn == "sigma_none") {
            auto it = cals.find(f[1]);
            if (it == cals.end())
                it = cals.emplace(f[1], kp::pricing::varcal::VarianceCalendar::load(f[1], "default", root)).first;
            const kp::time::Instant t{std::stoll(f[2])};
            if (fn == "sigma_none") { T.hit(fn, !r4::sigma(nullptr, it->second, t) && f[3] == "None", line); continue; }
            kp::pricing::atmvol::AtmFit fit;
            if (f[3] == "1") { fit.forward = 1.0; fit.w = hx(f[4]); }
            else { fit.reason = "not_ok"; fit.w = hx(f[4]); }
            T.hit(fn, same(r4::sigma(&fit, it->second, t), f[5]), line);
        } else if (fn == "clock") {
            const kp::time::Instant t{std::stoll(f[1])};
            T.hit(fn, (r4::in_trading_session(t) ? "1" : "0") == f[2] && r4::run_stamp(t) == f[3]
                      && r4::occ_expiry(t) == f[4] && r4::board_stamp(t) == f[5], line);
        } else {
            T.hit("unknown:" + fn, false, line);
        }
    }
    return T.report();
}
