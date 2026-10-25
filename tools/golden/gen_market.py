#!/usr/bin/env python3
"""Vectors for theo::parse_iso_ns / market_from_json: the close_time parse edgemgr.settlement_of does.

    iso <string> <epoch_ns | None>       (TAB-separated; the string may contain spaces)
"""
import argparse, datetime as dt, pathlib, random, sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
from gen_golden import emit  # noqa: E402
from strategy import edgemgr  # noqa: E402

rnd = random.Random(3)
S = ["2026-08-26T20:00:00Z", "2026-08-26T20:00:00+00:00", "2026-08-26T16:00:00-04:00",
     "2026-08-26T20:00:00.123456Z", "2026-08-26T20:00:00.5Z", "2026-08-26T20:00:00.1234567Z",
     "2026-08-26 20:00:00Z", "2026-08-26T20:00Z", "2026-08-26T20Z", "2026-08-26",
     "2026-08-26T20:00:00", "garbage", "", "2026-13-01T00:00:00Z", "2026-08-26T24:00:00Z",
     "2026-08-26T20:00:00+05:30", "1969-12-31T23:59:59.5Z", "2026-02-30T00:00:00Z"]
for _ in range(3000):
    t = dt.datetime(2020, 1, 1, tzinfo=dt.timezone.utc) + dt.timedelta(seconds=rnd.randint(0, 10**9))
    S.append(t.isoformat().replace("+00:00", "Z"))
L = []
for s in S:
    v = edgemgr.settlement_of({"close_time": s})
    if v is None:
        out = "None"
    else:
        d = v - dt.datetime(1970, 1, 1, tzinfo=dt.timezone.utc)
        out = str((d.days * 86400 + d.seconds) * 10**9 + d.microseconds * 1000)
    L.append(f"iso\t{s}\t{out}")
ap = argparse.ArgumentParser(); ap.add_argument("--out", default="tests/data/golden")
emit(pathlib.Path(ap.parse_args().out), "market", "strategy/edgemgr.py", L)
