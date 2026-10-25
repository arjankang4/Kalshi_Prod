// Shared loader for the captured real boards, plus the small parsing helpers every vector
// replay needs. skew_boards.vec carries each board once; the suites reference them by index.
#pragma once
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "kp/pricing/skew.hpp"
#include "kp/pricing/theo_core.hpp"
#include "harness.hpp"

namespace kp::test {

inline bool is_none(const std::string& s) { return s == "None"; }
inline double hx(const std::string& s) { return std::strtod(s.c_str(), nullptr); }
inline std::optional<double> opt(const std::string& s) {
    return is_none(s) ? std::nullopt : std::optional<double>(hx(s));
}
inline bool same_opt(std::optional<double> g, const std::string& w) {
    if (is_none(w) || !g) return is_none(w) == !g.has_value();
    return same_bits(*g, hx(w));
}
inline std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::size_t p0 = 0;
    for (std::size_t p = 0; p <= s.size(); ++p)
        if (p == s.size() || s[p] == sep) { out.push_back(s.substr(p0, p - p0)); p0 = p + 1; }
    return out;
}
inline std::vector<double> take(std::istringstream& ss, std::size_t n) {
    std::vector<double> v; v.reserve(n);
    for (std::size_t i = 0; i < n; ++i) { std::string t; ss >> t; v.push_back(hx(t)); }
    return v;
}
inline const kp::pricing::theo::Quote& empty_quote() {
    static const kp::pricing::theo::Quote Q{};
    return Q;
}

struct RealBoard {
    kp::pricing::skew::Board board;
    std::vector<double> strikes;
};

inline std::vector<RealBoard> load_boards(const std::string& path) {
    std::vector<RealBoard> out;
    std::ifstream in(path);
    if (!in) return out;
    std::string header, line;
    std::getline(in, header);
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::istringstream ss(line);
        std::string tag; std::size_t bi, ns;
        ss >> tag >> bi >> ns;
        RealBoard rb;
        rb.strikes = take(ss, ns);
        std::string bar; ss >> bar;                    // "||"
        std::size_t nl; ss >> nl;
        for (std::size_t i = 0; i < nl; ++i) {
            std::string tok; ss >> tok;
            const auto f = split(tok, ':');            // right:strike:bp:ap:bs:as
            if (f.size() != 6) continue;
            kp::pricing::theo::Quote q;
            q.present = true;
            if (!is_none(f[2])) q.bp = hx(f[2]);
            if (!is_none(f[3])) q.ap = hx(f[3]);
            if (!is_none(f[4])) q.bs = hx(f[4]);
            if (!is_none(f[5])) q.as = hx(f[5]);
            rb.board[{f[0][0], hx(f[1])}] = q;
        }
        if (out.size() <= bi) out.resize(bi + 1);
        out[bi] = std::move(rb);
    }
    return out;
}

}  // namespace kp::test
