#!/usr/bin/env python3
"""kp::time::from_et vs datetime(y, m, d, H, M, S, tzinfo=ET) (fold=0).timestamp(), including the
repeated November hour and the skipped March hour, where the answer is zoneinfo's rule and not
whatever mktime picks. `from_et <y> <m> <d> <H> <M> <S> <epoch_s>`"""
import argparse, datetime as dt, pathlib, random, sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
from gen_golden import emit, config  # noqa: E402

rnd = random.Random(11)
L = []
cases = []
for y in (2025, 2026, 2027):
    for mo, d0 in ((3, 1), (11, 1)):
        for d in range(d0, d0 + 14):
            for H in range(0, 5):
                for M in (0, 15, 30, 59):
                    cases.append((y, mo, d, H, M, rnd.randint(0, 59)))
for _ in range(20000):
    t = dt.date(2024, 1, 1) + dt.timedelta(days=rnd.randint(0, 1500))
    cases.append((t.year, t.month, t.day, rnd.randint(0, 23), rnd.randint(0, 59), rnd.randint(0, 59)))
for y, mo, d, H, M, S in cases:
    ts = dt.datetime(y, mo, d, H, M, S, tzinfo=config.ET).timestamp()
    L.append(f"from_et {y} {mo} {d} {H} {M} {S} {int(ts)}")
ap = argparse.ArgumentParser(); ap.add_argument("--out", default="tests/data/golden")
emit(pathlib.Path(ap.parse_args().out), "et", "config.py", L)
