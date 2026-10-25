// Realized variance: the averaged estimator, the session-variance clock, and one window's sigma.
//
// Port of the LIVE half of Kalshi/rvol/features.py: averaged_rv, variance_time,
// annualized, _ms and window_sigma, which is everything rvol/live.py reaches. The feature/target
// matrix builders stay in Python.
//
// THE ESTIMATOR is realized variance averaged over every one-second-offset sub-grid of the
// sampling scale -- 60 grids at 60s, 10 at 10s -- expressed as a RATE times the nominal window so
// ragged coverage does not weight one offset over another. It is numpy in the reference, and the
// arithmetic here is numpy's, not libm's: see kp/rvol/npcompat.hpp. `np.log` on this box is
// Intel SVML and differs from glibc by an ulp on ~1e-4 of index levels, which a sum of thousands
// of squared log returns does not forgive.
//
// THE CLOCK is session-variance units: one RTH session == 1.0, the pre-open portion of a window
// apportioned by the overnight SHAPE (pricing/gapcal) times varcal's overnight_share. Then
// annualized at 252, the same clock HourlyPricer.sigma_annualized uses.
//
// DATETIME ARITHMETIC IS WALL-CLOCK WHERE THE REFERENCE'S IS. Python compares and subtracts two
// aware datetimes that share a tzinfo on their naive fields, and `now - timedelta(minutes=m)` is
// field arithmetic. Every such site here does the same, so the DST Sundays agree with the
// reference rather than with physics.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "kp/pricing/gapcal.hpp"
#include "kp/pricing/varcal.hpp"
#include "kp/time/et.hpp"

namespace kp::rvol::features {

inline constexpr int SCALE_S = 30;              // default scale, and every TARGET's
inline constexpr int N_OFFSETS = 0;             // 0 == Python None: one sub-grid per second
inline constexpr double TRADING_DAYS = 252.0;
inline constexpr int OPEN_H = 9, OPEN_M = 30;
inline constexpr int CLOSE_H = 16, CLOSE_M = 0;

// A window with less than this fraction of its variance-time covered is not measured but
// guessed; it comes back NaN rather than as a small rv that reads as low volatility.
inline constexpr double MIN_COVERAGE = 0.6;

struct RvCov { double rv; double coverage; };

// Averaged realized variance over [t0_ms, t1_ms) at `scale_s`: (rv, coverage), or (nan, 0.0).
// `ts_ms` ascending; empty == Python None.
RvCov averaged_rv(const std::vector<std::int64_t>& ts_ms, const std::vector<double>& px,
                  std::int64_t t0_ms, std::int64_t t1_ms, int scale_s = SCALE_S,
                  int n_offsets = N_OFFSETS);

// rvol.features.gap_calendar: the overnight shape, loaded once per (underlying, profile, root)
// and cached for the process. THROWS if the artifact is missing -- deliberately, as the
// reference does: a silent fallback would make a missing calendar look like a working model.
const kp::pricing::gapcal::GapCalendar& gap_calendar(const std::string& underlying,
                                                     const std::string& profile = "default",
                                                     const std::string& root = "");

// Variance-time between two ET instants in session units. `t1` is assumed inside RTH. `gcal`
// null means gap_calendar(cal.underlying(), "default", root) -- loaded only if `t0` is pre-open.
double variance_time(const kp::pricing::varcal::VarianceCalendar& cal,
                     const kp::time::Instant& t0, const kp::time::Instant& t1,
                     const kp::pricing::gapcal::GapCalendar* gcal = nullptr,
                     const std::string& root = "");

// session-variance units -> annualized sigma. NaN for non-finite or negative input.
double annualized(double session_var);

// int(dt.timestamp() * 1000): microseconds (datetime's resolution; ns are truncated as datetime
// would), divided once by 1e6, times 1000, truncated toward zero -- NOT ns / 1e6.
std::int64_t ms(const kp::time::Instant& t);

// `t - timedelta(minutes=m)` on an ET-aware datetime: wall-clock field arithmetic, then the
// ET offset of the resulting wall time (first occurrence when ambiguous, fold=0).
kp::time::Instant wall_minus_minutes(const kp::time::Instant& t, int minutes);

// (b - a).total_seconds() for two datetimes that both carry config.ET: Python subtracts their
// NAIVE fields (wall clock), exact in microseconds. Differs from kp::time::total_seconds only
// across a DST transition.
double wall_seconds(const kp::time::Instant& a, const kp::time::Instant& b);

struct WindowSigma { double sigma, proxy_frac, coverage; };

// Annualized sigma implied by the window [t0, t1), splicing the index (RTH) with the ETF proxy
// (pre-open). (nan, nan, coverage) when the window is too sparsely covered. Proxy arrays empty ==
// Python None.
WindowSigma window_sigma(const kp::pricing::varcal::VarianceCalendar& cal,
                         const std::vector<std::int64_t>& ts_idx, const std::vector<double>& px_idx,
                         const std::vector<std::int64_t>& ts_px, const std::vector<double>& px_px,
                         const kp::time::Instant& t0, const kp::time::Instant& t1,
                         int scale_s = SCALE_S,
                         const kp::pricing::gapcal::GapCalendar* gcal = nullptr,
                         const std::string& root = "");

}  // namespace kp::rvol::features
