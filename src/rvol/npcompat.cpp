#include "kp/rvol/npcompat.hpp"

#include <immintrin.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>

// EVIDENCE, measured against the reference interpreter environment (numpy 2.5.2,
// scipy-openblas 0.3.34 DYNAMIC_ARCH, on an AVX-512 Ice Lake class host):
//
//   np.log     `numpy.lib.introspect.opt_func_info` reports log/dd current = X86_V4. Reading the
//              ufunc's loop pointer out of the PyUFuncObject lands on DOUBLE_log_X86_V4, whose
//              contiguous path calls __svml_log8_ha (tail lanes padded with 1.0) and whose
//              overlapping-buffer path calls glibc log. DOUBLE_log_X86_V3 is a plain glibc loop.
//              np.log vs math.log: 83 / 1e6 mismatches on [5000, 8000], 63 on [18000, 30000],
//              118 on [400, 800] -- all exactly 1 ulp. The transcription below reproduces
//              np.log on every input tested (see tests/port/test_rvol_features.cpp, `nplog`).
//   np.sum     20,000 random arrays, n in [0, 1200): `0.0 + pairwise(a)` matches 20,000/20,000;
//              `a[0] + pairwise(a[1:])` mismatches 10,861; `-0.0 + pairwise(a)` mismatches the
//              91 empty/negative-zero cases.
//   np.mean    == float(np.sum(a)) / n on 40,000 arrays and lists.
//   np.dot     n = 7 (the model's design row): FMA-contracted left fold from 0.0 matches; the
//              uncontracted fold does not (see the `npdot` tag of the same suite).

namespace kp::rvol::np {

namespace {

// __svml_dlog_ha_data_internal_avx512, read out of numpy's _multiarray_umath .rodata at
// 0x882940. Two 16-entry tables indexed by the top four mantissa bits of the rounded reciprocal,
// then the broadcast constants.
constexpr std::uint64_t LOG_HI[16] = {
    0x0000000000000000ULL, 0xbfaf0a30c0120000ULL, 0xbfbe27076e2b0000ULL, 0xbfc5ff3070a78000ULL,
    0xbfcc8ff7c79a8000ULL, 0xbfd1675cababc000ULL, 0xbfd4618bc21c4000ULL, 0xbfd739d7f6bbc000ULL,
    0x3fd269621134c000ULL, 0x3fcf991c6cb38000ULL, 0x3fca93ed3c8b0000ULL, 0x3fc5bf406b540000ULL,
    0x3fc1178e82280000ULL, 0x3fb9335e5d590000ULL, 0x3fb08598b59e0000ULL, 0x3fa0415d89e80000ULL,
};
constexpr std::uint64_t LOG_LO[16] = {
    0x0000000000000000ULL, 0x3d53ab33d066d1d2ULL, 0x3d2a342c2af0003cULL, 0xbd43d3c873e20a07ULL,
    0xbd4a21ac25d81ef3ULL, 0x3d59f1fc63382a8fULL, 0xbd5ec27d0b7b37b3ULL, 0xbd50069ce24c53fbULL,
    0x3d5b92783beb7677ULL, 0x3d59bcbecca0cdf3ULL, 0xbd530e486a0ac42dULL, 0x3d5ed8fdc149767eULL,
    0xbd4b8421cc74be04ULL, 0x3d52622b8757a8fbULL, 0x3d4d034451fecdfbULL, 0xbd577771fd187145ULL,
};
constexpr std::uint64_t C_ONE   = 0x3ff0000000000000ULL;   // +0x100
constexpr std::uint64_t C_3Q    = 0x3fe8000000000000ULL;   // +0x140  0.75
constexpr std::uint64_t C_P180  = 0x3fbc81cd309d7c70ULL;   // +0x180  ~ 1/9
constexpr std::uint64_t C_P1C0  = 0xbfc007357e93af62ULL;   // +0x1c0  ~-1/8
constexpr std::uint64_t C_P200  = 0x3fc249229cee81efULL;   // +0x200  ~ 1/7
constexpr std::uint64_t C_P240  = 0xbfc55553fb28db06ULL;   // +0x240  ~-1/6
constexpr std::uint64_t C_P280  = 0x3fc9999999cc9f5cULL;   // +0x280  ~ 1/5
constexpr std::uint64_t C_P2C0  = 0xbfd00000000c05bdULL;   // +0x2c0  ~-1/4
constexpr std::uint64_t C_P300  = 0x3fd5555555555466ULL;   // +0x300  ~ 1/3
constexpr std::uint64_t C_P340  = 0xbfdfffffffffffc6ULL;   // +0x340  ~-1/2
constexpr std::uint64_t C_LN2HI = 0x3fe62e42fefa0000ULL;   // +0x380
constexpr std::uint64_t C_LN2LO = 0x3d7cf79abc9e0000ULL;   // +0x3c0

inline double d(std::uint64_t u) { double x; std::memcpy(&x, &u, 8); return x; }
inline std::uint64_t u(double x) { std::uint64_t v; std::memcpy(&v, &x, 8); return v; }

// vrcp14pd has no portable spelling: its result is a specific 14-bit approximation, and which
// side of a rounding boundary the NEXT instruction lands on depends on its exact bits.
__attribute__((target("avx512f"))) double rcp14(double m) {
    const __m128d v = _mm_set_sd(m);
    return _mm_cvtsd_f64(_mm_rcp14_sd(v, v));
}

}  // namespace

bool x86_v4() {
    static const bool v = [] {
        __builtin_cpu_init();
        return __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512cd")
            && __builtin_cpu_supports("avx512bw") && __builtin_cpu_supports("avx512dq")
            && __builtin_cpu_supports("avx512vl");
    }();
    return v;
}

// The main path of __svml_log8_ha, one lane, in instruction order. Every {rn-sae} FMA is one
// std::fma (a single rounding) and every add/sub/mul is one IEEE op -- -ffp-contract=off keeps
// the compiler from fusing the ones that are not fused in the original.
double svml_log(double x) {
    // vfpclasspd $0xdf: QNaN, +0, -0, +inf, -inf, negative finite, SNaN -> the rare path.
    // (Denormals are NOT in the mask; getmant/getexp normalise them on the main path.)
    if (!(x > 0.0) || std::isinf(x)) return std::log(x);

    int e2 = 0;
    const double fr = std::frexp(x, &e2);        // x = fr * 2^e2, fr in [0.5, 1)
    const double m = fr * 2.0;                    // vgetmantpd $0x8: mantissa in [1, 2)
    double e = static_cast<double>(e2 - 1);       // vgetexppd: floor(log2 x)

    // vrcp14pd then vrndscalepd $0x58: the reciprocal rounded to a multiple of 1/32, ties-even.
    const double r = std::nearbyint(rcp14(m) * 32.0) / 32.0;

    const double R = std::fma(r, m, -d(C_ONE));                      // vfmsub213pd
    const bool k1 = r < d(C_3Q);                                        // vcmplt_oqpd
    const double a7 = std::fma(d(C_P200), R, d(C_P240));
    double a1 = std::fma(d(C_P180), R, d(C_P1C0));
    const double R2 = R * R;
    double a9 = std::fma(d(C_P280), R, d(C_P2C0));
    const unsigned idx = static_cast<unsigned>(u(r) >> 48) & 15u;       // vpsrlq $0x30 -> vpermt2pd
    a1 = std::fma(R2, a1, a7);
    const double R4 = R2 * R2;
    const double a8 = std::fma(d(C_P300), R, d(C_P340));
    if (k1) e = e + d(C_ONE);                                           // vaddpd {k1}
    a9 = std::fma(R2, a9, a8);
    const double hi = std::fma(e, d(C_LN2HI), d(LOG_HI[idx]));
    a1 = std::fma(R4, a1, a9);
    const double s = hi + R;
    const double lo_r = R - (s - hi);
    a1 = std::fma(R2, a1, lo_r);
    const double lo = std::fma(d(C_LN2LO), e, d(LOG_LO[idx]));
    return s + (a1 + lo);
}

std::vector<double> log(const std::vector<double>& a) {
    std::vector<double> out(a.size());
    if (x86_v4()) {
        for (std::size_t i = 0; i < a.size(); ++i) out[i] = svml_log(a[i]);
    } else {
        for (std::size_t i = 0; i < a.size(); ++i) out[i] = std::log(a[i]);
    }
    return out;
}

// numpy/_core/src/umath/loops_utils.h.src, @TYPE@_pairwise_sum. Verified empirically (see top).
double pairwise_sum(const double* a, std::size_t n) {
    if (n < 8) {
        double res = -0.0;
        for (std::size_t i = 0; i < n; ++i) res += a[i];
        return res;
    }
    if (n <= 128) {
        double r[8];
        for (int j = 0; j < 8; ++j) r[j] = a[j];
        std::size_t i = 8;
        for (; i < n - (n % 8); i += 8)
            for (int j = 0; j < 8; ++j) r[j] += a[i + j];
        double res = ((r[0] + r[1]) + (r[2] + r[3])) + ((r[4] + r[5]) + (r[6] + r[7]));
        for (; i < n; ++i) res += a[i];
        return res;
    }
    std::size_t n2 = n / 2;
    n2 -= n2 % 8;
    return pairwise_sum(a, n2) + pairwise_sum(a + n2, n - n2);
}

double sum(const std::vector<double>& a) { return 0.0 + pairwise_sum(a.data(), a.size()); }

double mean(const std::vector<double>& a) {
    if (a.empty()) return std::nan("");
    return sum(a) / static_cast<double>(a.size());
}

// numpy DOUBLE_dot -> `double sum = 0.; sum += cblas_ddot(n, x, 1, y, 1)`. OpenBLAS's x86_64
// ddot runs its SIMD kernel on n & -16 and a C tail loop `dot += y[i] * x[i]` that the library
// build contracts to an FMA. The model's rows are 7 long, so only the tail loop ever runs; the
// kernel's lane order is not reproduced and a longer vector THROWS rather than being guessed.
double dot(const std::vector<double>& x, const std::vector<double>& y) {
    const std::size_t n = x.size() < y.size() ? x.size() : y.size();
    if (n >= 16)
        throw std::logic_error("np::dot: n >= 16 reaches OpenBLAS's SIMD kernel, whose reduction "
                               "order is not transcribed");
    double acc = 0.0;
    for (std::size_t i = 0; i < n; ++i) acc = std::fma(y[i], x[i], acc);
    return 0.0 + acc;
}

}  // namespace kp::rvol::np
