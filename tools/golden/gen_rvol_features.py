#!/usr/bin/env python3
"""rvol/features.py -- the averaged realized-variance estimator, the session-variance clock, and
window_sigma -- plus the numpy arithmetic under them.

WHAT IS PINNED, AND WHY EACH ONE:

  nplog      np.log on this box is Intel SVML (__svml_log8_ha), not glibc: ~1e-4 of index-level
             inputs differ by an ulp. Swept over realistic levels, the full exponent range,
             denormals, the specials, and every double within 2,000 ulps of each 1/32-reciprocal
             rounding boundary the SVML table lookup turns on.
  npsum      pairwise summation, lengths 0..2,500 (both sides of the 8 and 128 block edges).
  npmean     np.mean of a Python list, as averaged_rv calls it.
  npdot      np.dot of two 7-vectors, the model's design row (through OpenBLAS ddot).
  ms         int(dt.timestamp() * 1000), which is NOT floor(ns / 1e6).
  wallminus  `now - timedelta(minutes=m)` on an ET-aware datetime (wall-clock arithmetic).
  vt         variance_time, including the pre-open branch through the gap calendar.
  arv        averaged_rv on synthetic paths with gaps, zeros, NaNs, repeats and odd scales.
  ws         window_sigma on REAL cached second bars (index + ETF proxy), eight sessions across
             both underlyings, every lookback at its own fitted scale plus the target scale, at
             times of day chosen to straddle the 09:30 splice and the 12:30 end of the pre-open.

Series are emitted as `series <id> <n>` followed by `pt <t_ms> <repr(close)>` lines; repr is
round-trip exact through strtod.
"""
import argparse, datetime as dt, math, pathlib, sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
from gen_golden import emit, h, et_real, config  # noqa: E402

import numpy as np  # noqa: E402
from pricing import varcal  # noqa: E402
from rvol import data as rdata  # noqa: E402
from rvol import features as F  # noqa: E402
from rvol.dataset import LOOKBACKS, LOOKBACK_SCALE_S  # noqa: E402

ET = config.ET
EPOCH = dt.datetime(1970, 1, 1, tzinfo=dt.timezone.utc)


def ns(t):
    return ((t - EPOCH) // dt.timedelta(microseconds=1)) * 1000


def hs(xs):
    return " ".join(h(float(v)) for v in xs)


REAL_DAYS = [("SPX", "2026-08-21"), ("SPX", "2026-03-09"), ("SPX", "2025-11-28"),
             ("SPX", "2025-04-07"), ("NDX", "2026-08-20"), ("NDX", "2025-11-03"),
             ("NDX", "2024-12-24"), ("NDX", "2025-04-09")]

# Times of day for the real-bar windows. 09:30:00 exactly (no RTH yet), inside the first minutes,
# across the 12:30 point where the 3h lookback stops reaching the pre-open, and into the close.
NOWS = [(9, 30, 0, 0), (9, 30, 0, 1), (9, 31, 7, 250000), (9, 45, 0, 0), (10, 2, 33, 123456),
        (11, 0, 0, 0), (12, 29, 59, 999999), (12, 30, 0, 0), (12, 30, 0, 1), (13, 5, 1, 1),
        (14, 44, 44, 440000), (15, 30, 0, 0), (15, 59, 59, 999999), (16, 0, 0, 0)]


def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--out", default="tests/data/golden")
    out = pathlib.Path(ap.parse_args().out)
    L = []
    rng = np.random.default_rng(20260923)

    # ---- nplog ------------------------------------------------------------------------------
    xs = [rng.uniform(400, 800, 20000), rng.uniform(4000, 9000, 20000),
          rng.uniform(15000, 32000, 20000), np.round(rng.uniform(4000, 30000, 20000), 2),
          np.exp(rng.uniform(-700, 700, 10000)), rng.uniform(0.5, 2.0, 10000),
          rng.uniform(0.93, 1.07, 5000)]
    for j in range(16, 33):
        c = 32.0 / (j + 0.5)
        xs.append(c + np.arange(-2000, 2000, 7) * np.spacing(c))
    xs.append(np.array([5e-324, 1e-310, 2.2250738585072014e-308, 1.0, 2.0, 0.5, 1.5, 4.0 / 3.0,
                        np.nextafter(1.0, 0), np.nextafter(1.0, 2), 1.7976931348623157e308,
                        0.0, -0.0, -1.0, np.inf, -np.inf]))
    x = np.concatenate(xs)
    with np.errstate(divide="ignore", invalid="ignore"):
        y = np.log(x)
    for a, b in zip(x, y):
        L.append(f"nplog {h(a)} {int(np.float64(b).view(np.uint64)):#018x}")

    # ---- npsum / npmean ---------------------------------------------------------------------
    lens = list(range(0, 140)) + [255, 256, 257, 263, 511, 1023, 1024, 1025, 1999, 2500]
    lens += [int(v) for v in rng.integers(0, 700, 300)]
    for n in lens:
        a = (rng.standard_normal(n) * 10.0 ** rng.uniform(-8, 2, n)) ** 2
        if n and rng.random() < 0.3:
            a = rng.standard_normal(n) * 10.0 ** rng.uniform(-5, 5, n)
        L.append(f"npsum {n} {h(float(a.sum()))} {hs(a)}".rstrip())
    for _ in range(400):
        n = int(rng.integers(1, 70))
        lst = [float(v) for v in rng.random(n) * 10.0 ** rng.uniform(-12, -4)]
        L.append(f"npmean {n} {h(float(np.mean(lst)))} {hs(lst)}")

    # ---- npdot ------------------------------------------------------------------------------
    for _ in range(20000):
        a = rng.standard_normal(7) * 10 ** rng.uniform(-3, 1, 7)
        b = rng.standard_normal(7)
        L.append(f"npdot {h(float(np.dot(a, b)))} {hs(a)} {hs(b)}")

    # ---- ms / wallminus / annualized ----------------------------------------------------------
    t_lo = dt.datetime(2024, 1, 1, tzinfo=dt.timezone.utc).timestamp()
    t_hi = dt.datetime(2027, 1, 1, tzinfo=dt.timezone.utc).timestamp()
    dropped = 0
    for _ in range(20000):
        us = int(rng.integers(int(t_lo * 1e6), int(t_hi * 1e6)))
        t = dt.datetime.fromtimestamp(us // 10**6, ET).replace(microsecond=us % 10**6)
        L.append(f"ms {ns(t)} {F._ms(t)}")
        m = int(rng.choice([5, 15, 30, 60, 180, 1, 1439]))
        t0 = t - dt.timedelta(minutes=m)
        if et_real(t0) is None:
            dropped += 1
            continue
        L.append(f"wallminus {ns(t)} {m} {ns(t0)}")
    # the DST Sundays, walked in 7-minute steps
    for day in (dt.date(2026, 3, 8), dt.date(2025, 11, 2), dt.date(2026, 11, 1)):
        base = dt.datetime.combine(day, dt.time(0, 0), tzinfo=dt.timezone.utc)
        for k in range(0, 36 * 60, 7):
            t = (base + dt.timedelta(minutes=k, microseconds=k * 37)).astimezone(ET)
            for m in (60, 180):
                t0 = t - dt.timedelta(minutes=m)
                if et_real(t0) is None:
                    dropped += 1
                    continue
                L.append(f"wallminus {ns(t)} {m} {ns(t0)}")
    for v in list(rng.uniform(0, 1e-3, 2000)) + [0.0, -0.0, -1e-9, math.inf, math.nan, 1.0]:
        L.append(f"annualized {h(v)} {h(F.annualized(float(v)))}")

    # ---- variance_time ----------------------------------------------------------------------
    cals = {u: varcal.load(u) for u in ("SPX", "NDX")}
    for u, cal in cals.items():
        for _ in range(4000):
            d = dt.date(2026, 1, 2) + dt.timedelta(days=int(rng.integers(0, 330)))
            t1 = (dt.datetime.combine(d, dt.time(9, 30), tzinfo=ET)
                  + dt.timedelta(microseconds=int(rng.integers(0, 390 * 60 * 10**6 + 1))))
            t0 = t1 - dt.timedelta(microseconds=int(rng.integers(0, 17 * 3600 * 10**6)))
            if et_real(t0) is None or et_real(t1) is None:
                dropped += 1
                continue
            L.append(f"vt {u} {ns(t0)} {ns(t1)} {h(F.variance_time(cal, t0, t1))}")

    # ---- averaged_rv on synthetic paths ------------------------------------------------------
    sid = 0
    base_ms = int(dt.datetime(2026, 8, 21, 9, 30, tzinfo=ET).timestamp() * 1000)
    for kind in range(12):
        n = int(rng.integers(50, 12000))
        dts = rng.choice([1000, 1000, 1000, 5000, 1500, 250, 60000], n)
        ts = base_ms - 3 * 3600 * 1000 + np.cumsum(dts).astype("int64")
        px = 7000.0 * np.exp(np.cumsum(rng.standard_normal(n) * 1e-4))
        if kind % 3 == 1:
            px = np.round(px, 2)
        if kind % 4 == 2:
            bad = rng.integers(0, n, max(1, n // 200))
            px[bad] = rng.choice([0.0, -1.0, np.nan], bad.size)
        if kind == 5:
            ts[n // 2:n // 2 + 3] = ts[n // 2]          # repeated vendor timestamps
        L.append(f"series {sid} {n}")
        for a, b in zip(ts, px):
            L.append(f"pt {int(a)} {float(b)!r}")
        for _ in range(60):
            t0 = int(ts[0]) + int(rng.integers(-600_000, int(ts[-1] - ts[0])))
            t1 = t0 + int(rng.integers(-1000, 4 * 3600 * 1000))
            sc = int(rng.choice([10, 30, 60, 1, 7, 45]))
            no = int(rng.choice([0, 0, 0, 1, 3, 30, 7]))
            with np.errstate(divide="ignore", invalid="ignore"):
                rv, cov = F.averaged_rv(ts, px, t0, t1, scale_s=sc, n_offsets=no or None)
            L.append(f"arv {sid} {t0} {t1} {sc} {no} {h(rv)} {h(cov)}")
        sid += 1
    L.append(f"series {sid} 0")
    L.append(f"arv {sid} {base_ms} {base_ms + 600000} 30 0 {h(math.nan)} {h(0.0)}")
    empty_sid = sid
    sid += 1

    # ---- window_sigma on real bars -----------------------------------------------------------
    n_ws = 0
    for u, day in REAL_DAYS:
        d = dt.date.fromisoformat(day)
        src = rdata.SOURCES[u]
        ts_i, px_i = rdata.load_day(src["index"], d)
        ts_p, px_p = rdata.load_day(src["proxy"], d)
        ids = []
        for ts, px in ((ts_i, px_i), (ts_p, px_p)):
            L.append(f"series {sid} {len(ts)}")
            for a, b in zip(ts, px):
                L.append(f"pt {int(a)} {float(b)!r}")
            ids.append(sid)
            sid += 1
        cal = cals[u]
        for (hh, mm, ss, us) in NOWS:
            now = dt.datetime.combine(d, dt.time(hh, mm, ss, us), tzinfo=ET)
            for name, mins in LOOKBACKS:
                for sc in sorted({LOOKBACK_SCALE_S[name], F.SCALE_S}):
                    t0 = now - dt.timedelta(minutes=mins)
                    for pid in (ids[1], empty_sid):
                        tp, pp = (ts_p, px_p) if pid == ids[1] else (None, None)
                        sig, pf, cov = F.window_sigma(cal, ts_i, px_i, tp, pp, t0, now, scale_s=sc)
                        L.append(f"ws {u} {ids[0]} {pid} {ns(t0)} {ns(now)} {sc} "
                                 f"{h(sig)} {h(pf)} {h(cov)}")
                        n_ws += 1
    print(f"    (dropped {dropped} nonexistent wall clocks; {n_ws} real-bar windows)")
    emit(out, "rvol_features", "rvol/features.py", L)


if __name__ == "__main__":
    main()
