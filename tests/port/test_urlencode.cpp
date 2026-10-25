// net::quote_plus / net::urlencode against urllib.parse, byte for byte (see gen_urlencode.py).
#include "kp/net/http.hpp"
#include "vecio.hpp"

using namespace kp::test::vec;

namespace {
std::string unhex(const std::string& h) {
    if (h == "-") return "";
    std::string out;
    for (std::size_t i = 0; i + 1 < h.size(); i += 2) out.push_back((char)std::stoi(h.substr(i, 2), nullptr, 16));
    return out;
}
std::string want(const std::string& w) { return w == "<empty>" ? std::string() : w; }
}  // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "tests/data/golden";
    Tally T("urlencode");
    T.expect({"qp", "ue"});
    for (const auto& line : read(dir, "urlencode")) {
        const auto w = words(line);
        if (w[0] == "qp") {
            T.hit("qp", kp::net::quote_plus(unhex(w[1])) == want(w[2]), line);
        } else if (w[0] == "ue") {
            const int n = std::stoi(w[1]);
            std::vector<std::pair<std::string, std::string>> p;
            for (int i = 0; i < n; ++i) p.emplace_back(unhex(w[2 + 2 * (std::size_t)i]), unhex(w[3 + 2 * (std::size_t)i]));
            T.hit("ue", kp::net::urlencode(p) == want(w[2 + 2 * (std::size_t)n]), line);
        } else {
            T.hit("unknown_tag:" + w[0], false, line);
        }
    }
    return T.report();
}
