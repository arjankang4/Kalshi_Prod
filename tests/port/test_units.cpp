// storage/units against the reference: value, None, or the exception TYPE and MESSAGE.
#include <cmath>

#include "kp/storage/units.hpp"
#include "vecio.hpp"

using namespace kp::test::vec;
namespace U = kp::storage::units;
using kp::json::Value;

namespace {
Value input(const std::string& t) {
    if (t == "NaN") return Value::make_number(std::nan(""));
    if (t == "Infinity") return Value::make_number(HUGE_VAL);
    if (t == "-Infinity") return Value::make_number(-HUGE_VAL);
    return kp::json::parse(t);
}
std::string unesc(std::string s) {
    for (const auto& [from, to] : {std::pair<std::string, std::string>{"\\x20", " "}, {"\\x5c", "\\"}}) {
        std::size_t p = 0;
        while ((p = s.find(from, p)) != std::string::npos) { s.replace(p, from.size(), to); p += to.size(); }
    }
    return s;
}
}  // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "tests/data/golden";
    Tally T("units");
    T.expect({"micros", "cc"});
    for (const auto& line : read(dir, "units")) {
        const auto w = words(line);
        const Value v = input(w[1]);
        std::string got;
        try {
            const auto r = w[0] == "micros" ? U::to_micros(v) : U::to_cc(v);
            got = r ? std::to_string(*r) : "None";
        } catch (const kp::storage::PyError& e) {
            got = "!" + e.type + "!" + e.what();
        }
        T.hit(w[0], got == unesc(w[2]), line + "   GOT " + got);
    }
    return T.report();
}
