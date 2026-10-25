// rvol/live against the reference: the backfill through an injected fetch (success, closes_ms
// failing, second_bars failing after closes_ms succeeded, a late start), then a 5-second poll of
// real index levels through IndexBuffer::append -- repeats, regressions, missing quotes, the 3h
// trim -- with LiveRealizedVol::features read on a stepped monotonic clock that hits and misses
// the LEAN_RECOMPUTE_S cache on its exact boundary.
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "kp/rvol/live.hpp"
#include "vecio.hpp"

using namespace kp::test::vec;
namespace LV = kp::rvol::live;
using kp::time::Instant;

namespace {
struct Scenario {
    std::vector<std::pair<std::int64_t, double>> closes;
    std::vector<LV::Bar> bars;
    std::string mode;
    std::shared_ptr<LV::IndexBuffer> buf;
    std::unique_ptr<LV::LiveRealizedVol> lrv;
};

std::string tokens(const std::optional<kp::rvol::model::Features>& f) {
    if (!f) return "None";
    std::string s;
    for (const auto& [k, v] : *f) {
        if (!s.empty()) s += ' ';
        char buf[64];
        if (v) std::snprintf(buf, sizeof buf, "%a", *v);
        s += k + "=" + (v ? std::string(buf) : std::string("None"));
    }
    return s;
}

bool same_tokens(const std::optional<kp::rvol::model::Features>& f,
                 const std::vector<std::string>& w, std::size_t from) {
    if (!f) return w.size() == from + 1 && w[from] == "None";
    if (w.size() - from != f->size()) return false;
    std::size_t i = from;
    for (const auto& [k, v] : *f) {
        const auto eq = w[i].find('=');
        if (w[i].substr(0, eq) != k || !same(v, w[i].substr(eq + 1))) return false;
        ++i;
    }
    return true;
}
}  // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "tests/data/golden";
    const std::string root = argc > 2 ? argv[2] : "";
    kp::time::init();
    Tally T("rvol_live");
    T.expect({"closes", "bars", "backfill", "append", "feat"});

    std::map<std::string, Scenario> sc;
    std::map<std::string, std::shared_ptr<const kp::pricing::varcal::VarianceCalendar>> cals;
    std::vector<std::pair<std::int64_t, double>>* fill_closes = nullptr;
    std::vector<LV::Bar>* fill_bars = nullptr;

    for (const auto& line : read(dir, "rvol_live")) {
        const auto w = words(line);
        const std::string& fn = w[0];
        if (fn == "pt") {
            fill_closes->emplace_back(std::stoll(w[1]), hx(w[2]));
        } else if (fn == "bar") {
            LV::Bar b;
            b.t = std::stoll(w[1]);
            if (w[2] != "missing") b.c = ohx(w[2]);
            fill_bars->push_back(b);
        } else if (fn == "closes") {
            fill_closes = &sc[w[1]].closes;
            T.hit(fn, true, line);
        } else if (fn == "bars") {
            fill_bars = &sc[w[1]].bars;
            T.hit(fn, true, line);
        } else if (fn == "backfill") {
            Scenario& s = sc.at(w[1]);
            const std::string u = w[1].substr(0, w[1].find('.'));
            s.mode = w[3];
            if (!cals.count(u))
                cals[u] = std::make_shared<const kp::pricing::varcal::VarianceCalendar>(
                    kp::pricing::varcal::VarianceCalendar::load(u, "default", root));
            s.buf = std::make_shared<LV::IndexBuffer>(u);
            s.lrv = std::make_unique<LV::LiveRealizedVol>(u, cals[u], s.buf, root);
            LV::Fetch fetch;
            fetch.closes_ms = [&s, u](const std::string& und, const std::string& day) {
                if (und != u || day != "2026-08-21") throw std::logic_error("unexpected closes_ms args");
                if (s.mode == "closes") throw std::runtime_error("Fail: closes_ms down");
                return s.closes;
            };
            fetch.second_bars = [&s, u](const std::string& tk, const std::string& a,
                                        const std::string& b) {
                if (tk != LV::source_of(u).proxy || a != "2026-08-21" || b != a)
                    throw std::logic_error("unexpected second_bars args");
                if (s.mode == "bars") throw std::runtime_error("Fail: second_bars down");
                return s.bars;
            };
            const bool ok = s.buf->backfill(Instant{std::stoll(w[2])}, fetch);
            const auto arr = s.buf->arrays();
            const std::string np = s.buf->proxy_t() ? std::to_string(s.buf->proxy_t()->size()) : "None";
            T.hit(fn, (ok ? "1" : "0") == w[4] && std::to_string(arr.first.size()) == w[5]
                          && same(s.buf->span_s(), w[6]) && np == w[7], line);
        } else if (fn == "append") {
            Scenario& s = sc.at(w[1]);
            const std::string& kind = w[2];
            bool ret;
            if (kind == "none") {
                ret = s.buf->append(nullptr);
            } else {
                kp::massive::index::IndexQuote q;
                q.underlying = w[1].substr(0, w[1].find('.'));
                if (w[3] != "None") q.ts_ns = std::stoll(w[3]);
                q.value = hx(w[4]);
                ret = s.buf->append(&q);
            }
            const auto arr = s.buf->arrays();
            const std::string first = arr.first.empty() ? "-" : std::to_string(arr.first.front());
            T.hit(fn, (ret ? "1" : "0") == w[5] && std::to_string(arr.first.size()) == w[6]
                          && same(s.buf->span_s(), w[7]) && first == w[8], line);
        } else if (fn == "feat") {
            Scenario& s = sc.at(w[1]);
            const auto f = s.lrv->features(Instant{std::stoll(w[2])}, hx(w[3]), w[4] == "1");
            const bool ok = same_tokens(f, w, 5);
            T.hit(fn, ok, ok ? line : line + "\n    got: " + tokens(f));
        } else {
            T.hit("unknown:" + fn, false, line);
        }
    }
    return T.report();
}
