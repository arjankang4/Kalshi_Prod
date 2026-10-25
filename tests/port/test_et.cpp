// kp::time::from_et against zoneinfo fold=0 across DST transitions and 20k random readings.
#include <string>
#include "kp/time/et.hpp"
#include "vecio.hpp"
using namespace kp::test::vec;
int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "tests/data/golden";
    kp::time::init();
    Tally T("et");
    T.expect({"from_et"});
    for (const auto& line : read(dir, "et")) {
        const auto w = words(line);
        const auto t = kp::time::from_et(std::stoi(w[1]), std::stoi(w[2]), std::stoi(w[3]), std::stoi(w[4]),
                                         std::stoi(w[5]), std::stoi(w[6]));
        T.hit(w[0], t.ns == std::stoll(w[7]) * 1000000000LL, line + "  got " + std::to_string(t.ns / 1000000000LL));
    }
    return T.report();
}
