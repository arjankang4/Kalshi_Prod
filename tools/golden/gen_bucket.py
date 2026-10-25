#!/usr/bin/env python3
"""kalshi/client.py, the PURE half -- TokenBucket, fp, _max_batch, order_body, write_budget's dict.

Nothing here opens a socket: the client module is imported for its functions and its clock is
replaced (`client.time`) so the bucket runs on a scripted monotonic clock.

    fp        f"{x:.{places}f}" -- every order price goes through it, so the half-way cases (a price
              exactly between two 4dp values, and the binary64 neighbours of each) are swept, plus
              places 0..6 over random magnitudes.
    maxb      _max_batch over None / 0 / negative / fractional / the measured 900-300 tier.
    floordiv  the float `//` it rests on.
    body      order_body, compared as the json.dumps BYTES the batched endpoint receives.
    bucket    TokenBucket scripts on a fake clock: refill, clamp, debt, resize, penalise, detail.
    limits    write_budget()'s dict built from a GET /account/limits response.
    pyfloat   Python float() on the strings the order path reads (count_fp, reduced_by).
"""
import argparse, json, math, pathlib, random, sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
from gen_golden import emit, h  # noqa: E402
from kalshi import client as k  # noqa: E402


def sx(s):
    return s.encode("utf8").hex() or "-"


def on(v):
    return "None" if v is None else h(v)


class FakeTime:
    def __init__(self):
        self.t = 0.0
        self.step = 0.0

    def monotonic(self):
        v = self.t
        self.t += self.step
        return v


def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--out", default="tests/data/golden")
    out = pathlib.Path(ap.parse_args().out)
    rnd = random.Random(20260923)
    L = []

    # --- fp ------------------------------------------------------------------------------
    xs = [0.0, -0.0, 0.01, 0.99, 0.5, 1.0, 0.12345, 0.00005, 0.00015, 0.99995, 1e-9, -0.01,
          123456.789, 1e22, 5e-324, float("inf"), float("-inf"), float("nan")]
    for i in range(100):
        xs.append(i / 100)
    for i in range(0, 100000, 7):
        x = (i + 0.5) / 10000                      # exact decimal halves at the 4th place
        xs += [x, math.nextafter(x, 0), math.nextafter(x, 2)]
    for _ in range(20000):
        xs.append(rnd.uniform(-2, 2))
    for _ in range(3000):
        xs.append(rnd.uniform(-1, 1) * 10 ** rnd.randint(-8, 12))
    for x in xs:
        places = 4 if rnd.random() < 0.6 else rnd.randint(0, 6)
        L.append(f"fp {h(x)} {places} {sx(k.fp(x, places))}")
        if places != 4:
            L.append(f"fp {h(x)} 4 {sx(k.fp(x))}")

    # --- _max_batch / floordiv ----------------------------------------------------------------
    figs = [None, 0, 0.0, 900.0, 300.0, 100.0, 200.0, 9.0, 10.0, 19.99, -5.0, 1e9, 0.5, 29.999]
    for c in figs:
        for r in figs:
            L.append(f"maxb {on(c)} {on(r)} {k._max_batch(c, r)}")
    for _ in range(3000):
        c = rnd.choice([None, 0.0, rnd.uniform(-100, 2000), float(rnd.randint(0, 2000))])
        r = rnd.choice([None, 0.0, rnd.uniform(-100, 2000), float(rnd.randint(0, 2000))])
        L.append(f"maxb {on(c)} {on(r)} {k._max_batch(c, r)}")
    for _ in range(5000):
        a = rnd.uniform(-1e4, 1e4) if rnd.random() < 0.7 else float(rnd.randint(-100, 100))
        b = rnd.choice([10.0, -10.0, 3.0, 0.1, rnd.uniform(-50, 50) or 1.0])
        L.append(f"floordiv {h(a)} {h(b)} {h(a // b)}")

    # --- order_body -------------------------------------------------------------------------
    tickers = ["KXINX-26AUG21H1600-B7687", "KXINXU-26AUG21H1500-T7675", "T", "Xé中"]
    for _ in range(3000):
        t = rnd.choice(tickers)
        side = rnd.choice(["bid", "ask"])
        price = k.fp(rnd.randint(1, 99) / 100 + rnd.choice([0, 0, 0.005, 1e-12]))
        count = rnd.choice(["20.00", "10.00", "7.50", "1.00"])
        coid = rnd.choice([None, "", "cid-1", "3f1c0b7e-6a1e-4d0f-9b3a-2c1d7e8f9a0b"])
        tif = rnd.choice([None, "good_till_canceled", "immediate_or_cancel"])
        po = rnd.choice([None, True, False])
        stp = rnd.choice([None, "maker", "taker_at_cross"])
        kw = {}
        if tif is not None:
            kw["time_in_force"] = tif
        if po is not None:
            kw["post_only"] = po
        if stp is not None:
            kw["self_trade_prevention_type"] = stp
        b = k.order_body(t, side, price, count, client_order_id=coid, **kw)
        L.append(f"body {sx(t)} {side} {sx(price)} {sx(count)} {sx(coid or '')} "
                 f"{sx(tif or '')} {'-' if po is None else int(po)} {sx(stp or '')} "
                 f"{sx(json.dumps(b))} {sx(json.dumps({'orders': [b]}))}")

    # --- TokenBucket on a scripted clock -----------------------------------------------------
    real_time = k.time
    ft = FakeTime()
    k.time = ft
    try:
        for sid in range(400):
            cap = rnd.choice([900.0, 100.0, 0.0, 30.0, rnd.uniform(0, 1000)])
            rate = rnd.choice([300.0, 0.0, 100.0, rnd.uniform(0, 500)])
            ft.t = rnd.uniform(0, 1e5)
            ft.step = rnd.choice([0.0, 0.001, 0.01, 0.05, 1.0, rnd.uniform(0, 0.2)])
            L.append(f"bnew {sid} {h(cap)} {h(rate)} {h(ft.t)} {h(ft.step)}")
            b = k.TokenBucket(cap, rate)
            for _ in range(rnd.randint(5, 40)):
                op = rnd.choice(["avail", "afford", "debit", "debit", "resize", "penalise",
                                 "detail", "jump"])
                if op == "avail":
                    L.append(f"bop {sid} avail {h(b.available())}")
                elif op == "afford":
                    tok = rnd.choice([2, 10, 20, 300, rnd.uniform(0, 1000)])
                    L.append(f"bop {sid} afford {h(float(tok))} {int(b.can_afford(tok))}")
                elif op == "debit":
                    tok = float(rnd.choice([2, 10, 60, 300, rnd.randint(1, 900)]))
                    L.append(f"bop {sid} debit {h(tok)} {h(b.debit(tok))}")
                elif op == "penalise":
                    tok = float(rnd.choice([10, 300]))
                    L.append(f"bop {sid} penalise {h(tok)} {h(b.penalise(tok))}")
                elif op == "resize":
                    c2 = rnd.choice([None, 900.0, 100.0, rnd.uniform(1, 1000)])
                    r2 = rnd.choice([None, 300.0, 50.0])
                    b.resize(capacity=c2, refill_rate=r2)
                    L.append(f"bop {sid} resize {on(c2)} {on(r2)} {h(b.capacity)} "
                             f"{h(b.refill_rate)}")
                elif op == "detail":
                    L.append(f"bop {sid} detail {sx(json.dumps(b.detail()))}")
                else:
                    dt_ = rnd.choice([0.0, 0.5, 3.0, -1.0, 100.0])
                    ft.t += dt_
                    L.append(f"bop {sid} jump {h(dt_)}")
            L.append(f"bop {sid} end {h(b.spent)} {b.denied}")
    finally:
        k.time = real_time

    # --- write_budget's dict ------------------------------------------------------------------
    real_req = k._request
    lims = [{"usage_tier": "advanced", "write": {"bucket_capacity": 900, "refill_rate": 300}},
            {"usage_tier": "basic", "write": {"bucket_capacity": 100.0, "refill_rate": 100.0}},
            {"usage_tier": None, "write": {}}, {}, {"write": None},
            {"usage_tier": "x", "write": {"bucket_capacity": "900", "refill_rate": "300.5"}},
            {"write": {"bucket_capacity": 0, "refill_rate": 0}},
            {"write": {"bucket_capacity": 900}}, {"write": {"refill_rate": 250}},
            {"usage_tier": {"a": 1}, "write": {"bucket_capacity": 45.5, "refill_rate": 7}},
            {"write": {"bucket_capacity": True, "refill_rate": 300}},
            {"write": {"bucket_capacity": "abc", "refill_rate": 300}},
            {"write": {"bucket_capacity": [1], "refill_rate": 300}}]
    for _ in range(200):
        lims.append({"usage_tier": rnd.choice(["advanced", "basic", "premier"]),
                     "write": {"bucket_capacity": rnd.choice([0, 900, rnd.uniform(0, 2000)]),
                               "refill_rate": rnd.choice([0, 300, rnd.randint(1, 1000)])}})
    try:
        for lim in lims:
            k._request = lambda *a, _l=lim, **kw: _l
            r = k.write_budget()
            if "error" in r:
                L.append(f"limits {sx(json.dumps(lim))} ERR")
            else:
                L.append(f"limits {sx(json.dumps(lim))} {sx(json.dumps(r))}")
    finally:
        k._request = real_req

    # --- Python float() on the strings the order path reads -----------------------------------
    strs = ["20.00", " 20.00 ", "1_000.5", "1__0", "_1", "1_", "0x10", "inf", "-Infinity", "nan",
            "NaN", "+.5", "5.", ".", "", "  ", "1e3", "1e", "1e+", "e3", "abc", "10.00\n", "-0",
            "1.5e-3", "1,000", "+-1", "0.0", "12345678901234567890", "1e400", "-1e-400"]
    for _ in range(2000):
        strs.append(f"{rnd.uniform(0, 1000):.{rnd.randint(0, 6)}f}")
    for s in strs:
        try:
            L.append(f"pyfloat {sx(s)} {h(float(s))}")
        except ValueError:
            L.append(f"pyfloat {sx(s)} ERR")

    emit(out, "bucket", "kalshi/client.py", L)


if __name__ == "__main__":
    main()
