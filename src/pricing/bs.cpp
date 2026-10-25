#include "kp/pricing/bs.hpp"

namespace kp::pricing::bs {

double norm_cdf(double x) { return 0.5 * (1.0 + std::erf(x / SQRT2)); }

double norm_pdf(double x) { return std::exp(-0.5 * x * x) / SQRT_2PI; }

// Python's max(0.0, x) keeps the first argument unless the second compares strictly greater,
// so max(0.0, nan) is 0.0 and max(0.0, -0.0) is 0.0. std::fmax agrees on the NaN case but is
// implementation-defined for two zeros of differing sign, so spell out the Python form.
static inline double py_max0(double x) { return x > 0.0 ? x : 0.0; }

double intrinsic(double F, double K, char cp) {
    return cp == 'C' ? py_max0(F - K) : py_max0(K - F);
}

// (d1, d2) with r = q = 0. Caller guarantees F > 0, K > 0, w > 0.
D1D2 d1_d2(double F, double K, double w) {
    const double d1 = (std::log(F / K) + 0.5 * w * w) / w;
    return {d1, d1 - w};
}

// Undiscounted Black-Scholes. `w` is total stdev, not annualised vol.
OptD bs_price(double F, double K, double w, char cp) {
    if (F <= 0.0 || K <= 0.0) return MISSING;
    if (w <= 0.0) return intrinsic(F, K, cp);
    const D1D2 d = d1_d2(F, K, w);
    if (cp == 'C') return F * norm_cdf(d.d1) - K * norm_cdf(d.d2);
    return K * norm_cdf(-d.d2) - F * norm_cdf(-d.d1);
}

double vega_w(double F, double K, double w) {
    if (F <= 0.0 || K <= 0.0 || w <= 0.0) return 0.0;
    return F * norm_pdf(d1_d2(F, K, w).d1);
}

OptD p_above(double F, double K, double w) {
    if (F <= 0.0 || K <= 0.0) return MISSING;
    if (w <= 0.0) return F > K ? 1.0 : 0.0;
    return norm_cdf(d1_d2(F, K, w).d2);
}

OptD implied_w(double F, double K, OptD price, char cp,
               double lo, double hi, double min_time_value) {
    if (F <= 0.0 || K <= 0.0 || !price || *price < 0.0) return MISSING;
    const double intr = intrinsic(F, K, cp);
    const double cap = (cp == 'C') ? F : K;   // call <= F, put <= K, both undiscounted
    if (*price <= intr + min_time_value || *price >= cap - 1e-12) return MISSING;
    for (int i = 0; i < MAX_ITER; ++i) {
        const double mid = 0.5 * (lo + hi);
        // *bs_price is safe: F > 0 and K > 0 were checked above, so it is never MISSING.
        if (*bs_price(F, K, mid, cp) < *price) lo = mid;
        else                                   hi = mid;
        if (hi - lo < W_TOL) break;
    }
    return 0.5 * (lo + hi);
}

}  // namespace kp::pricing::bs
