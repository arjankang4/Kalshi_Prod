// The floating-point environment is what every golden vector assumes.
//
// This runs FIRST in the test suite and gates everything else. If the box's libm changed or a
// build flag drifted, every downstream parity failure would be noise -- fail here in
// milliseconds with a clear message instead.
#include "../harness.hpp"
#include "kp/pycompat/round.hpp"

#include <cfenv>
#include <cfloat>
#include <cmath>
#include <limits>

using namespace kp::test;
namespace pc = kp::pycompat;

int main() {
    pc::assert_c_numeric_locale();

    section("IEEE-754 environment");
    check("double is IEC559", std::numeric_limits<double>::is_iec559);
    eq<int>("sizeof(double)", (int)sizeof(double), 8);
    // No x87 80-bit intermediates: -fexcess-precision=standard plus SSE2 on x86-64.
    eq<int>("FLT_EVAL_METHOD", (int)FLT_EVAL_METHOD, 0);
    // pycompat::py_round is nearbyint, which is round-half-to-even ONLY under FE_TONEAREST.
    eq<int>("rounding mode is FE_TONEAREST", std::fegetround(), FE_TONEAREST);

    section("the optimiser is not rewriting arithmetic");
    bits("0.1 + 0.2", 0.1 + 0.2, 0x1.3333333333334p-2);
    {
        // With GCC's default -ffp-contract=fast AND an FMA-capable -march, this product-sum
        // would fuse and round once instead of twice. cmake/FloatingPoint.cmake forbids it.
        volatile double a = 0.1, b = 0.2, c = 0.3;
        bits("a*b+c is not contracted into an FMA", a * b + c, 0x1.47ae147ae147bp-2);
    }

    section("libm is the same libm CPython calls");
    // /lib64/libm.so.6 is shared by both: pyconfig.h:318 sets HAVE_ERF=1, and ldd on
    // math.cpython-312-x86_64-linux-gnu.so resolves libm to the system one. A 360,019-case
    // sweep of erf/exp/log/sqrt/pow found zero mismatches; these pin a slice of it.
    bits("erf(1.0)",     std::erf(1.0),      0x1.af767a741088bp-1);
    bits("erf(0.5)",     std::erf(0.5),      0x1.0a7ef5c18edd2p-1);
    bits("erf(-0.7)",    std::erf(-0.7),    -0x1.5b08c21171646p-1);
    bits("exp(-0.5)",    std::exp(-0.5),     0x1.368b2fc6f960ap-1);
    bits("exp(-12.25)",  std::exp(-12.25),   0x1.411fb0da07713p-18);
    bits("log(1.5)",     std::log(1.5),      0x1.9f323ecbf984cp-2);
    bits("log(0.75)",    std::log(0.75),    -0x1.269621134db92p-2);

    section("pow is not sqrt and is not cbrt");
    // pricing/theo.py:175 defines cbrt(x) as max(x, 0.0) ** (1.0/3.0). That is pow, and
    // std::cbrt disagrees -- it feeds micro(), hence every option microprice, hence every
    // theo. If someone "fixes" this to std::cbrt, these four go red.
    bits("pow(27,1/3)",  std::pow(27.0,  1.0/3.0), 0x1.8000000000000p+1);
    bits("pow(64,1/3)",  std::pow(64.0,  1.0/3.0), 0x1.fffffffffffffp+1);
    bits("pow(125,1/3)", std::pow(125.0, 1.0/3.0), 0x1.3ffffffffffffp+2);
    bits("pow(119,1/3)", std::pow(119.0, 1.0/3.0), 0x1.3acbbb0e756b6p+2);
    {
        // NOTE the volatile. Without it GCC constant-folds the libm call with MPFR, which is
        // CORRECTLY ROUNDED and therefore need not agree with glibc at runtime -- measured on
        // this box, folded cbrt(27) is 0x1.8p+1 while glibc's runtime cbrt(27) is
        // 0x1.8000000000001p+1. Python of course gets the glibc answer.
        //
        // That is a live hazard for the port, not a test artifact: any transcendental applied
        // to compile-time constants may be folded to a value the reference never produces. The
        // rule is NO constexpr/literal-argument transcendentals except sqrt, which is
        // IEEE-mandated correctly rounded so MPFR and glibc necessarily agree (see SQRT2 above).
        volatile double k27 = 27.0;
        check("std::cbrt(27) differs from pow(27,1/3) at runtime",
              !same_bits(std::cbrt(k27), std::pow(k27, 1.0/3.0)));
        bits("runtime cbrt(27) is glibc's, not MPFR's", std::cbrt(k27), 0x1.8000000000001p+1);
    }

    // pricing/bs.py:31-32. sqrt is IEEE-mandated correctly rounded, so these are safe as
    // compile-time constants -- but pin them, because no OTHER transcendental may be constexpr.
    bits("SQRT2",    std::sqrt(2.0),                       0x1.6a09e667f3bcdp+0);
    bits("SQRT_2PI", std::sqrt(2.0 * 3.141592653589793),   0x1.40d931ff62705p+1);

    section("the shims are actually installed");
    bits("py_round(2.5) is 2, not 3", pc::py_round(2.5), 2.0);
    bits("py_round(1.5) is 2",        pc::py_round(1.5), 2.0);
    bits("py_round(0.5) is 0",        pc::py_round(0.5), 0.0);
    bits("py_round(-2.5) is -2",      pc::py_round(-2.5), -2.0);
    check("std::round(2.5) is 3, i.e. would have been wrong", std::round(2.5) == 3.0);

    return report("test_fpenv");
}
