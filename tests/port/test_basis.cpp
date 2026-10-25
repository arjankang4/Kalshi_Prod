// pricing/basis against the reference: operation scripts replayed on the C++ BasisTracker, every
// float bit-exact and every detail dict compared as json.dumps text.
#include <map>
#include <string>

#include "kp/json/value.hpp"
#include "kp/pricing/basis.hpp"
#include "vecio.hpp"

using namespace kp::test::vec;
namespace bm = kp::pricing::basis;
namespace ix = kp::massive::index;

namespace {

std::pair<std::string, std::string> split_tab(const std::string& line) {
    const auto p = line.find('\t');
    if (p == std::string::npos) return {line, ""};
    return {line.substr(0, p), line.substr(p + 1)};
}

std::string J(const kp::json::Value& v) { return kp::json::dumps(v, kp::json::COMPACT); }

}  // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "tests/data/golden";
    Tally T("basis");
    T.expect({"new", "obs", "spot", "det", "basis"});
    std::map<int, bm::BasisTracker> trk;

    for (const auto& line : read(dir, "basis")) {
        const auto [head, js] = split_tab(line);
        const auto w = words(head);
        const std::string& fn = w[0];
        const int id = std::stoi(w[1]);
        if (fn == "new") {
            trk.erase(id);
            trk.emplace(id, bm::BasisTracker(w[2], ohx(w[3]), ohx(w[4]), std::stoi(w[5])));
            T.hit(fn, true, line);
            continue;
        }
        auto it = trk.find(id);
        if (it == trk.end()) { T.hit(fn, false, line); continue; }
        auto& t = it->second;
        if (fn == "obs") {
            std::optional<ix::IndexQuote> q;
            if (w[3] != "-") {
                ix::IndexQuote iq;
                iq.underlying = t.underlying();
                iq.value = ohx(w[3]);
                if (!none(w[4])) iq.ts_ns = std::stoll(w[4]);
                q = iq;
            }
            const bool folded = t.observe(ohx(w[2]), q, hx(w[5]));
            T.hit(fn, (folded ? "1" : "0") == w[6], line);
        } else if (fn == "spot") {
            const auto r = t.spot(ohx(w[2]));
            T.hit(fn, same(r.spot, w[3]) && J(r.detail.to_json()) == js, line);
        } else if (fn == "det") {
            T.hit(fn, J(t.detail().to_json()) == js, line);
        } else if (fn == "basis") {
            T.hit(fn, same(t.basis(), w[2]) && (t.ready() ? "1" : "0") == w[3], line);
        } else {
            T.hit("unknown_tag:" + fn, false, line);
        }
    }
    return T.report();
}
