// pricing/lean backoff_cents / backoff_micros against the reference, both legs and every knee.
#include <string>

#include "kp/pricing/lean.hpp"
#include "vecio.hpp"

using namespace kp::test::vec;
namespace ln = kp::pricing::lean;

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "tests/data/golden";
    Tally T("lean_backoff");
    T.expect({"bo"});
    for (const auto& line : read(dir, "lean_backoff")) {
        const auto w = words(line);
        if (w[0] != "bo") { T.hit("unknown_tag:" + w[0], false, line); continue; }
        const auto a = ohx(w[1]);
        const auto st = ohx(w[2]), md = ohx(w[3]), hf = ohx(w[4]), en = ohx(w[5]);
        T.hit("bo", same(ln::backoff_cents(a, st, md, hf, en), w[6])
                        && ln::backoff_micros(a, st, md, hf, en) == std::stol(w[7]), line);
    }
    // The defaults' three knees: BACKOFF_START_CENTS now, BACKOFF_MID_CENTS at BACKOFF_HALF_S,
    // 0c at BACKOFF_END_S (1.5c, 0.75c at 4s, 0c at 8s).
    kp::test::check("1.5c at the fill", ln::backoff_micros(0.0) == 15000);
    kp::test::check("0.75c at 4s", ln::backoff_micros(4.0) == 7500);
    kp::test::check("0c at 8s", ln::backoff_micros(8.0) == 0);
    kp::test::check("clean side pays nothing", ln::backoff_micros(std::nullopt) == 0);
    return T.report();
}
