#!/usr/bin/env python3
"""strategy/leanchain.py -- the realized-vol shadow chain, end to end, the way run_04 drives it.

LeanChain(U) is built for real (the live variance calendar and rvol model), started through a
monkeypatched backfill that returns REAL cached second bars for 2026-08-21 truncated at the start
instant, then fed a 5-second poll of real index levels through observe(). At ~65 checkpoints
across the session chain() is read against the REAL captured SPXW board (skew_boards.vec board
0, taken 2026-08-21 11:50 ET) and the REAL KXINXU/KXINX market list from the same fixture --
hourly boards at 12:00-15:00 plus the 4pm board -- in three variants:

    A  enabled=None (config.LEAN_ENABLED), no smile        -> flat Black-Scholes p_rv/p_ref
    B  enabled=False, the board's fitted smile             -> p_rv None, shadow through the smile
    C  enabled=True, a REFUSED (flat) smile                -> falls back to Black-Scholes

plus synthetic markets for every row refusal (no settlement, 4pm, past, between, less, a
duplicate ticker, a next-day settlement), an empty board (atm_* refusal), a forward of None, a
closes_ms outage (the buffer fills from polls alone), and an unknown underlying (unavailable).

The three rules test_leanchain.py pins -- no lean on the 4pm board, each market at its OWN
settlement, raw implied as the reference -- are all visible in these rows; the C++ suite also
asserts them directly.

`time.monotonic` is a stepped clock; the checkpoint's value is emitted so the C++ passes the same.
"""
import argparse, datetime as dt, json, math, pathlib, sys, time, types
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
import gen_golden  # noqa: E402
from gen_golden import emit, h, config, captures  # noqa: E402

import numpy as np  # noqa: E402
from massive import index as ix  # noqa: E402
from pricing import atmvol, skew  # noqa: E402
from rvol import data as rdata  # noqa: E402
from rvol import live as rlive  # noqa: E402
from strategy import edgemgr  # noqa: E402
from strategy import leanchain as lcm  # noqa: E402

ET = config.ET
EPOCH = dt.datetime(1970, 1, 1, tzinfo=dt.timezone.utc)
DAY = dt.date(2026, 8, 21)


def ns(t):
    return ((t - EPOCH) // dt.timedelta(microseconds=1)) * 1000


def on(v):
    return "None" if v is None else h(v)


def os_(v):
    return "-" if v is None else str(v)


def et(hh, mm, ss=0, us=0, day=DAY):
    return dt.datetime.combine(day, dt.time(hh, mm, ss, us), tzinfo=ET)


def zulu(t):
    return t.astimezone(dt.timezone.utc).isoformat().replace("+00:00", "Z")


class Fail(RuntimeError):
    pass


def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--out", default="tests/data/golden")
    out = pathlib.Path(ap.parse_args().out)
    L = []
    rng = np.random.default_rng(90210)

    board, strikes = gen_golden._real_boards()[0]
    afit = atmvol.fit(board, strikes)
    assert afit.ok, afit.reason
    real_fit = skew.fit(board, strikes, afit.forward, afit.w)
    flat_fit = skew.fit(board, strikes, None, None)
    assert real_fit.ok and not flat_fit.ok and flat_fit.flat

    # The market list: the real hourly + 4pm KXINXU board, thinned on the 4pm side, plus
    # synthetic rows reaching every refusal.
    mk_file = [p for p in captures("*/markets_iter2.json")][0]
    raw = json.loads(mk_file.read_text())
    markets = [m for m in raw if not m["close_time"].startswith("2026-08-21T20")]
    markets += [m for m in raw if m["close_time"].startswith("2026-08-21T20")][::7]
    markets += [
        {"ticker": "SYN-NOCLOSE", "strike_type": "greater", "floor_strike": 7700, "cap_strike": None,
         "close_time": None},
        {"ticker": "SYN-BETWEEN", "strike_type": "between", "floor_strike": 7690,
         "cap_strike": 7699.9999, "close_time": zulu(et(13, 0))},
        {"ticker": "SYN-LESS", "strike_type": "less", "floor_strike": None, "cap_strike": 7650,
         "close_time": zulu(et(14, 0))},
        {"ticker": "SYN-PAST", "strike_type": "greater", "floor_strike": 7700, "cap_strike": None,
         "close_time": zulu(et(9, 0))},
        {"ticker": "SYN-1559", "strike_type": "greater", "floor_strike": 7690.5, "cap_strike": None,
         "close_time": zulu(et(15, 59, 59))},
        {"ticker": "SYN-1600:01", "strike_type": "greater", "floor_strike": 7700, "cap_strike": None,
         "close_time": zulu(et(16, 0, 1))},
        {"ticker": "SYN-TOMORROW", "strike_type": "greater", "floor_strike": 7700, "cap_strike": None,
         "close_time": zulu(et(12, 0, day=DAY + dt.timedelta(days=1)))},
        {"ticker": "SYN-DUP", "strike_type": "greater", "floor_strike": 7650, "cap_strike": None,
         "close_time": zulu(et(12, 0))},
        {"ticker": "SYN-DUP", "strike_type": "greater", "floor_strike": 7750, "cap_strike": None,
         "close_time": zulu(et(15, 0))},
    ]
    for m in markets:
        ct = edgemgr.settlement_of(m)
        L.append(f"market {m['ticker']} {m.get('strike_type') or '-'} {on(m.get('floor_strike'))} "
                 f"{on(m.get('cap_strike'))} {'None' if ct is None else int(ct.timestamp())}")

    # THE SWEEP LIST: 2,000 markets with distinct whole-second settlements through the session,
    # so every chain() over it evaluates 2,000 distinct remaining-variance ratios. `sqrt(ratio)`
    # and `ratio ** 0.5` differ on ~0.086% of doubles; the ~250 settlements of the real list
    # cannot tell them apart, twenty thousand can.
    sweep = []
    for i in range(2000):
        close = et(9, 30) + dt.timedelta(seconds=int(rng.integers(0, 390 * 60)))
        sweep.append({"ticker": f"SW{i}", "strike_type": "greater_or_equal",
                      "floor_strike": float(np.round(rng.uniform(7550, 7850), 2)),
                      "cap_strike": None, "close_time": zulu(close)})
    for m in sweep:
        ct = edgemgr.settlement_of(m)
        L.append(f"smarket {m['ticker']} {m['strike_type']} {on(m['floor_strike'])} None "
                 f"{int(ct.timestamp())}")

    clock = {"t": 5000.0}
    start_at = {"t": None}

    class FakeDT(dt.datetime):
        @classmethod
        def now(cls, tz=None):
            return start_at["t"]

    real_mono, real_dt = time.monotonic, rlive.datetime
    real_closes, real_bars = rlive.ix.closes_ms, rlive.mhist.second_bars
    time.monotonic = lambda: clock["t"]
    rlive.datetime = types.SimpleNamespace(datetime=FakeDT, timedelta=dt.timedelta)

    def dump(tag, res):
        m = res["_meta"]
        rvf = m.get("rv_features")
        rvf_s = "None" if rvf is None else " ".join(f"{k}={on(v)}" for k, v in rvf.items())
        L.append(f"meta {tag} {os_(m.get('lean_reason'))} {h(m['buffer_span_s'])} "
                 f"{on(m.get('variance_left'))} {on(m.get('w_impl'))} {on(m.get('sigma_impl'))} "
                 f"{on(m.get('forward'))} {os_(m.get('rv_horizon'))} {on(m.get('minutes_left'))} "
                 f"{on(m.get('sigma_rv'))} {1 if m['enabled'] else 0} {1 if m['smiled'] else 0} "
                 f"{rvf_s}")
        n = 0
        for tk, r in res.items():
            if tk == "_meta":
                continue
            n += 1
            L.append(f"row {tag} {tk} {on(r.get('p_rv'))} {on(r.get('p_rv_shadow'))} "
                     f"{on(r.get('p_ref'))} {on(r.get('sigma_rv'))} {on(r.get('sigma_impl'))} "
                     f"{on(r.get('w_rv'))} {on(r.get('w_impl'))} {1 if r['enabled'] else 0} "
                     f"{os_(r.get('reason'))} {os_(r.get('rv_horizon'))} {on(r.get('minutes'))}")
        first = next((r for r in res.values() if r.get("sigma_rv")), {}).get("sigma_rv")
        L.append(f"end {tag} {n} {on(first)}")

    try:
        for u, mode, b_at, end_at in (("SPX", "ok", et(9, 35, 0, 250000), et(16, 2)),
                                      ("NDX", "ok", et(10, 10), et(15, 1)),
                                      ("SPX", "closes", et(9, 25), et(10, 30))):
            src = rdata.SOURCES[u]
            ts_i, px_i = rdata.load_day(src["index"], DAY)
            ts_p, px_p = rdata.load_day(src["proxy"], DAY)
            scen = f"{u}.{mode}"
            cut = int(b_at.timestamp() * 1000)
            rows = [(int(a), float(b)) for a, b in zip(ts_i, px_i) if a <= cut]
            bars = [{"t": int(a), "c": float(b)} for a, b in zip(ts_p, px_p) if a <= cut]
            L.append(f"closes {scen} {len(rows)}")
            L.extend(f"pt {a} {b!r}" for a, b in rows)
            L.append(f"bars {scen} {len(bars)}")
            L.extend(f"bar {b['t']} {h(b['c'])}" for b in bars)

            def fake_closes(und, day, _rows=rows, _mode=mode):
                if _mode == "closes":
                    raise Fail("closes_ms down")
                return list(_rows)

            def fake_bars(tk, a, b, _bars=bars):
                return list(_bars)

            rlive.ix.closes_ms, rlive.mhist.second_bars = fake_closes, fake_bars
            start_at["t"] = b_at
            lc = lcm.LeanChain(u)
            assert lc.ok, lc.reason
            ok = lc.start()
            L.append(f"start {scen} {ns(b_at)} {mode} {1 if ok else 0}")

            k, ck = 0, 0
            t_poll = b_at
            while t_poll <= end_at:
                k += 1
                t_poll = b_at + dt.timedelta(seconds=5 * k, microseconds=int(rng.integers(0, 900000)))
                pm = int(t_poll.timestamp() * 1000)
                j = int(np.searchsorted(ts_i, pm, side="right")) - 1
                clock["t"] += 2.5
                if j < 0 or rng.random() < 0.01:
                    q, qs = None, "none - -"
                else:
                    tsn = int(ts_i[j]) * 1_000_000 + int(rng.integers(0, 1_000_000))
                    q = ix.IndexQuote(u, src["index"], float(px_i[j]), tsn)
                    qs = f"q {tsn} {h(q.value)}"
                lc.observe(q)
                L.append(f"observe {scen} {qs}")
                if k % 70 != 0 and t_poll <= end_at:
                    continue
                ck += 1
                fwd = float(px_i[j]) if j >= 0 else None
                if ck % 11 == 5:
                    fwd = None
                variants = [("A", None, None, "real"), ("B", False, "real", "real"),
                            ("C", True, "flat", "real")]
                variants = [v + ("main",) for v in variants]
                if ck % 9 == 4:
                    variants.append(("D", True, "real", "empty", "main"))
                if ck % 6 == 0 and mode == "ok":
                    variants.append(("S", True, "real", "real", "sweep"))
                    variants.append(("T", None, None, "real", "sweep"))
                for var, en, sk, bd, mk in variants:
                    tag = f"{scen}#{ck}{var}"
                    L.append(f"chain {tag} {ns(t_poll)} {h(clock['t'])} {on(fwd)} "
                             f"{'None' if en is None else int(en)} {sk or 'none'} {bd} {mk}")
                    sf = {"real": real_fit, "flat": flat_fit, None: None}[sk]
                    qb, st = (board, strikes) if bd == "real" else ({}, [])
                    with np.errstate(divide="ignore", invalid="ignore"):
                        res = lc.chain(markets if mk == "main" else sweep, qb, st, t_poll, fwd,
                                       enabled=en, skew_fit=sf)
                    dump(tag, res)

        # An underlying with no artifacts: unavailable, and chain() says so without raising.
        lc = lcm.LeanChain("XYZ")
        L.append(f"unavailable XYZ {1 if lc.ok else 0} {1 if lc.reason else 0}")
        res = lc.chain(markets, board, strikes, et(12, 0), 7700.0)
        m = res["_meta"]
        L.append(f"unavailable_chain XYZ {len(res) - 1} {1 if m['lean_reason'] else 0} "
                 f"{h(m['buffer_span_s'])} {1 if m['enabled'] else 0}")
    finally:
        time.monotonic = real_mono
        rlive.datetime = real_dt
        rlive.ix.closes_ms, rlive.mhist.second_bars = real_closes, real_bars
    emit(out, "leanchain", "strategy/leanchain.py", L)


if __name__ == "__main__":
    main()
