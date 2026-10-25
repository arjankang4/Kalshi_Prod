#!/usr/bin/env python3
"""pricing/basis.py -- BasisTracker: the index-tick EMA of `F - index` and the fast spot.

Driven as OPERATION SCRIPTS: every tracker is created, fed and read here, and each line carries
the op's inputs and every output, so the C++ replays the same sequence against its own tracker.
Outputs are compared twice -- the floats bit-exactly (float.hex), and the whole `detail` dict as
json.dumps text, which pins key order, int-vs-float, round(_, 4)/round(_, 3) and the f-string
reason spellings (`basis_break_+20.00pts`, `basis_warming_3/15`) in one comparison.

Branches swept: first fold, repeated ts_ns (ignored), ts None (never deduplicated), value None /
futures None (not folded), negative dt (clamped to 0), half-life <= 0 (w = 0), warming, no
futures, the break detector on both sides of its band, alarm_pct 0 (band off), non-finite and
non-positive spot, NaN inputs, and the refusal counter's first-occurrence key order.

    new   <id> <underlying> <half_life|None> <alarm_pct|None> <min_samples>
    obs   <id> <fmid|None> <value|None|-> <ts|None> <now> <folded 0/1>      (value '-' = no quote)
    spot  <id> <fmid|None> <spot|None> \t <detail json>
    det   <id> \t <detail json>
    basis <id> <ema|None> <ready>
"""
import argparse, json, math, pathlib, random, sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
from gen_golden import emit, h  # noqa: E402
from massive import index as ix  # noqa: E402
from pricing import basis as bm  # noqa: E402

NS = 1_000_000_000


def on(v):
    return "None" if v is None else h(v)


def J(d):
    return json.dumps(d, separators=(",", ":"))


def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--out", default="tests/data/golden")
    out = pathlib.Path(ap.parse_args().out)
    L = []
    trk = {}

    def new(i, u, hl=None, apct=None, ms=bm.MIN_SAMPLES):
        trk[i] = bm.BasisTracker(u, half_life_s=hl, alarm_pct=apct, min_samples=ms)
        L.append(f"new {i} {u} {on(hl)} {on(apct)} {ms}")

    def obs(i, fmid, value, ts, now, u="SPX", noquote=False):
        q = None if noquote else ix.IndexQuote(u, ix.INDEX_TICKER.get(u, "I:X"), value, ts)
        r = trk[i].observe(fmid, q, now=now)
        vtok = "-" if noquote else on(value)
        L.append(f"obs {i} {on(fmid)} {vtok} {'None' if ts is None else ts} {h(now)} {1 if r else 0}")

    def spot(i, fmid):
        s, d = trk[i].spot(fmid)
        L.append(f"spot {i} {on(fmid)} {on(s)}\t{J(d)}")

    def det(i):
        L.append(f"det {i}\t{J(trk[i].detail())}")
        L.append(f"basis {i} {on(trk[i].basis)} {1 if trk[i].ready else 0}")

    # --- the reference's own test scenarios, verbatim inputs ---
    new(0, "SPX", 10.0, None, 3)
    for i in range(60):
        obs(0, 7700.0 + i * 0.1 + 14.0, 7700.0 + i * 0.1, (i + 1) * NS, 1000.0 + i)
    for _ in range(5):
        obs(0, 7800.0, 7706.0, 60 * NS, 1060.0)
    spot(0, 7720.0); spot(0, 7725.0); det(0)

    new(1, "SPX")
    spot(1, 7720.0)
    for i in range(20):
        obs(1, 7714.0, 7700.0, (i + 1) * NS, 1000.0 + i)
        spot(1, 7720.0)
    spot(1, None); det(1)

    new(2, "SPX", 60.0, None, 3)
    for i in range(60):
        obs(2, 7714.0, 7700.0, (i + 1) * NS, 1000.0 + i)
    obs(2, 7734.0, 7700.0, 61 * NS, 1061.0)
    spot(2, 7734.0); spot(2, None); spot(2, 7734.0); det(2)

    for i, (u, lvl) in enumerate((("SPX", 7700.0), ("NDX", 29000.0))):
        new(3 + i, u, None, None, 3)
        for k in range(30):
            obs(3 + i, lvl + 14.0 + (67.0 if u == "NDX" else 0.0), lvl, (k + 1) * NS, 1000.0 + k, u=u)
        det(3 + i)

    # --- edge branches ---
    new(5, "SPX", 10.0, None, 1)
    obs(5, None, 7700.0, 1 * NS, 0.0)                 # no futures: not folded
    obs(5, 7714.0, None, 1 * NS, 0.0)                 # no value: not folded
    obs(5, 7714.0, 7700.0, 1 * NS, 0.0, noquote=True)  # no quote at all
    spot(5, 7714.0)                                   # warming 0/1
    obs(5, 7714.0, 7700.0, None, 0.0)                 # ts None: folded
    obs(5, 7715.0, 7700.0, None, 0.0)                 # ts None again: STILL folded (never dedup)
    obs(5, 7716.0, 7700.0, 5 * NS, -3.0)              # clock went BACKWARDS: dt clamps to 0
    obs(5, 7716.0, 7700.0, 5 * NS, 10.0)              # repeated ts after a None: ignored
    spot(5, 7716.0); spot(5, 1.0); spot(5, -50.0); det(5)

    new(6, "SPX", 0.0, 0.0, 1)                        # half-life 0 -> w = 0; alarm band off
    obs(6, 7714.0, 7700.0, 1 * NS, 0.0)
    obs(6, 7800.0, 7700.0, 2 * NS, 1.0)
    spot(6, 7800.0); det(6)
    new(7, "SPX", -5.0, 1e-9, 1)                      # negative half-life -> w = 0; tiny band
    obs(7, 7714.0, 7700.0, 1 * NS, 0.0)
    obs(7, 7714.5, 7700.0, 2 * NS, 1.0)
    spot(7, 7714.0); det(7)
    new(8, "SPX", 10.0, None, 1)                      # NaN futures poisons the EMA
    obs(8, 7714.0, 7700.0, 1 * NS, 0.0)
    obs(8, float("nan"), 7700.0, 2 * NS, 1.0)
    spot(8, 7714.0); det(8)
    new(9, "SPX", 10.0, None, 2)                      # the first spot() is ready but has no residual
    obs(9, 7714.0, 7700.0, 1 * NS, 0.0)
    obs(9, 7714.0, 7700.0, 2 * NS, 0.0)
    spot(9, 7714.0); spot(9, float("inf")); det(9)

    # --- randomized sessions: real-looking walks with drift, steps, gaps, dropped ticks ---
    rnd = random.Random(20260826)
    for s in range(400):
        i = 100 + s
        u = rnd.choice(("SPX", "NDX"))
        lvl = 7650.0 + rnd.uniform(-200, 200) if u == "SPX" else 29100.0 + rnd.uniform(-800, 800)
        hl = rnd.choice((None, 60.0, 10.0, rnd.uniform(1, 120)))
        apct = rnd.choice((None, 0.0005, rnd.uniform(1e-5, 1e-3)))
        ms = rnd.choice((15, 3, 1, 30))
        new(i, u, hl, apct, ms)
        basis = rnd.uniform(5, 20) if u == "SPX" else rnd.uniform(40, 90)
        now, ts = rnd.uniform(0, 1e6), rnd.randrange(1, 10**6) * NS
        for k in range(rnd.randrange(5, 120)):
            lvl += rnd.gauss(0, 0.5 if u == "SPX" else 2.0)
            basis += rnd.gauss(0, 0.02)
            if rnd.random() < 0.01:
                basis += rnd.choice((-1, 1)) * rnd.uniform(5, 40)       # a roll / break
            now += rnd.choice((1.0, 1.0, 1.0, 0.5, 2.0, 3.0, 30.0, rnd.uniform(0, 5)))
            ts += NS * rnd.choice((1, 1, 1, 2, 0))                         # 0 = repeated stamp
            fmid = lvl + basis + rnd.gauss(0, 0.25)
            r = rnd.random()
            if r < 0.02:
                obs(i, None, lvl, ts, now, u=u)
            elif r < 0.03:
                obs(i, fmid, None, ts, now, u=u)
            elif r < 0.04:
                obs(i, fmid, lvl, None, now, u=u)
            else:
                obs(i, fmid, lvl, ts, now, u=u)
            if rnd.random() < 0.3:
                spot(i, None if rnd.random() < 0.05 else fmid + rnd.gauss(0, 1.0))
        det(i)
    emit(out, "basis", "pricing/basis.py", L)


if __name__ == "__main__":
    main()
