#!/usr/bin/env python3
"""The pure half of strategy/run_04_quote_loop.py: verdict, _still_open, _sigma and config's clock
helpers (in_trading_session, run_stamp, occ_expiry, the discover_boards stamp)."""
import argparse, datetime as dt, pathlib, random, sys, types
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
from gen_golden import emit, h, config  # noqa: E402
from pricing import varcal  # noqa: E402
from strategy import run_04_quote_loop as r4  # noqa: E402

rnd = random.Random(404)
L = []
UTC = dt.timezone.utc


def ns(t):
    d = t - dt.datetime(1970, 1, 1, tzinfo=UTC)
    return (d.days * 86400 + d.seconds) * 10**9 + d.microseconds * 1000


# verdict: the four outcomes and every reason, on random and on boundary stats.
cases = [(0, 0, 0, 0, 10, True), (10, 0, 10, 5, 10, True), (10, 0, 10, 5, 10, False),
         (10, 10, 0, 0, 10, True), (4, 0, 0, 1, 10, True), (5, 0, 0, 1, 10, True),
         (10, 1, 0, 3, 10, True), (10, 0, 5, 3, 10, True), (10, 0, 0, 0, 10, True),
         (354, 0, 0, 7960, 360, True), (360, 36, 0, 1, 360, True), (360, 35, 0, 1, 360, True)]
for _ in range(3000):
    n = rnd.randint(0, 400)
    e = rnd.randint(0, n)
    cases.append((n, e, rnd.randint(0, max(0, n - e)), rnd.choice([0, rnd.randint(0, 9000)]),
                  rnd.choice([360.0, 30.0, 1.0, rnd.uniform(1, 900)]), rnd.random() < 0.5))
for n, e, nsp, pl, exp, ins in cases:
    v, code, d = r4.verdict({"cycles": n, "errors": e, "no_spot": nsp, "placed": pl}, exp, ins)
    L.append("verdict\t" + "\t".join(map(str, (n, e, nsp, pl, h(float(exp)), int(ins)))) + "\t"
             + "\t".join([v, str(code), str(d["expected"]), h(d["error_rate"]), h(d["spot_rate"]),
                          "|".join(d["reasons"]), f"{d['error_rate']:.0%}", f"{d['spot_rate']:.0%}"]))

# _still_open
now = dt.datetime(2026, 8, 26, 15, 30, tzinfo=config.ET)
for _ in range(2000):
    off = rnd.randint(-7200, 7200)
    t = (now + dt.timedelta(seconds=off)).astimezone(UTC)
    ct = rnd.choice([t.isoformat().replace("+00:00", "Z"), t.isoformat(), None, "", "garbage",
                     (now + dt.timedelta(seconds=off)).isoformat()])
    kept = r4._still_open([{"ticker": "T", "close_time": ct}], now)
    L.append(f"still_open\t{ns(now)}\t{'None' if ct is None else ct}\t{len(kept)}")

# _sigma on the real calendars across the day
for u in ("SPX", "NDX"):
    cal = varcal.load(u)
    for _ in range(3000):
        t = dt.datetime(2026, 8, 26, 9, 0, tzinfo=config.ET) + dt.timedelta(
            seconds=rnd.uniform(0, 8 * 3600))
        t = t.replace(microsecond=0)
        ok = rnd.random() < 0.9
        w = rnd.uniform(0.0005, 0.02)
        fit = types.SimpleNamespace(ok=ok, w=w)
        s = r4._sigma(fit, cal, t)
        L.append(f"sigma\t{u}\t{ns(t)}\t{int(ok)}\t{h(w)}\t{'None' if s is None else h(s)}")
    L.append(f"sigma_none\t{u}\t{ns(now)}\t{'None' if r4._sigma(None, cal, now) is None else 'x'}")

# clock helpers across DST edges, weekends, and the session boundaries
base = dt.datetime(2026, 1, 1, tzinfo=UTC)
insts = [dt.datetime(2026, 8, 26, 9, 29, 59, tzinfo=config.ET), dt.datetime(2026, 8, 26, 9, 30, tzinfo=config.ET),
         dt.datetime(2026, 8, 26, 15, 59, 59, tzinfo=config.ET), dt.datetime(2026, 8, 26, 16, 0, tzinfo=config.ET),
         dt.datetime(2026, 11, 1, 1, 30, tzinfo=UTC), dt.datetime(2026, 3, 8, 7, 0, tzinfo=UTC)]
insts += [base + dt.timedelta(seconds=rnd.randint(0, 3 * 365 * 86400)) for _ in range(4000)]
for t in insts:
    et = t.astimezone(config.ET)
    L.append(f"clock\t{ns(t)}\t{int(config.in_trading_session(et))}\t"
             f"{t.astimezone(UTC).strftime('%Y%m%dT%H%M%SZ')}\t{config.occ_expiry(et.date())}\t"
             + f"{et.date():%y%b%d}".upper())
ap = argparse.ArgumentParser(); ap.add_argument("--out", default="tests/data/golden")
emit(pathlib.Path(ap.parse_args().out), "run04", "strategy/run_04_quote_loop.py", L)
