// kp::json::dumps against CPython json.dumps in the default, compact and indent styles, over
// random nested documents with control characters, non-BMP code points, int64-edge ints and
// floats on both sides of repr's exponent switch. Also pins int-vs-float survival through parse.
#include <string>
#include <vector>

#include "kp/json/value.hpp"
#include "vecio.hpp"

using namespace kp::test::vec;

namespace {
std::vector<std::string> tabs(const std::string& s) {
    std::vector<std::string> out;
    std::size_t p0 = 0;
    for (std::size_t p = 0; p <= s.size(); ++p)
        if (p == s.size() || s[p] == '\t') { out.push_back(s.substr(p0, p - p0)); p0 = p + 1; }
    return out;
}
// The generator folds the real newlines of an indented dump to \x01: ensure_ascii output never
// carries a raw control byte, so the marker cannot collide with content.
std::string unescape_nl(std::string s) {
    for (auto& c : s) if (c == '\x01') c = '\n';
    return s;
}
}  // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "tests/data/golden";
    Tally T("jsondumps");
    T.expect({"dumps:default", "dumps:compact", "dumps:indent1", "dumps:indent2"});
    for (const auto& line : read(dir, "jsondumps")) {
        const auto f = tabs(line);
        if (f.size() != 4) { T.hit("malformed", false, line); continue; }
        kp::json::DumpStyle st;
        if (f[1] == "compact") st = kp::json::COMPACT;
        else if (f[1] == "indent1") st.indent = 1;
        else if (f[1] == "indent2") st.indent = 2;
        const std::string want = f[1].rfind("indent", 0) == 0 ? unescape_nl(f[3]) : f[3];
        std::string got;
        try { got = kp::json::dumps(kp::json::parse(f[2]), st); } catch (...) { got = "<throw>"; }
        T.hit("dumps:" + f[1], got == want, line + "\n      got " + got);
    }
    // ints survive parse exactly, including past 2^53
    const auto v = kp::json::parse("{\"a\":1787241600123456789,\"b\":1.0,\"c\":0}");
    kp::test::check("int64 exact", v.find("a")->is_int() && v.find("a")->integer() == 1787241600123456789LL);
    kp::test::check("float stays float", !v.find("b")->is_int());
    return T.report();
}
