// rvol/features against the reference, and the numpy arithmetic beneath it: SVML log, pairwise
// sum, mean, BLAS dot, the millisecond and wall-clock conversions, variance_time, averaged_rv on
// synthetic paths and window_sigma on eight sessions of real cached second bars.
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "kp/pricing/varcal.hpp"
#include "kp/rvol/features.hpp"
#include "kp/rvol/npcompat.hpp"
#include "vecio.hpp"

using namespace kp::test::vec;
namespace F = kp::rvol::features;
namespace np = kp::rvol::np;
using kp::time::Instant;

namespace {
struct Series { std::vector<std::int64_t> t; std::vector<double> p; };

std::vector<double> hxs(const std::vector<std::string>& w, std::size_t from, std::size_t n) {
    std::vector<double> v;
    for (std::size_t i = 0; i < n; ++i) v.push_back(hx(w[from + i]));
    return v;
}
}  // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "tests/data/golden";
    const std::string root = argc > 2 ? argv[2] : "";
    kp::time::init();
    Tally T("rvol_features");
    T.expect({"nplog", "npsum", "npmean", "npdot", "ms", "wallminus", "annualized", "vt",
              "series", "arv", "ws"});
    if (!np::x86_v4()) {
        std::printf("NOTE: this CPU lacks numpy's X86_V4 feature set; np.log is glibc here and "
                    "the vectors (taken on an AVX-512 host) will not all match.\n");
    }

    std::map<std::string, kp::pricing::varcal::VarianceCalendar> cals;
    auto cal = [&](const std::string& u) -> const kp::pricing::varcal::VarianceCalendar& {
        auto it = cals.find(u);
        if (it == cals.end())
            it = cals.emplace(u, kp::pricing::varcal::VarianceCalendar::load(u, "default", root)).first;
        return it->second;
    };
    std::map<int, Series> series;
    Series* filling = nullptr;

    for (const auto& line : read(dir, "rvol_features")) {
        const auto w = words(line);
        const std::string& fn = w[0];
        if (fn == "pt") {
            filling->t.push_back(std::stoll(w[1]));
            filling->p.push_back(hx(w[2]));
        } else if (fn == "series") {
            filling = &series[std::stoi(w[1])];
            filling->t.reserve(std::stoul(w[2]));
            T.hit(fn, true, line);
        } else if (fn == "nplog") {
            const double x = hx(w[1]);
            // Raw bits, so the SIGN of a NaN is pinned too (np.log(-1) is -nan).
            const double got = np::log(std::vector<double>{x})[0];
            std::uint64_t gb;
            std::memcpy(&gb, &got, 8);
            T.hit(fn, gb == std::stoull(w[2], nullptr, 16), line);
        } else if (fn == "npsum") {
            const std::size_t n = std::stoul(w[1]);
            T.hit(fn, same(np::sum(hxs(w, 3, n)), w[2]), line);
        } else if (fn == "npmean") {
            const std::size_t n = std::stoul(w[1]);
            T.hit(fn, same(np::mean(hxs(w, 3, n)), w[2]), line);
        } else if (fn == "npdot") {
            T.hit(fn, same(np::dot(hxs(w, 2, 7), hxs(w, 9, 7)), w[1]), line);
        } else if (fn == "ms") {
            T.hit(fn, F::ms(Instant{std::stoll(w[1])}) == std::stoll(w[2]), line);
        } else if (fn == "wallminus") {
            T.hit(fn, F::wall_minus_minutes(Instant{std::stoll(w[1])}, std::stoi(w[2])).ns
                          == std::stoll(w[3]), line);
        } else if (fn == "annualized") {
            T.hit(fn, same(F::annualized(hx(w[1])), w[2]), line);
        } else if (fn == "vt") {
            const double got = F::variance_time(cal(w[1]), Instant{std::stoll(w[2])},
                                                Instant{std::stoll(w[3])}, nullptr, root);
            T.hit(fn, same(got, w[4]), line);
        } else if (fn == "arv") {
            const Series& s = series.at(std::stoi(w[1]));
            const auto r = F::averaged_rv(s.t, s.p, std::stoll(w[2]), std::stoll(w[3]),
                                          std::stoi(w[4]), std::stoi(w[5]));
            T.hit(fn, same(r.rv, w[6]) && same(r.coverage, w[7]), line);
        } else if (fn == "ws") {
            const Series& si = series.at(std::stoi(w[2]));
            const Series& sp = series.at(std::stoi(w[3]));
            const auto r = F::window_sigma(cal(w[1]), si.t, si.p, sp.t, sp.p,
                                           Instant{std::stoll(w[4])}, Instant{std::stoll(w[5])},
                                           std::stoi(w[6]), nullptr, root);
            T.hit(fn, same(r.sigma, w[7]) && same(r.proxy_frac, w[8]) && same(r.coverage, w[9]),
                  line);
        } else {
            T.hit("unknown:" + fn, false, line);
        }
    }
    return T.report();
}
