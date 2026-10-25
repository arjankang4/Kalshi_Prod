// strategy/risktape against the reference: every row as json text AND the bytes the Journal wrote
// for it, the cadence on its boundary, detail(), and the pure helpers.
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "kp/json/value.hpp"
#include "kp/pricing/pricer.hpp"
#include "kp/pricing/skew.hpp"
#include "kp/pricing/varcal.hpp"
#include "kp/strategy/journal.hpp"
#include "kp/strategy/risktape.hpp"
#include "tapes_io.hpp"
#include "vecio.hpp"

using namespace kp::test::vec;
using namespace kp::test::tapes;
namespace rt = kp::strategy::risktape;
namespace jr = kp::strategy::journal;
namespace em = kp::strategy::edgemgr;
namespace sk = kp::pricing::skew;
namespace th = kp::pricing::theo;

namespace {

std::string slurp(const std::string& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "tests/data/golden";
    const std::string root = argc > 2 ? argv[2] : "";
    kp::time::init();
    Tally T("risktape");
    T.expect({"board", "mkt", "em", "fit", "tape", "due", "snap", "det", "vcd", "bkt", "rs"});

    char tmpl[] = "/tmp/kp_risktape_XXXXXX";
    const char* td = ::mkdtemp(tmpl);
    if (!td) { std::printf("FATAL: mkdtemp\n"); return 1; }
    auto j = jr::open_journal(td, "20260821T143000Z", "risk.jsonl");

    sk::Board board;
    std::vector<double> strikes;
    std::vector<th::Market> daily, hourly, markets;
    std::map<int, em::EdgeMap> sets;
    const auto cal = kp::pricing::varcal::VarianceCalendar::load("SPX", "default", root);
    double F = 0, w_fit = 0, w_atm = 0;
    std::optional<sk::SkewFit> fitted;
    std::optional<rt::RiskTape> tape;

    for (const auto& line : read(dir, "risktape")) {
        const auto parts = tabs(line);
        const auto w = words(parts[0]);
        const std::string& fn = w[0];
        if (fn == "board") {
            const std::size_t n = std::stoul(w[1]);
            for (std::size_t i = 0; i < n; ++i) add_leg(board, w[2 + i]);
            for (const auto& [k, q] : board)
                if (strikes.empty() || strikes.back() != k.strike) strikes.push_back(k.strike);
            std::sort(strikes.begin(), strikes.end());
            strikes.erase(std::unique(strikes.begin(), strikes.end()), strikes.end());
            T.hit(fn, true, line);
        } else if (fn == "mkt") {
            const auto m = th::market_from_json(kp::json::parse(parts[1]));
            (w[1] == "daily" ? daily : hourly).push_back(m);
            markets.push_back(m);
            T.hit(fn, true, line);
        } else if (fn == "em") {
            const auto f = words(parts[1]);
            em::EdgeRecord r;
            r.delta = ohx(f[1]);
            r.vega = ohx(f[2]);
            r.theta = ohx(f[3]);
            sets[std::stoi(w[1])].set(f[0], r);
            T.hit(fn, true, line);
        } else if (fn == "fit") {
            F = hx(w[1]); w_fit = hx(w[2]); w_atm = hx(w[3]);
            fitted = sk::fit(board, strikes, F, w_fit);
            T.hit(fn, true, line);
        } else if (fn == "tape") {
            tape.emplace(j.get(), ohx(w[1]));
            T.hit(fn, true, line);
        } else if (fn == "due") {
            T.hit(fn, (tape->due(hx(w[1])) ? "1" : "0") == w[2], line);
        } else if (fn == "snap") {
            const kp::time::Instant now{std::stoll(w[2])};
            const long long ts = std::stoll(w[3]);
            const double mono = hx(w[4]);
            const bool write = w[5] == "1";
            const std::string& pm = w[6];
            std::optional<kp::pricing::pricer::Pricer> pr;
            if (pm == "lazy") pr.emplace(board, strikes, F);
            else if (pm == "lazyoff") pr.emplace(board, strikes, F + 11.5);
            else if (pm == "inst") { pr.emplace(board, strikes, F); pr->use_skew(*fitted); }
            else if (pm == "flat") {
                pr.emplace(board, strikes, F);
                pr->use_skew(sk::SkewFit(F, w_atm, std::nullopt, {}, "too_few_smoothed_buckets"));
            } else if (pm == "flatnone") {
                pr.emplace(board, strikes, F);
                pr->use_skew(sk::SkewFit(F, std::nullopt, std::nullopt, {}, "no_atm_fit"));
            }
            rt::Positions pos;
            const std::size_t np = std::stoul(w[14]);
            for (std::size_t i = 0; i < np; ++i) {
                const std::string& t = w[15 + i];
                const auto eq = t.rfind('=');
                pos.set(t.substr(0, eq), ohx(t.substr(eq + 1)));
            }
            const auto before = std::filesystem::exists(j->path()) ? std::filesystem::file_size(j->path()) : 0;
            const auto row = tape->snapshot(
                "SPX", now, pos, &sets.at(std::stoi(w[12])), markets, pr ? &*pr : nullptr,
                w[7] == "1" ? &cal : nullptr, ohx(w[8]), ohx(w[9]), ohx(w[10]), ohx(w[11]),
                w[13] == "1" ? hourly : std::vector<th::Market>{}, mono, ts, write);
            j->flush();
            std::string got = slurp(j->path()).substr(before);
            if (!got.empty() && got.back() == '\n') got.pop_back();
            T.hit(fn, J(row) == parts[1] && (got.empty() ? "-" : got) == parts[2], line);
        } else if (fn == "det") {
            T.hit(fn, J(tape->detail().to_json()) == parts[1], line);
        } else if (fn == "vcd") {
            const std::string& mode = w[3];
            std::optional<sk::SkewFit> sf;
            if (mode == "fit") sf = *fitted;
            else if (mode == "flat") sf = sk::SkewFit(F, w_atm, std::nullopt, {}, "x");
            else if (mode == "flatnone") sf = sk::SkewFit(F, std::nullopt, std::nullopt, {}, "x");
            const auto d = rt::vanilla_call_delta(ohx(w[1]), ohx(w[2]), sf ? &*sf : nullptr, ohx(w[4]));
            T.hit(fn, same(d, w[5]), line);
        } else if (fn == "bkt") {
            const auto b = rt::bucket_of(ohx(w[1]));
            T.hit(fn, (b ? std::to_string(*b) : std::string("None")) == w[2], line);
        } else if (fn == "rs") {
            const th::Market* m = nullptr;
            for (const auto& x : markets) if (x.ticker == w[1]) { m = &x; break; }
            const th::Market blank{};
            T.hit(fn, same(rt::reference_strike(m ? *m : blank), w[2]), line);
        } else {
            T.hit("unknown_tag:" + fn, false, line);
        }
    }
    j->close();
    std::error_code ec;
    std::filesystem::remove_all(td, ec);
    return T.report();
}
