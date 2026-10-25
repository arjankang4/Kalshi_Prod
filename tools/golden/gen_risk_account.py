#!/usr/bin/env python3
"""strategy/risklimit.SessionRisk.mark_account / opening_equity / the stale-mark halt / summary.

Sessions of marks against random account snapshots, books, fairs and missing snapshots (the stale
path), each on its own temp RISK_STATE_DIR so opening equity is written by the first mark and then
READ BACK by a second SessionRisk -- the restart case the persistence exists for. The wall clock is
pinned (it names the day's file). One JSON line per step:
    step <json: inputs and every output>
"""
import argparse, datetime as dt, json, pathlib, random, sys, tempfile, types
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
from gen_golden import emit, config  # noqa: E402
from strategy import risklimit  # noqa: E402

NOW = dt.datetime(2026, 8, 26, 14, 30, 5, 123456, tzinfo=dt.timezone.utc)


class _DT(dt.datetime):
    @classmethod
    def now(cls, tz=None):
        return NOW.astimezone(tz) if tz else NOW.replace(tzinfo=None)


risklimit.datetime = types.SimpleNamespace(datetime=_DT, timezone=dt.timezone, date=dt.date,
                                           timedelta=dt.timedelta)
rnd = random.Random(923)
L = []


def rsum(r):
    s = r.summary()
    return s


for sess in range(400):
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="riskacct-"))
    config.RISK_STATE_DIR = tmp
    limit = rnd.choice([None, 250.0, 50.0, 5.0])
    r = risklimit.SessionRisk(limit_dollars=limit, halt_path=tmp / "NOHALT")
    tickers = [f"T{i}" for i in range(rnd.randint(1, 8))]
    mono = 1000.0
    bal = round(rnd.uniform(500, 5000), 4)
    restarted = False
    for step in range(rnd.randint(2, 25)):
        mono += rnd.choice([0.5, 1.0, 2.0, 12.0, 31.0])
        if rnd.random() < 0.15:
            snap = None
        else:
            pos = {t: rnd.choice([0.0, 20.0, -20.0, 5.0, -100.0, 40.0]) for t in tickers
                   if rnd.random() < 0.7}
            bal = round(bal + rnd.uniform(-30, 30), 4)
            snap = (mono - rnd.choice([0.0, 0.2, 1.0, 45.0]), bal, pos)
        books = {}
        for t in tickers:
            if rnd.random() < 0.8:
                bb = rnd.choice([None, 0.0, round(rnd.uniform(0.01, 0.98), 2)])
                ba = rnd.choice([None, 1.0, round(rnd.uniform(0.02, 0.99), 2)])
                books[t] = (bb, ba)
        fairs = {t: rnd.choice([None, rnd.random()]) for t in tickers if rnd.random() < 0.6}
        if rnd.random() < 0.1:
            fairs = {}
        if not restarted and step > 3 and rnd.random() < 0.2:
            # a restart: a fresh SessionRisk must READ the persisted opening equity
            r = risklimit.SessionRisk(limit_dollars=limit, halt_path=tmp / "NOHALT")
            restarted = True
        pnl = r.mark_account(snap, books, fairs, now_mono=mono)
        why = r.halt_reason()
        rec = {"sess": sess, "limit": limit, "mono": mono,
               "snap": None if snap is None else [snap[0], snap[1], snap[2]],
               "books": {t: list(v) for t, v in books.items()}, "fairs": fairs,
               "restart": restarted, "pnl": pnl, "halt": why, "stale_s": getattr(r, "stale_s", None),
               "summary": rsum(r)}
        L.append("step\t" + json.dumps(rec))
        if restarted:
            restarted = "done"
    # the file the first mark wrote
    for f in tmp.glob("opening_equity_*.json"):
        L.append("file\t" + json.dumps({"sess": sess, "name": f.name, "text": f.read_text()}))
ap = argparse.ArgumentParser(); ap.add_argument("--out", default="tests/data/golden")
emit(pathlib.Path(ap.parse_args().out), "risk_account", "strategy/risklimit.py", L)
