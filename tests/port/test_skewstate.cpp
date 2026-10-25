// strategy/skewstate against the reference: observe() scripts with the monotonic clock driven by
// the generator; after every call the served curve (p_above incl. the monotone projection, ratio)
// bit-exact on and off the grid, and detail() as json.dumps text.
#include <map>
#include <sstream>
#include <string>

#include "kp/strategy/skewstate.hpp"
#include "tapes_io.hpp"
#include "vecio.hpp"

using namespace kp::test::vec;
using namespace kp::test::tapes;
namespace ss = kp::strategy::skewstate;
namespace sk = kp::pricing::skew;

namespace {

struct B { sk::Board q; };

bool curve_ok(const sk::SkewFit& c, const std::vector<double>& ks, const std::string& want) {
    const auto w = words(want);
    if (w.size() != 4 + 2 * ks.size()) return false;
    if ((c.flat() ? "flat" : "fit") != w[0]) return false;
    if ((c.ok() ? "1" : "0") != w[1]) return false;
    if ((c.reason().empty() ? "-" : c.reason()) != w[2]) return false;
    if (std::to_string(c.n_buckets()) != w[3]) return false;
    for (std::size_t i = 0; i < ks.size(); ++i) {
        if (!same(c.p_above(ks[i]), w[4 + 2 * i])) return false;
        if (!same(c.ratio(ks[i]), w[5 + 2 * i])) return false;
    }
    return true;
}

std::vector<double> take(const std::vector<std::string>& w, std::size_t& i) {
    const std::size_t n = std::stoul(w[i++]);
    std::vector<double> v;
    for (std::size_t k = 0; k < n; ++k) v.push_back(hx(w[i++]));
    return v;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "tests/data/golden";
    Tally T("skewstate");
    T.expect({"board", "new", "obs", "bare"});
    std::map<int, sk::Board> boards;
    std::map<int, ss::SkewState> st;

    for (const auto& line : read(dir, "skewstate")) {
        const auto parts = tabs(line);
        const auto w = words(parts[0]);
        const std::string& fn = w[0];
        const int id = std::stoi(w[1]);
        if (fn == "board") {
            sk::Board b;
            const std::size_t n = std::stoul(w[2]);
            for (std::size_t i = 0; i < n; ++i) add_leg(b, w[3 + i]);
            boards[id] = std::move(b);
            T.hit(fn, true, line);
        } else if (fn == "new") {
            st.erase(id);
            st.emplace(id, ss::SkewState(w[2], ohx(w[3]), ohx(w[4]), ohx(w[5])));
            T.hit(fn, true, line);
        } else if (fn == "obs") {
            auto& s = st.at(id);
            const auto& b = boards.at(std::stoi(w[2]));
            std::size_t i = 7;
            const auto grid = take(w, i);
            const auto ks = take(w, i);
            const auto& c = s.observe(b, grid, ohx(w[3]), ohx(w[4]), hx(w[5]), w[6] == "1");
            T.hit(fn, curve_ok(c, ks, parts[1]) && J(s.detail().to_json()) == parts[2], line);
        } else if (fn == "bare") {
            auto& s = st.at(id);
            std::size_t i = 5;
            const auto ks = take(w, i);
            const auto c = s.rebuild(hx(w[2]), hx(w[3]), hx(w[4]));
            T.hit(fn, curve_ok(c, ks, parts[1]), line);
        } else {
            T.hit("unknown_tag:" + fn, false, line);
        }
    }
    return T.report();
}
