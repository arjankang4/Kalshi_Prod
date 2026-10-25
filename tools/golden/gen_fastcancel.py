#!/usr/bin/env python3
"""strategy/fastcancel.py -- reprice_shifted on the real pricers, and FastGuard's decision logic.

    rp        reprice_shifted with a REAL theo.Pricer (the captured boards, parity and given spots)
              and a REAL hourly.HourlyPricer (SPX calendar, three times of day), over the captured
              /markets rows and moves from 0 through a 20-point crash. The {ticker: fair} map and
              the restored forwards are compared bit for bit.
    g_*       FastGuard driven one `_tick` at a time -- never through its thread or a sleep:
                g_new     a guard over fakes (a linear FakePricer, a scriptable futures mid, a
                          tracker that can refuse) or over a REAL daily pricer
                g_pub     publish / retire
                g_tick    one step: the futures mid and any refusal are set first, then _tick;
                          compared: cancels, repriced, the orders pulled (on_cancel), the book,
                          and -- live -- every request the scripted exchange received.
"""
import argparse, datetime as dt, json, pathlib, random, sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
from gen_golden import emit, h, captures, config, REF, _real_boards  # noqa: E402
from kalshi import client as k  # noqa: E402
from pricing import hourly as hr  # noqa: E402
from pricing import theo as th  # noqa: E402
from pricing import varcal as vc  # noqa: E402
from strategy import fastcancel as fc  # noqa: E402
from strategy import orders as O  # noqa: E402

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from gen_orders import ScriptK, dump_book, jx, sx, on, rnd as orders_rnd  # noqa: E402

rnd = random.Random(777)


def ns(t):
    return int(t.timestamp() * 1_000_000_000)


def mkt_js(m):
    return {"ticker": m["ticker"], "strike_type": m.get("strike_type"),
            "floor_strike": m.get("floor_strike"), "cap_strike": m.get("cap_strike"),
            "close_time": m.get("close_time")}


class FakePricer:
    def __init__(self, spot, slope=0.01, base=0.50, dead=False, boom=False):
        self.spot, self.slope, self.base, self._origin = spot, slope, base, spot
        self.dead, self.boom = dead, boom

    def market_fair(self, market):
        if self.boom:
            raise RuntimeError("smile blew up")
        if self.dead:
            return None, {"reason": "no_vol"}
        return self.base + self.slope * (self.spot - self._origin), {}


class FakeHourly:
    """The hourly pricer's seam: its forward lives on `fit.forward`."""

    def __init__(self, fwd, slope):
        self.fit = type("Fit", (), {})()
        self.fit.forward = fwd
        self.slope, self._origin = slope, fwd

    def market_fair(self, market):
        return 0.50 + self.slope * (self.fit.forward - self._origin), {}


class FakeFutures:
    def __init__(self):
        self.mid_ = {}

    def mid(self, u, max_age_s=None):
        return self.mid_.get(u), 0.01


class FakeTracker:
    def __init__(self):
        self.refuse = None

    def spot(self, mid):
        if self.refuse or mid is None:
            return None, {"reason": self.refuse or "no_futures"}
        return mid, {"reason": None}


def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--out", default="tests/data/golden")
    out = pathlib.Path(ap.parse_args().out)
    L = [f"ref {sx(str(REF))}"]

    boards = _real_boards()
    rows = []
    for f in captures("*/markets_iter*.json"):
        ms = json.loads(f.read_text())
        rows += ms if isinstance(ms, list) else ms.get("markets", [])
    markets = [mkt_js(m) for m in rows]
    L.append(f"markets {jx(markets)}")
    cal = vc.load("SPX")
    day = dt.date(2026, 8, 21)
    nows = [dt.datetime.combine(day, dt.time(hh, mm), tzinfo=config.ET)
            for hh, mm in ((10, 0), (13, 30), (15, 30))]

    # --- reprice_shifted on the real pricers ---------------------------------------------------
    moves = [0.0, 0.05, -0.05, 1.0, -1.0, 5.15, -5.15, 20.0, -20.0]
    for bi, (board, strikes) in enumerate(boards):
        p0 = th.Pricer(board, strikes)
        for spot in (None, p0.spot, (p0.spot or 7700.0) + 3.0):
            for now in nows:
                daily = th.Pricer(board, strikes, spot=spot)
                hourly = hr.HourlyPricer(board, strikes, now, cal)
                for mv in moves + [rnd.uniform(-10, 10) for _ in range(4)]:
                    idx = sorted(rnd.sample(range(len(markets)), min(len(markets), 40)))
                    items = [(rows[i], daily if rnd.random() < 0.6 else hourly) for i in idx]
                    use_d = rnd.random() < 0.9
                    use_h = rnd.random() < 0.9
                    bd = daily.spot if use_d else None
                    bh = hourly.fit.forward if use_h and hourly.fit else None
                    fairs = fc.reprice_shifted(daily, hourly, bd, bh, mv, items)
                    spec = [[i, 0 if p is daily else 1] for i, (_, p) in zip(idx, items)]
                    L.append(f"rp {bi} {on(spot)} {ns(now)} {int(use_d)} {int(use_h)} {h(mv)} "
                             f"{jx(spec)} {jx(fairs)} {on(daily.spot)} {on(hourly.fit.forward)}")

    # --- reprice_shifted WITH A SMILE INSTALLED (reference fix 2026-09-23) -------------------------
    # The curve carries its own forward and SkewFit.p_above_at reads it, so a re-price that moves only
    # Pricer.spot / fit.forward is a no-op whenever a non-flat curve is on -- the live case. Three
    # installs: one curve SHARED by both pricers (the live shape), daily-only, and a lazily built one
    # (the `_skew` the reference reads, never the `.skew` property). After each move a zero-move
    # re-price must give the pre-move fairs back: forwards AND the projection memo were restored.
    from pricing import skew as skewmod
    for bi, (board, strikes) in enumerate(boards):
        F = th.Pricer(board, strikes).spot
        w = skewmod.atm_vol(board, strikes, F)
        sf = skewmod.fit(board, strikes, F, w)
        L.append(f"rps_curve {bi} {h(F)} {on(w)} {int(sf.flat)}")
        for now in nows:
            for mode in ("shared", "daily_only", "lazy"):
                daily = th.Pricer(board, strikes, spot=F)
                hourly = hr.HourlyPricer(board, strikes, now, cal)
                if mode == "shared":
                    daily.use_skew(sf)
                    hourly.use_skew(sf)
                elif mode == "daily_only":
                    daily.use_skew(sf)
                else:
                    _ = daily.skew
                for mv in moves:
                    idx = sorted(rnd.sample(range(len(markets)), min(len(markets), 30)))
                    items = [(rows[i], daily if rnd.random() < 0.5 else hourly) for i in idx]
                    bd, bh = daily.spot, (hourly.fit.forward if hourly.fit else None)
                    before = fc.reprice_shifted(daily, hourly, bd, bh, 0.0, items)
                    fairs = fc.reprice_shifted(daily, hourly, bd, bh, mv, items)
                    after = fc.reprice_shifted(daily, hourly, bd, bh, 0.0, items)
                    spec = [[i, 0 if p is daily else 1] for i, (_, p) in zip(idx, items)]
                    L.append(f"rps {bi} {ns(now)} {mode} {h(mv)} {jx(spec)} {jx(before)} {jx(fairs)} "
                             f"{jx(after)} {on(daily.spot)} {on(hourly.fit.forward)} {on(sf.forward)}")

    # --- FastGuard over fakes, one tick at a time -------------------------------------------------
    real_k, real_uuid = O.k, O.uuid
    try:
        for gid in range(1500):
            live = rnd.random() < 0.5
            mb = rnd.choice([1, 2, 30])
            fut = FakeFutures()
            trk = {"SPX": FakeTracker(), "NDX": FakeTracker()}
            if rnd.random() < 0.1:
                trk["NDX"] = None
            book = O.RestingBook()
            tks = [f"G{gid}-{u}-{i}" for u in ("SPX", "NDX") for i in range(rnd.randint(1, 4))]
            n_oid = [0]
            for t in tks:
                st = book.state(t)
                for side in ("bid", "ask"):
                    rows_ = []
                    for j in range(rnd.randint(0, 3)):
                        n_oid[0] += 1
                        px = round(0.50 + (-1 if side == "bid" else 1) * rnd.choice(
                            [0.0, 0.005, 0.01, 0.02, 0.03, 0.05]), 4)
                        oid = None if rnd.random() < 0.05 else f"g{gid}-{n_oid[0]}"
                        rows_.append({"ticker": t, "side": side, "price": px, "count": "20.00",
                                      "order_id": oid})
                    st.set_side(side, rows_)
            fake = ScriptK(None, p429=0.1, p500=0.05, pexc=0.05)
            fake.write_budget = lambda: {"max_batch": mb}
            # Dry or live, the guard's apply passes no max_batch, so the REFERENCE reads
            # k.write_budget() on every cancel burst -- the fake answers it.
            O.k = fake
            pulled = []
            g = fc.FastGuard(fut, trk, book, live=live,
                             min_move=rnd.choice([None, 0.05, 1.0]),
                             on_cancel=lambda cs, res: pulled.append(cs))
            L.append(f"g_new {gid} {int(live)} {mb} {on(g.min_move)} "
                     f"{int(trk['NDX'] is None)} {jx(dump_book(book))}")
            pricers = {}
            for u in ("SPX", "NDX"):
                if rnd.random() < 0.9:
                    base = 7700.0 if u == "SPX" else 29300.0
                    slope = rnd.choice([0.01, -0.01, 0.002, 0.03])
                    mode = rnd.choices(["ok", "dead", "boom"], [90, 5, 5])[0]
                    dpr = FakePricer(base, slope=slope, dead=(mode == "dead"), boom=(mode == "boom"))
                    hpr = FakeHourly(base - 1.5, slope * 2) if rnd.random() < 0.5 else None
                    dm = [{"ticker": t} for t in tks if f"-{u}-" in t and rnd.random() < 0.7]
                    hm = [{"ticker": t} for t in tks if f"-{u}-" in t and rnd.random() < 0.5]
                    sp = rnd.choice([base, base, None])
                    g.publish(u, dpr, hpr, dm, hm, sp)
                    pricers[u] = (dpr, hpr)
                    L.append(f"g_pub {gid} {u} {h(base)} {h(slope)} {mode} "
                             f"{'-' if hpr is None else h(hpr.fit.forward)} {jx([m['ticker'] for m in dm])} "
                             f"{jx([m['ticker'] for m in hm])} {on(sp)}")
            for step in range(rnd.randint(1, 6)):
                if rnd.random() < 0.1 and g.boards:
                    u = rnd.choice(sorted(g.boards))
                    g.retire(u)
                    L.append(f"g_retire {gid} {u}")
                for u in ("SPX", "NDX"):
                    base = 7700.0 if u == "SPX" else 29300.0
                    fut.mid_[u] = rnd.choice([None, base, base + rnd.choice(
                        [0.01, -0.03, 0.05, -0.05, 1.0, -2.0, 2.0, 5.15, -5.15, 0.049])])
                    if trk[u] is not None:
                        trk[u].refuse = rnd.choice([None, None, None, "basis_break_+20.00pts"])
                pulled.clear()
                fake.calls = []
                err = "-"
                try:
                    g._tick()
                except RuntimeError as e:
                    err = sx(str(e))
                st = {u: [fut.mid_.get(u), None if trk[u] is None else trk[u].refuse]
                      for u in ("SPX", "NDX")}
                restored = {u: [p[0].spot, None if p[1] is None else p[1].fit.forward]
                            for u, p in pricers.items()}
                L.append(f"g_tick {gid} {jx(st)} {err} {g.cancels} {g.repriced} "
                         f"{jx(pulled)} {jx(fake.calls if live else [])} {jx(dump_book(book))} "
                         f"{jx(restored)}")
    finally:
        O.k, O.uuid = real_k, real_uuid

    # --- FastGuard over a REAL daily pricer ------------------------------------------------------
    board, strikes = boards[0]
    daily = th.Pricer(board, strikes)
    priced = []
    for m in rows:
        f_, _d = daily.market_fair(m)
        if f_ is not None and 0.03 < f_ < 0.97:
            priced.append((m, f_))
    for gid in range(300):
        book = O.RestingBook()
        sub = rnd.sample(priced, min(len(priced), 12))
        n = 0
        for m, f_ in sub:
            st = book.state(m["ticker"])
            for side in ("bid", "ask"):
                n += 1
                px = round(round((f_ + (-1 if side == "bid" else 1) * rnd.uniform(0.0, 0.04)) / 0.01)
                           * 0.01, 4)
                st.set_side(side, [{"ticker": m["ticker"], "side": side, "price": px,
                                    "count": "20.00", "order_id": f"r{gid}-{n}"}])
        fut = FakeFutures()
        trk = {"SPX": FakeTracker()}
        g = fc.FastGuard(fut, trk, book, live=False)
        g.publish("SPX", daily, None, [s[0] for s in sub], [], daily.spot)
        L.append(f"gr_new {gid} {jx([s[0]['ticker'] for s in sub])} {jx(dump_book(book))}")
        for step in range(3):
            fut.mid_["SPX"] = daily.spot + rnd.choice([0.0, 0.5, -0.5, 2.0, -2.0, 5.15, -5.15,
                                                        rnd.uniform(-8, 8)])
            g._tick()
            L.append(f"gr_tick {gid} {h(fut.mid_['SPX'])} {g.cancels} {g.repriced} "
                     f"{jx(dump_book(book))} {on(daily.spot)}")
    emit(out, "fastcancel", "strategy/fastcancel.py", L)


if __name__ == "__main__":
    main()
