// Small parsing helpers shared by the basis/skewstate/boards/deltaadj/risktape/journal suites.
#pragma once
#include <string>
#include <utility>
#include <vector>

#include "kp/json/value.hpp"
#include "kp/massive/snapshot.hpp"
#include "kp/pricing/skew.hpp"
#include "vecio.hpp"

namespace kp::test::tapes {

// Split on TAB: the numeric head, then any JSON / free-text fields.
inline std::vector<std::string> tabs(const std::string& line) {
    std::vector<std::string> out;
    std::size_t p0 = 0;
    for (std::size_t p = 0; p <= line.size(); ++p)
        if (p == line.size() || line[p] == '\t') { out.push_back(line.substr(p0, p - p0)); p0 = p + 1; }
    return out;
}

inline std::string J(const kp::json::Value& v) { return kp::json::dumps(v, kp::json::COMPACT); }

// "R:K:bp:ap:bs:as" -> one board entry (None = absent field).
inline void add_leg(kp::pricing::skew::Board& b, const std::string& tok) {
    std::vector<std::string> f;
    std::size_t p0 = 0;
    for (std::size_t p = 0; p <= tok.size(); ++p)
        if (p == tok.size() || tok[p] == ':') { f.push_back(tok.substr(p0, p - p0)); p0 = p + 1; }
    kp::pricing::theo::Quote q;
    q.present = true;
    q.bp = vec::ohx(f[2]);
    q.ap = vec::ohx(f[3]);
    q.bs = vec::ohx(f[4]);
    q.as = vec::ohx(f[5]);
    b[{f[0][0], vec::hx(f[1])}] = q;
}

// A raw Massive snapshot {sym: Q frame} as JSON -> OptionSnapshot. The pricing path reads only
// bp/ap/bs/as (and `t` rides along).
inline kp::massive::OptionSnapshot snapshot_from_json(const kp::json::Value& v) {
    kp::massive::OptionSnapshot out;
    for (const auto& [sym, fr] : v.obj()) {
        kp::massive::OptionQuote oq;
        oq.q.present = true;
        oq.q.bp = fr.opt_num("bp");
        oq.q.ap = fr.opt_num("ap");
        oq.q.bs = fr.opt_num("bs");
        oq.q.as = fr.opt_num("as");
        if (const auto* t = fr.find("t"); t && t->is_number()) oq.t_ms = t->integer();
        out[sym] = oq;
    }
    return out;
}

}  // namespace kp::test::tapes
