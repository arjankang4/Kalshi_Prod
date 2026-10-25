#!/usr/bin/env python3
"""strategy/edgemgr.py -- the per-market required edge, its refusals, the adverse gate, the cache.

NO R, NO QUOTE is the property under test, asserted from all five directions: no calendar, no
calibration artifact, missing forward/sigma, no settlement timestamp, and an R the curve refuses.
Each returns a DIFFERENT reason string, so a port that collapsed them would still price identically
and would make every unquotable board say the same thing.

THE TWO REFUSALS THAT LOOK ALIKE AND ARE NOT. `R is None` degrades to the flat no-R record; an R
past the calibrated maximum is a REFUSAL that must not degrade. They differ in `source` ("no_R" vs
"curve") and in whether R is carried, and both are swept.

THE CHARGE IN TWO PARTS: `floor_micros` and `bonus_micros` ride beside the total, and
the ADVERSE-SELECTION GATE refuses a market whose |delta| * move_scale exceeds the edge charged.
Both are swept, the gate on both sides of its boundary.

THE EDGE_RECOMPUTE_S CACHE is swept on its exact boundary by driving `time.monotonic` from here.

Markets are emitted as `market` lines so the C++ builds the identical board rather than a copy of
this loop that could drift from it.
"""
import argparse, datetime as dt, pathlib, random, sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
from gen_golden import emit, h, config  # noqa: E402
from pricing import edge as pedge  # noqa: E402
from pricing import varcal as vc  # noqa: E402
from strategy import edgemgr as emod  # noqa: E402


def on(v):
    return "None" if v is None else h(v)


def oi(v):
    return "None" if v is None else str(v)


D = dt.date(2026, 8, 20)


def et(hh, mm, ss=0):
    return dt.datetime.combine(D, dt.time(hh, mm, ss), tzinfo=config.ET)


def ns(t):
    return int(t.timestamp() * 1_000_000_000)


def mk(tk, styp, lo, hi, close):
    return {"ticker": tk, "strike_type": styp, "floor_strike": lo, "cap_strike": hi,
            "close_time": close.isoformat().replace("+00:00", "Z") if close else None}


def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--out", default="tests/data/golden")
    out = pathlib.Path(ap.parse_args().out)
    L = []
    F, SIG = 7691.1, 0.12
    close16 = et(16, 0).astimezone(dt.timezone.utc)
    markets = []
    # +-150 points is about +-2.6 sigma at SIG on a 6h horizon; wider puts every market past 7
    # sigma where R underflows and the curve returns its minimum for all of them.
    for i, off in enumerate(range(-150, 155, 5)):
        markets.append(mk(f"D{i}", "greater", F + off, None, close16))
    for i, off in enumerate(range(-60, 65, 5)):
        markets.append(mk(f"B{i}", "between", F + off, F + off + 24.9999, close16))
    markets.append(mk("NOCLOSE", "greater", F, None, None))
    markets.append(mk("BADTYPE", "unhandled", F, None, close16))
    markets.append(mk("SOON", "greater", F, None, et(15, 59).astimezone(dt.timezone.utc)))
    markets.append(mk("PAST", "greater", F, None, et(9, 0).astimezone(dt.timezone.utc)))
    for m in markets:
        ct = emod.settlement_of(m)
        L.append(f"market {m['ticker']} {m['strike_type'] or '-'} {on(m['floor_strike'])} "
                 f"{on(m['cap_strike'])} {'None' if ct is None else int(ct.timestamp())}")

    def dump(tag, em, tick):
        for m in markets:
            r = em.get(m["ticker"])
            L.append(f"rec {tag} {tick} {m['ticker']} {oi(r.get('edge_micros'))} "
                     f"{on(r.get('edge'))} {h(r.get('risk_u'))} {h(r.get('scale_pos'))} "
                     f"{on(r.get('R'))} {on(r.get('delta'))} {on(r.get('vega'))} "
                     f"{on(r.get('theta'))} {on(r.get('w'))} {on(r.get('var_left'))} "
                     f"{on(r.get('minutes_to_close'))} {1 if r.get('quotable') else 0} "
                     f"{r.get('source') or '-'} {r.get('reason') or '-'} "
                     f"{oi(r.get('floor_micros'))} {oi(r.get('bonus_micros'))} "
                     f"{on(r.get('adverse_dollars'))} {on(r.get('move_scale'))}")

    def summ(tag, em):
        s = em.summary()
        L.append(f"summary {tag} {s.get('n', 0)} {s.get('n_priced', 0)} {s.get('refused', 0)} "
                 f"{on(s.get('edge_min'))} {on(s.get('edge_med'))} {on(s.get('edge_max'))} "
                 f"{1 if s.get('enabled') else 0} {s.get('reason') or '-'} "
                 f"{1 if 'n_priced' in s else 0}")

    for u in ("SPX", "NDX"):
        cal = vc.load(u)
        curve = pedge.Curve.load(u)
        for now_h, now_m in ((10, 0), (13, 30), (15, 58), (15, 59)):
            now = et(now_h, now_m)
            em = emod.EdgeManager(u, curve=curve, calendar=cal)
            em.refresh(markets, F, SIG, now, force=True)
            tag = f"{u}@{now_h:02d}{now_m:02d}"
            L.append(f"run {tag} {ns(now)} {h(F)} {h(SIG)} None")
            dump(tag, em, 0)
            summ(tag, em)
            # THE ADVERSE GATE, on the same board: from a move no market fails to one every
            # at-the-money market fails. 0.25 pts is the measured regime.
            for ms in (0.0, 0.05, 0.25, 1.0, 5.0):
                em = emod.EdgeManager(u, curve=curve, calendar=cal)
                em.refresh(markets, F, SIG, now, force=True, move_scale=ms)
                tag2 = f"{tag}~{ms}"
                L.append(f"run {tag2} {ns(now)} {h(F)} {h(SIG)} {h(ms)}")
                dump(tag2, em, 0)
                summ(tag2, em)

        for fwd, sg in ((None, SIG), (F, None), (F, 0.0), (F, -0.1), (None, None)):
            em = emod.EdgeManager(u, curve=curve, calendar=cal)
            em.refresh(markets, fwd, sg, et(12, 0), force=True)
            r = em.get("D0")
            L.append(f"noinput {u} {on(fwd)} {on(sg)} {1 if r['quotable'] else 0} "
                     f"{r['source']} {r['reason']} {oi(r.get('edge_micros'))} {h(r['risk_u'])} "
                     f"{oi(r.get('floor_micros'))} {oi(r.get('bonus_micros'))}")

        for op_flat in (False, True):
            em = emod.EdgeManager(u, curve=curve, calendar=cal)
            em.operator_flat = op_flat
            em.refresh(markets, None, None, et(12, 0), force=True)
            r = em.get("D0")
            L.append(f"override {u} {1 if op_flat else 0} {1 if r['quotable'] else 0} "
                     f"{r['source']} {r['reason']} {oi(r.get('edge_micros'))}")
            L.append(f"unknown {u} {1 if op_flat else 0} "
                     f"{em.get('NOSUCHTICKER')['reason']} "
                     f"{1 if em.get('NOSUCHTICKER')['quotable'] else 0}")

    # THE EDGE_RECOMPUTE_S CACHE on its boundary. Inside the window the map is returned UNTOUCHED although the
    # board moved; `force` bypasses it.
    cal = vc.load("SPX")
    curve = pedge.Curve.load("SPX")
    clock = {"t": 1000.0}
    real_monotonic = emod.time.monotonic
    emod.time.monotonic = lambda: clock["t"]
    try:
        em = emod.EdgeManager("SPX", curve=curve, calendar=cal)
        RW = float(config.EDGE_RECOMPUTE_S)
        for dt_s, force, fwd in ((0.0, False, F), (0.0, False, F),
                                 (RW - 0.001, False, F * 1.05), (RW, False, F * 1.05),
                                 (RW, False, F * 1.05), (0.0, True, F * 1.10),
                                 (10.0, False, F * 1.20), (100.0, False, F * 1.20)):
            clock["t"] += dt_s
            em.refresh(markets, fwd, SIG, et(12, 0), force=force)
            r = em.get("D0")
            L.append(f"cache {h(clock['t'])} {1 if force else 0} {h(fwd)} {ns(et(12, 0))} "
                     f"{oi(r.get('edge_micros'))} {on(r.get('R'))} {on(r.get('delta'))}")
    finally:
        emod.time.monotonic = real_monotonic

    em = emod.EdgeManager("NOSUCH")
    L.append(f"unavailable NOSUCH {1 if em.available else 0} {1 if em.enabled else 0} "
             f"{em.reason or '-'}")
    em.refresh(markets, 7691.1, 0.12, et(12, 0), force=True)
    r = em.get("D0")
    L.append(f"unavailable_rec NOSUCH {1 if r['quotable'] else 0} {r['source']} {r['reason']}")
    emit(out, "edgemgr", "strategy/edgemgr.py", L)


if __name__ == "__main__":
    main()
