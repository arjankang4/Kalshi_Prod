# The floating-point contract for the whole project.
#
# This port's bar is BIT-IDENTICAL output against the Python reference (the Kalshi
# repo). That is only achievable because CPython's math module and a g++
# binary on this box both call the same /lib64/libm.so.6 -- verified over 360,019 cases
# (erf/exp/log/sqrt/pow) with zero mismatches. See tests/unit/test_fpenv.cpp, which re-runs
# a slice of that gate so a toolchain bump cannot silently break parity.
#
# What libm gives us for free, the optimiser can take away. Every flag below exists to stop
# GCC rewriting arithmetic that the Python performs one operation at a time.
add_library(kp_fp INTERFACE)
add_library(kp::fp ALIAS kp_fp)

target_compile_options(kp_fp INTERFACE
    # THE critical one. GCC defaults to -ffp-contract=fast for C++, which fuses a*b+c into a
    # single FMA with ONE rounding instead of two. On the default x86-64 baseline there is no
    # FMA instruction so it is latent -- but the moment anyone adds -march=native on this
    # Haswell+ box, pricing/skew.py's spline evaluation
    #     y[lo] + b*dx + c[lo]*dx*dx + dd*dx*dx*dx
    # (four multiply-adds in a row) changes by an ulp, and that ulp flows through SkewFit.w_of
    # into every wing theo, then through quoting._edge_micros' integer threshold into a
    # different order. Turning contraction off makes -march=native safe rather than forbidden.
    -ffp-contract=off

    # Explicit, and also neutralises an -Ofast inherited from a parent scope.
    -fno-fast-math
    -fno-associative-math          # forbids reassociating the summation folds in
                                   # theo.bracket_fair / greeks.bracket_greek / atmvol
    -fno-reciprocal-math
    -fno-unsafe-math-optimizations # stops pow(x,0.5) -> sqrt(x) and pow(x,1/3) -> cbrt(x).
                                   # pricing/theo.py:175 defines cbrt as pow(x, 1.0/3.0) and
                                   # the two differ: 27 -> 3.0 vs 3.0000000000000004.
    -fno-finite-math-only
    # NOTE: -fexcess-precision=standard is NOT set. GCC 11 rejects it for C++
    # ("sorry, unimplemented"); it arrived for C++ in GCC 12. It is not needed here: x86-64
    # uses SSE2 rather than x87, so there are no 80-bit intermediates and FLT_EVAL_METHOD is
    # already 0. tests/unit/test_fpenv.cpp asserts that rather than trusting it, so if this
    # ever builds on a target where it is false, the suite fails loudly.
    -frounding-math                # no constant folding under an assumed rounding mode;
                                   # pycompat::py_round depends on FE_TONEAREST being live
)

# Deliberately NOT set anywhere: -ffast-math, -Ofast, -march=*, -mfma, -mavx2, -fno-math-errno.
# tests/unit/test_fpenv.cpp asserts the resulting environment at runtime.
