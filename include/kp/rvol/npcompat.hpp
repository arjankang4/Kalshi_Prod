// numpy's float64 arithmetic, reproduced to the bit for the ops the live realized-vol path uses.
//
// The reference computes realized variance in numpy, and numpy is NOT libm plus a left fold. Each
// op below was measured against the reference interpreter's numpy 2.5.2 (OpenBLAS 0.3.34
// DYNAMIC_ARCH) on this box before being written down, and each disagreement is real:
//
//   np.log     on an AVX-512 machine (numpy's X86_V4 dispatch target) calls Intel SVML's
//              `__svml_log8_ha`, which disagrees with glibc's log on ~1e-4 of index-level
//              inputs by 1 ulp -- and a realized variance is a sum of thousands of squared log
//              differences, so an ulp in any level moves the answer. On a machine without the
//              X86_V4 feature set numpy falls back to glibc log, and so does this.
//   np.sum     is PAIRWISE (8 accumulators, blocks of <=128), not a left fold.
//   np.mean    is that sum divided by n.
//   np.dot     on two contiguous 1-D float64 vectors is `0.0 + cblas_ddot(...)`, and OpenBLAS's
//              short-vector tail loop is FMA-contracted: `dot = fma(y[i], x[i], dot)`.
//
// See src/rvol/npcompat.cpp for the evidence behind each.
#pragma once
#include <cstddef>
#include <vector>

namespace kp::rvol::np {

// True when numpy on this CPU would dispatch its X86_V4 (AVX-512 F/CD/BW/DQ/VL) loops.
bool x86_v4();

// One lane of Intel SVML's __svml_log8_ha, transcribed from its disassembly in numpy's
// _multiarray_umath. Exact for every input; the special classes (NaN, +-0, +-inf, negative)
// take SVML's rare path, whose answers coincide with glibc's. Requires AVX-512F for the one
// instruction with no portable spelling (vrcp14pd), so call only when x86_v4().
double svml_log(double x);

// np.log(a) for a contiguous float64 array: svml_log per element on X86_V4, glibc log otherwise.
std::vector<double> log(const std::vector<double>& a);

// numpy's pairwise summation of a[0..n), exactly as DOUBLE_add's reduce loop runs it.
double pairwise_sum(const double* a, std::size_t n);

// a.sum() / np.sum(a) for a 1-D contiguous float64 array (numpy's add.reduce: -0.0 identity
// then the pairwise sum).
double sum(const std::vector<double>& a);

// np.mean(a) for a 1-D float64 array. NaN for empty (numpy warns and returns nan).
double mean(const std::vector<double>& a);

// float(np.dot(x, y)) for equal-length contiguous float64 vectors, through OpenBLAS ddot.
double dot(const std::vector<double>& x, const std::vector<double>& y);

}  // namespace kp::rvol::np
