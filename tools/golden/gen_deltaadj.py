#!/usr/bin/env python3
"""strategy/deltaadj.py (DeltaAdjuster + FuturesTicker) and the pure half of massive/futures.py.

deltaadj.vec -- scripts against a real FuturesTicker whose REST call (`futures._get`), clocks
(`time.time`, `time.monotonic`) and stop event are replaced from here, so ONE iteration of the
real `_poll` loop runs per `poll` line and the wait it computes is recorded. Rows cover: a good
quote, an empty page, bid/ask missing/zero/crossed/NaN, a missing timestamp (the KeyError path:
counted as an error, mid untouched), vendor errors, and the wait clamp. `mid` is read across the
staleness bound on both sides and with a future-dated stamp. The DeltaAdjuster is driven through
every refusal (no theo / no anchor / no delta / stale / no quote / over the cap) and the carry,
on SPX and NDX sizes, and its detail() and the ticker's detail() are compared as json text.
`start()` is driven through its two failure branches (front_month raises / returns None) with
the thread constructor stubbed, since neither starts one.

futures_pure.vec -- third_friday over 2020-2035, quarterly_candidates for EVERY day of
2023-2027 at n in {-1,0,1,2,3,4}, front_month against scripted volume tables (ties keep the
first, the incumbent is probed first, the MIN_FRONT_VOLUME floor), parse_ticker against a
patched date.today(), and the RTH window in ns across the DST changes.
"""
import argparse, datetime as dt, json, math, pathlib, random, sys, types
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
from gen_golden import emit, h  # noqa: E402
from massive import futures as fut  # noqa: E402
from massive.history import MassiveHistoryError  # noqa: E402
from strategy import deltaadj as da  # noqa: E402

NS = 1_000_000_000


def on(v):
    return "None" if v is None else h(v)


def J(d):
    return json.dumps(d, separators=(",", ":"))


class Clk:
    wall = 0.0
    mono = 0.0
    mono_after = None    # the monotonic reading the loop takes AFTER the request


def _mono():
    v = Clk.mono
    if Clk.mono_after is not None:
        Clk.mono, Clk.mono_after = Clk.mono_after, None
    return v


da.time = types.SimpleNamespace(time=lambda: Clk.wall, monotonic=_mono)


class OnceEvent:
    """is_set() False once, then True: exactly one pass of the real `_poll` loop."""
    def __init__(self):
        self.n = 0
        self.waited = None

    def is_set(self):
        self.n += 1
        return self.n > 1

    def wait(self, t):
        self.waited = t

    def set(self):
        pass


PAGE = {"page": None}


def fake_get(path, **params):
    p = PAGE["page"]
    if isinstance(p, Exception):
        raise p
    return p


fut._get = fake_get


def gen_deltaadj(out):
    L = []
    rnd = random.Random(8675309)
    tick = {}
    adj = {}

    def new_ticker(i, u, poll_s=None, contract="ESU6"):
        t = da.FuturesTicker(u) if poll_s is None else da.FuturesTicker(u, poll_s=poll_s)
        t.ticker = contract
        tick[i] = t
        L.append(f"ticker {i} {u} {on(poll_s)} {contract or '-'}")

    def poll(i, row, mono0, mono1):
        """row: dict | 'empty' | ('err', type, msg)"""
        t = tick[i]
        if row == "empty":
            PAGE["page"] = {"results": []}; tok = "empty"
        elif isinstance(row, tuple):
            PAGE["page"] = row[1](row[2]); tok = f"err:{row[1].__name__}"
        else:
            PAGE["page"] = {"results": [row]}
            tok = (f"row:{on(row.get('bid_price'))}:{on(row.get('ask_price'))}:"
                   f"{row['timestamp'] if 'timestamp' in row else 'None'}")
        ev = OnceEvent()
        t._stop = ev
        Clk.mono, Clk.mono_after = mono0, mono1
        t._poll()
        L.append(f"poll {i} {tok} {h(mono0)} {h(mono1)} {h(ev.waited)} {t.polls} {t.errors}"
                 f"\t{t.last_error or ''}\t{row[2] if isinstance(row, tuple) else ''}")

    def mid(i, wall, max_age=None):
        Clk.wall = wall
        m, a = tick[i].mid() if max_age is None else tick[i].mid(max_age)
        L.append(f"mid {i} {h(wall)} {on(max_age)} {on(m)} {on(a)}")

    def tdet(i, wall):
        Clk.wall = wall
        L.append(f"tdet {i} {h(wall)}\t{J(tick[i].detail())}")

    def new_adj(a, i, max_adj=None):
        t = tick[i] if i is not None else None
        adj[a] = da.DeltaAdjuster(t) if max_adj is None else da.DeltaAdjuster(t, max_adj=max_adj)
        L.append(f"adj {a} {'None' if i is None else i} {on(max_adj)}")

    def anchor(a, wall):
        Clk.wall = wall
        v = adj[a].anchor_now()
        L.append(f"anchor {a} {h(wall)} {on(v)} {on(adj[a].anchor_age_s)}")

    def adjust(a, wall, theo, delta):
        Clk.wall = wall
        f, d = adj[a].adjust(theo, delta)
        L.append(f"adjust {a} {h(wall)} {on(theo)} {on(delta)} {on(f)}\t{J(d)}")

    def adet(a, wall):
        Clk.wall = wall
        L.append(f"adet {a} {h(wall)}\t{J(adj[a].detail())}")

    T0 = 1_787_000_000.0
    ts0 = int(T0 * NS)
    # --- the reference's semantics, one branch at a time ---
    new_ticker(0, "SPX")
    mid(0, T0)                                                         # never polled
    tdet(0, T0)
    poll(0, {"bid_price": 7700.0, "ask_price": 7700.25, "timestamp": ts0}, 10.0, 10.05)
    mid(0, T0 + 0.5); mid(0, T0 + 3.0); mid(0, T0 + 3.0000001); mid(0, T0 - 0.001)
    mid(0, T0 + 5.0, 10.0); mid(0, T0 + 1.0, 0.5)
    poll(0, "empty", 11.0, 11.3)                                       # wait clamps to 0
    poll(0, {"bid_price": 0.0, "ask_price": 7700.0, "timestamp": ts0 + NS}, 12.0, 12.0)
    poll(0, {"bid_price": None, "ask_price": 7700.0, "timestamp": ts0 + NS}, 13.0, 13.1)
    poll(0, {"bid_price": 7701.0, "ask_price": 7700.0, "timestamp": ts0 + NS}, 14.0, 14.1)
    poll(0, {"bid_price": 7700.5, "ask_price": 7700.5, "timestamp": ts0 + 2 * NS}, 15.0, 15.1)
    poll(0, {"bid_price": float("nan"), "ask_price": 7700.5, "timestamp": ts0 + 3 * NS}, 16.0, 16.1)
    poll(0, {"bid_price": 7700.5, "ask_price": 7701.0}, 17.0, 17.1)    # KeyError: 'timestamp'
    poll(0, ("err", MassiveHistoryError, "HTTP 502 from /futures/v1/quotes"), 18.0, 18.2)
    poll(0, ("err", TimeoutError, "x" * 200), 19.0, 19.0)
    mid(0, T0 + 2.5)
    tdet(0, T0 + 2.5)
    tdet(0, T0 + 99.0)

    new_ticker(1, "NDX", poll_s=1.0, contract="NQU6")
    poll(1, {"bid_price": 29100.0, "ask_price": 29101.0, "timestamp": ts0}, 5.0, 5.25)

    # the adjuster through every branch
    new_adj(0, None)                            # no ticker
    anchor(0, T0); adjust(0, T0, 0.42, 0.1); adjust(0, T0, None, 0.1); adet(0, T0)
    new_ticker(2, "SPX")
    new_adj(1, 2)
    anchor(1, T0)                                # nothing polled: anchor None
    adjust(1, T0, 0.42, 0.1)                     # no_anchor
    poll(2, {"bid_price": 7700.0, "ask_price": 7700.5, "timestamp": ts0}, 1.0, 1.01)
    anchor(1, T0 + 0.2)
    adjust(1, T0 + 0.3, 0.42, None)              # no_delta
    adjust(1, T0 + 0.3, None, 0.11)              # no_theo: NOT counted
    poll(2, {"bid_price": 7701.0, "ask_price": 7701.5, "timestamp": ts0 + NS}, 1.25, 1.26)
    adjust(1, T0 + 1.2, 0.42, 0.1149)            # +1pt at the steepest ATM delta: carried
    adjust(1, T0 + 1.2, 0.42, -0.1149)
    adjust(1, T0 + 1.2, 0.42, 0.26)              # 0.26 > 0.25: refused
    adjust(1, T0 + 1.2, 0.42, 0.25)              # exactly the cap: carried (strict >)
    adjust(1, T0 + 5.0, 0.42, 0.1)               # stale: futures_stale_4.0s
    adjust(1, T0 + 0.5, 0.42, 0.1)               # future-dated: age < 0
    adet(1, T0 + 1.2)
    new_adj(2, 2, max_adj=1.0)
    anchor(2, T0 + 1.1); adjust(2, T0 + 1.1, 0.3, 0.2); adet(2, T0 + 1.1)

    # --- randomized sessions: ES and NQ, polls interleaved with anchors and carries ---
    for s in range(60):
        i = 100 + s
        u = rnd.choice(("SPX", "NDX"))
        lvl = 7700.0 if u == "SPX" else 29300.0
        tickv = 0.25 if u == "SPX" else 0.25
        new_ticker(i, u, poll_s=rnd.choice((None, 0.25, 0.1)), contract=("ESU6" if u == "SPX" else "NQU6"))
        a = 100 + s
        new_adj(a, i, max_adj=rnd.choice((None, 0.25, 0.5)))
        wall, mono, ts = T0 + rnd.uniform(0, 1e5), rnd.uniform(0, 1e4), None
        for k in range(rnd.randrange(20, 80)):
            wall += rnd.choice((0.25, 0.25, 0.3, 1.0, 4.0))
            mono += rnd.uniform(0.2, 0.3)
            r = rnd.random()
            if r < 0.6:
                lvl += tickv * rnd.choice((-2, -1, 0, 1, 2))
                ts = int((wall - rnd.uniform(0, 0.5)) * NS)
                spread = tickv * rnd.choice((1, 1, 2, 0, -1))
                row = {"bid_price": lvl, "ask_price": lvl + spread, "timestamp": ts}
                if rnd.random() < 0.03:
                    row.pop("timestamp")
                poll(i, row, mono, mono + rnd.uniform(0, 0.4))
            elif r < 0.65:
                poll(i, rnd.choice(("empty", ("err", MassiveHistoryError, "502"))), mono, mono + 0.01)
            elif r < 0.72:
                anchor(a, wall)
            else:
                delta = rnd.choice((None, rnd.uniform(-0.12, 0.12), rnd.uniform(-0.03, 0.03), rnd.uniform(-0.6, 0.6)))
                theo = rnd.choice((None, rnd.uniform(0.01, 0.99), rnd.uniform(0.3, 0.7)))
                adjust(a, wall, theo, delta)
        adet(a, wall)

    # --- start(): the two failure branches (no thread is started on either) ---
    real_fm, real_thread = fut.front_month, da.threading.Thread
    for mode in ("raise", "none"):
        def fm(product, day, _m=mode):
            if _m == "raise":
                raise MassiveHistoryError("no bars for " + product)
            return None
        fut.front_month = fm
        t = da.FuturesTicker("NDX")
        t.start(day=dt.date(2026, 8, 21))
        L.append(f"start NDX {mode}\t{J(t.detail())}")
    fut.front_month, da.threading.Thread = real_fm, real_thread
    emit(out, "deltaadj", "strategy/deltaadj.py", L)


def gen_futures(out):
    L = []
    for y in range(2020, 2036):
        for m in range(1, 13):
            L.append(f"tf {y} {m} {fut.third_friday(y, m).isoformat()}")
    d = dt.date(2023, 1, 1)
    while d <= dt.date(2027, 12, 31):
        for n in (-1, 0, 1, 2, 3, 4):
            for prod in ("ES",):
                c = fut.quarterly_candidates(prod, d, n=n)
                L.append(f"qc {prod} {d.isoformat()} {n} {len(c)} "
                         + " ".join(f"{tk}@{e.isoformat()}" for tk, e in c))
        d += dt.timedelta(days=1)
    for u in ("SPX", "NDX", "RUT", ""):
        L.append(f"product {u or '-'} {fut.PRODUCT.get(u, u) or '-'} {fut.PRODUCT.get(u) or 'None'} "
                 f"{fut.CACHE_TICKER.get(u) or 'None'}")
    for r in ("1sec", "1min", "1second", "1minute", "1s", "1m", "second", "minute", "S1", "60", ""):
        L.append(f"res {r or '-'} {1 if r in fut.RESOLUTIONS else 0}")

    # front_month against scripted volumes
    rnd = random.Random(31337)
    real_sv = fut.session_volume
    table = {}
    fut.session_volume = lambda tk, day: table.get((tk, day.isoformat()), 0)
    days = [dt.date(2024, 9, 12), dt.date(2024, 9, 16), dt.date(2024, 9, 20),
            dt.date(2026, 3, 27), dt.date(2026, 6, 19), dt.date(2026, 12, 31), dt.date(2025, 1, 2)]
    days += [dt.date(2024, 1, 1) + dt.timedelta(days=rnd.randrange(0, 1400)) for _ in range(300)]
    for d in days:
        cands = [tk for tk, _ in fut.quarterly_candidates("ES", d, n=4)] + ["ESZ9", "ESH0"]
        table.clear()
        vols = []
        for tk in cands:
            v = rnd.choice((0, 1, 999, 1000, 1001, rnd.randint(0, 2_000_000), 552300, 365541,
                            1_233_410, 86_557))
            if rnd.random() < 0.1 and vols:
                v = vols[-1][1]                    # a tie: the earlier candidate must win
            table[(tk, d.isoformat())] = v
            vols.append((tk, v))
        for n in (1, 2, 3):
            for inc in (None, "", cands[0], "ESZ9", "ESH0"):
                fm = fut.front_month("ES", d, n_candidates=n, incumbent=inc)
                L.append(f"fm ES {d.isoformat()} {n} {'None' if inc is None else (inc or '-')} "
                         f"{len(vols)} " + " ".join(f"{tk}={v}" for tk, v in vols)
                         + f" {fm or 'None'}")
    fut.session_volume = real_sv

    # parse_ticker against a patched date.today()
    real_dt = fut.datetime
    for tday in (dt.date(2026, 8, 23), dt.date(2024, 1, 1), dt.date(2029, 12, 31),
                  dt.date(2030, 1, 1), dt.date(2035, 6, 1)):
        class FakeDate(dt.date):
            @classmethod
            def today(cls, _t=tday):
                return _t
        fut.datetime = types.SimpleNamespace(date=FakeDate, time=dt.time,
                                             timedelta=dt.timedelta, datetime=dt.datetime)
        for tk in ["ESU4", "ESZ5", "NQH6", "ESM9", "ESU0", "ESU1", "ESU5", "ESU6", "ESU7",
                   "ESH5-ESM5", "ESF6", "esU4", "ESU", "ESU44", "E1U4", "ESU4\n", "ESU4 ", "",
                   "ABZ9", "ESX4"]:
            p = fut.parse_ticker(tk)
            L.append(f"pt {tday.year} {json.dumps(tk)} "
                     + ("None" if p is None else f"{p[0]} {p[1]} {p[2]}"))
    fut.datetime = real_dt

    d = dt.date(2024, 1, 1)
    while d <= dt.date(2027, 12, 31):
        for hh, mm in ((9, 30), (16, 0), (17, 0), (18, 0), (0, 0), (12, 34)):
            L.append(f"etns {d.isoformat()} {hh} {mm} {fut._et_ns(d, dt.time(hh, mm))}")
        d += dt.timedelta(days=3)
    emit(out, "futures_pure", "massive/futures.py", L)


def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--out", default="tests/data/golden")
    out = pathlib.Path(ap.parse_args().out)
    gen_deltaadj(out)
    gen_futures(out)


if __name__ == "__main__":
    main()
