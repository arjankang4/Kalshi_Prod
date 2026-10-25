#!/usr/bin/env python3
"""pricing/lean.py backoff_cents / backoff_micros -- the decaying entry-bar tax after a fill.

Swept on both legs and every boundary: None and negative ages, exactly 0, the half-life knee (5s,
where the exponential hands over to the linear leg), the end (10s), and the binary64 neighbours of
each, plus overridden legs (start 0 takes the `start > 0` guard; mid > start grows instead of
decays). The micros go through Python's half-to-even round() on the UNROUNDED cents, and the
exponential leg is a `**` -- a pow -- so thousands of random ages are needed to pin it.
"""
import argparse, math, pathlib, random, sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
from gen_golden import emit, h  # noqa: E402
from pricing import lean  # noqa: E402


def on(v):
    return "None" if v is None else h(v)


def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--out", default="tests/data/golden")
    out = pathlib.Path(ap.parse_args().out)
    rnd = random.Random(5150)
    L = []
    ages = [None, -1.0, -1e-12, 0.0, -0.0, 5.0, 10.0, 2.5, 7.5, 1e-300, 1e9]
    for knee in (0.0, 5.0, 10.0):
        x = knee
        for _ in range(3):
            x = math.nextafter(x, -1)
            ages.append(x)
        x = knee
        for _ in range(3):
            x = math.nextafter(x, 100)
            ages.append(x)
    for i in range(0, 12001):
        ages.append(i / 1000.0)                  # a millisecond grid, 0..12 s
    for _ in range(30000):
        ages.append(rnd.uniform(-1, 12))
    for a in ages:
        L.append(f"bo {on(a)} None None None None {h(lean.backoff_cents(a))} "
                 f"{lean.backoff_micros(a)}")
    for _ in range(20000):
        a = rnd.choice([None, rnd.uniform(-1, 30), float(rnd.randint(0, 20))])
        st = rnd.choice([None, 0.0, 1.0, 2.0, rnd.uniform(0.1, 5)])
        md = rnd.choice([None, 0.5, 0.25, rnd.uniform(0.01, 3)])
        hf = rnd.choice([None, 5.0, 2.0, rnd.uniform(0.5, 8)])
        en = rnd.choice([None, 10.0, 20.0, rnd.uniform(8.5, 25)])
        kw = {k_: v for k_, v in (("start", st), ("mid", md), ("half", hf), ("end", en))
              if v is not None}
        try:
            c = lean.backoff_cents(a, **kw)
            m = lean.backoff_micros(a, **kw)
        except ZeroDivisionError:
            continue
        L.append(f"bo {on(a)} {on(st)} {on(md)} {on(hf)} {on(en)} {h(c)} {m}")
    emit(out, "lean_backoff", "pricing/lean.py", L)


if __name__ == "__main__":
    main()
