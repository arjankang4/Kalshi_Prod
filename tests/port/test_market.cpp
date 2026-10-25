// theo::parse_iso_ns against edgemgr.settlement_of (datetime.fromisoformat after Z -> +00:00).
#include <string>
#include <vector>

#include "kp/pricing/theo_core.hpp"
#include "vecio.hpp"

using namespace kp::test::vec;

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "tests/data/golden";
    Tally T("market");
    T.expect({"iso"});
    for (const auto& line : read(dir, "market")) {
        const auto a = line.find('\t'), b = line.rfind('\t');
        const std::string s = line.substr(a + 1, b - a - 1), want = line.substr(b + 1);
        const auto got = kp::pricing::theo::parse_iso_ns(s);
        T.hit("iso", want == "None" ? !got : (got && *got == std::stoll(want)), line);
    }
    return T.report();
}
