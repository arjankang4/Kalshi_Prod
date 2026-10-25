#!/usr/bin/env python3
"""rvol/live.py -- the IndexBuffer ring and LiveRealizedVol's cached features, driven the way the
quote loop drives them: one backfill, then a 5-second poll of the index snapshot.

The network fetches (massive.index.closes_ms, massive.history.second_bars) are monkeypatched with
REAL cached second bars for 2026-08-21, truncated at the backfill instant as a live fetch would
be, and the proxy bars carry junk the filter must drop (c missing / None / 0 / negative / NaN).
Exactly those rows are emitted, so the C++ fake fetch returns the same data.

Scenarios per underlying:
  ok           backfill 09:40:00.123456, poll to 16:05 (the index stops at 16:00, so the tail is
               all repeated vendor stamps), trims past 3h, pre-open windows splice the proxy.
  closes_fail  closes_ms raises: empty ring, proxy None, features None until two polls land.
  bars_fail    second_bars raises AFTER closes_ms succeeded: the index rows stay loaded, the
               proxy stays None, and backfilled is False -- the reference's partial state.
  late         backfill at 13:00; no window reaches the pre-open.

`time.monotonic` is patched to a stepped clock so the LEAN_RECOMPUTE_S cache is hit and missed on
its exact boundary (steps of 2.5s: hit, then recompute at exactly 5.0), with periodic force=True.
"""
import argparse, datetime as dt, math, pathlib, sys, time
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
from gen_golden import emit, h, config  # noqa: E402

import numpy as np  # noqa: E402
from massive import index as ix  # noqa: E402
from pricing import varcal  # noqa: E402
from rvol import data as rdata  # noqa: E402
from rvol import live as rlive  # noqa: E402

ET = config.ET
EPOCH = dt.datetime(1970, 1, 1, tzinfo=dt.timezone.utc)
DAY = dt.date(2026, 8, 21)


def ns(t):
    return ((t - EPOCH) // dt.timedelta(microseconds=1)) * 1000


def on(v):
    return "None" if v is None else h(v)


def feat_tokens(f):
    if f is None:
        return "None"
    return " ".join(f"{k}={on(v)}" for k, v in f.items())


def et(hh, mm, ss=0, us=0):
    return dt.datetime.combine(DAY, dt.time(hh, mm, ss, us), tzinfo=ET)


class Fail(RuntimeError):
    pass


def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--out", default="tests/data/golden")
    out = pathlib.Path(ap.parse_args().out)
    L = []
    rng = np.random.default_rng(777)
    clock = {"t": 1000.0}
    real_mono = time.monotonic
    real_closes, real_bars = rlive.ix.closes_ms, rlive.mhist.second_bars
    time.monotonic = lambda: clock["t"]
    try:
        for u in ("SPX", "NDX"):
            src = rdata.SOURCES[u]
            ts_i, px_i = rdata.load_day(src["index"], DAY)
            ts_p, px_p = rdata.load_day(src["proxy"], DAY)
            cal = varcal.load(u)
            for scen, b_at, end_at, mode in (("ok", et(9, 40, 0, 123456), et(16, 5), "ok"),
                                             ("closes_fail", et(9, 29), et(11, 0), "closes"),
                                             ("bars_fail", et(9, 31), et(10, 40), "bars"),
                                             ("late", et(13, 0), et(13, 30), "ok")):
                tag = f"{u}.{scen}"
                cut = int(b_at.timestamp() * 1000)
                rows = [(int(a), float(b)) for a, b in zip(ts_i, px_i) if a <= cut]
                bars = [{"t": int(a), "c": float(b)} for a, b in zip(ts_p, px_p) if a <= cut]
                # junk the backfill filter must drop, spliced in at fixed positions
                for pos, junk in ((5, {"t": 1}), (50, {"t": 2, "c": None}), (500, {"t": 3, "c": 0}),
                                  (900, {"t": 4, "c": -1.0}), (1200, {"t": 5, "c": math.nan}),
                                  (1500, {"t": 6, "c": 0.0})):
                    bars.insert(min(pos, len(bars)), junk)
                L.append(f"closes {tag} {len(rows)}")
                L.extend(f"pt {a} {b!r}" for a, b in rows)
                L.append(f"bars {tag} {len(bars)}")
                L.extend(f"bar {b['t']} {'missing' if 'c' not in b else on(b['c'])}" for b in bars)

                def fake_closes(und, day, _rows=rows, _mode=mode):
                    assert und == u and str(day) == str(DAY)
                    if _mode == "closes":
                        raise Fail("closes_ms down")
                    return list(_rows)

                def fake_bars(tk, a, b, _bars=bars, _mode=mode):
                    assert tk == src["proxy"] and a == b == str(DAY)
                    if _mode == "bars":
                        raise Fail("second_bars down")
                    return list(_bars)

                rlive.ix.closes_ms, rlive.mhist.second_bars = fake_closes, fake_bars
                buf = rlive.IndexBuffer(u)
                lrv = rlive.LiveRealizedVol(u, calendar=cal, buffer=buf)
                ok = buf.backfill(b_at)
                t_, p_ = buf.arrays()
                L.append(f"backfill {tag} {ns(b_at)} {mode} {1 if ok else 0} {len(t_)} "
                         f"{h(buf.span_s())} {'None' if buf.proxy_t is None else len(buf.proxy_t)}")

                k = 0
                t_poll = b_at
                prev_ts = None
                while t_poll <= end_at:
                    k += 1
                    t_poll = b_at + dt.timedelta(seconds=5 * k, microseconds=int(rng.integers(0, 900000)))
                    pm = int(t_poll.timestamp() * 1000)
                    j = int(np.searchsorted(ts_i, pm, side="right")) - 1
                    r = rng.random()
                    if j < 0 or r < 0.01:
                        q, kind = None, "none"
                    elif r < 0.02:
                        q, kind = ix.IndexQuote(u, src["index"], float(px_i[j]), None), "nots"
                    else:
                        ts_ns = int(ts_i[j]) * 1_000_000 + int(rng.integers(0, 1_000_000))
                        if r < 0.03 and prev_ts is not None:
                            ts_ns = prev_ts - int(rng.integers(0, 3_000_000_000))   # regression
                        q, kind = ix.IndexQuote(u, src["index"], float(px_i[j]), ts_ns), "q"
                        prev_ts = ts_ns
                    ret = buf.append(q)
                    t_, _ = buf.arrays()
                    qs = ("- -" if q is None else
                          f"{'None' if q.ts_ns is None else q.ts_ns} {h(q.value)}")
                    L.append(f"append {tag} {kind} {qs} {1 if ret else 0} {len(t_)} "
                             f"{h(buf.span_s())} {t_[0] if len(t_) else '-'}")
                    clock["t"] += 2.5
                    force = (k % 97 == 0)
                    with np.errstate(divide="ignore", invalid="ignore"):
                        f = lrv.features(now=t_poll, force=force)
                    L.append(f"feat {tag} {ns(t_poll)} {h(clock['t'])} {1 if force else 0} "
                             f"{feat_tokens(f)}")
    finally:
        time.monotonic = real_mono
        rlive.ix.closes_ms, rlive.mhist.second_bars = real_closes, real_bars
    emit(out, "rvol_live", "rvol/live.py", L)


if __name__ == "__main__":
    main()
