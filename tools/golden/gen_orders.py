#!/usr/bin/env python3
"""strategy/orders.py, in full -- the ladder policy, the book, the risk gate, and `apply`.

INPUTS AND OUTPUTS ARE JSON (hex-encoded, so a line stays whitespace-split). An order row is a dict
in this module and the C++ carries it as `Order`; comparing `json.dumps` of the reference's dicts
against the port's `to_json()` pins every key, every float's repr and every key's ORDER in one go.

    pysum      CPython 3.12 sum() over floats -- Neumaier-compensated, not a fold. Every contract
               and dollar total in this module is one.
    ov/pid/zh  order_value, _pair_id, zombie_held.
    plan       plan_side over random and decide_ladder-built ladders, every reason string.
    recon      reconcile: several markets, desired + stale sides, per-ticker and global counts.
    risk       within_risk_limits, near every cap (violations compared SORTED: the reference
               iterates a Python set, whose order is hash-seeded).
    fill       apply_fill sequences: partial, full, over-fill, unknown id, no ticker, bad count.
    apply      `apply` against a SCRIPTED fake client (`O.k` swapped, as test_orders does): partial
               failures, reduced_by 0, not_found, 429 / 500 / transport errors, short and long
               result lists, a write bucket on a fake clock, dry and live. Every request the fake
               receives is recorded (cancel ids; create bodies as the json.dumps bytes) and
               replayed in C++ against a fake kp::kalshi::Api that checks the request and returns
               the identical response -- so the call sequence itself (never a second send after a
               429) is part of the vector.
    cy_*       run_04's cycle, end to end over many cycles on a persistent book: fills, the
               theo/touch stamp, reconcile, budget.admit, the risk gate with reducing_only, apply.
"""
import argparse, json, math, pathlib, random, sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
from gen_golden import emit, h, captures  # noqa: E402
import config  # noqa: E402
from kalshi import client as k  # noqa: E402
from strategy import budget as B  # noqa: E402
from strategy import orders as O  # noqa: E402
from strategy import quoting as Q  # noqa: E402

rnd = random.Random(424242)


def sx(s):
    return s.encode("utf8").hex() or "-"


def jx(v):
    return sx(json.dumps(v))


def on(v):
    return "None" if v is None else h(v)


# --- fakes ----------------------------------------------------------------------------------------

class FakeTime:
    def __init__(self, t=1000.0, step=0.01):
        self.t, self.step = t, step

    def monotonic(self):
        v = self.t
        self.t += self.step
        return v


class Ids:
    def __init__(self, prefix):
        self.prefix, self.n = prefix, 0

    def uuid4(self):
        self.n += 1
        return f"{self.prefix}-{self.n}"


class ScriptK:
    """A client whose every answer is drawn at call time and RECORDED, for the C++ to replay."""

    order_body = staticmethod(k.order_body)
    fp = staticmethod(k.fp)

    def __init__(self, bucket=None, p429=0.08, p500=0.04, pexc=0.04):
        self.bucket = bucket
        self.calls = []
        self.p429, self.p500, self.pexc = p429, p500, pexc
        self.n_new = 0

    def write_budget(self):
        raise AssertionError("apply must be given max_batch")

    def _fail(self, rec, cost):
        u = rnd.random()
        if u < self.p429:
            body = rnd.choice(['{"error":{"code":"too_many_requests"}}', "rate limited"])
            rec["resp"] = {"kalshi": [429, body]}
            if self.bucket is not None:
                self.bucket.penalise(cost)
            raise k.KalshiError(429, body)
        u -= self.p429
        if u < self.p500:
            status = rnd.choice([500, 400, 503])
            body = rnd.choice(["boom", '{"error":{"code":"too_many_orders_in_batch"}}',
                               "é" * 300 + "x" * 200])
            rec["resp"] = {"kalshi": [status, body]}
            raise k.KalshiError(status, body)
        u -= self.p500
        if u < self.pexc:
            name, msg = rnd.choice([("ConnectionResetError", "peer reset the connection"),
                                    ("Exception", "中" * 90),
                                    ("TimeoutError", "timed out")])
            rec["resp"] = {"exc": [name, msg]}
            raise {"ConnectionResetError": ConnectionResetError, "Exception": Exception,
                   "TimeoutError": TimeoutError}[name](msg)

    def _shape(self, rows):
        u = rnd.random()
        if u < 0.05 and rows:
            return rows[:-1]
        if u < 0.08:
            return rows + [{"order_id": "extra", "reduced_by": 1}]
        if u < 0.10:
            return []
        return rows

    def batch_cancel_orders(self, ids):
        rec = {"kind": "cancel", "req": list(ids)}
        self.calls.append(rec)
        cost = k.CANCEL_TOKEN_COST * len(ids)
        if self.bucket is not None:
            self.bucket.debit(cost)
        self._fail(rec, cost)
        rows = []
        for oid in ids:
            c = rnd.choices(["ok", "zero", "gone", "err", "other", "str", "noid", "errzero",
                             "emptyerr"], [60, 8, 8, 5, 3, 5, 5, 3, 3])[0]
            if c == "ok":
                rows.append({"order_id": oid, "reduced_by": rnd.choice([10, 20, 5])})
            elif c == "zero":
                rows.append({"order_id": oid, "reduced_by": 0})
            elif c == "gone":
                rows.append({"order_id": oid, "error": {"code": "not_found", "message": "gone"},
                             "reduced_by": 0})
            elif c == "err":
                rows.append({"order_id": oid, "error": {"code": "internal", "message": "x"},
                             "reduced_by": 5})
            elif c == "other":
                rows.append({"order_id": "someone-else", "reduced_by": 5})
            elif c == "str":
                rows.append({"order_id": oid, "reduced_by": rnd.choice(["20.00", "0.00", "0"])})
            elif c == "noid":
                rows.append({"reduced_by": 10.0})
            elif c == "errzero":
                rows.append({"error": "string error", "reduced_by": 0})
            else:
                rows.append({"order_id": oid, "error": {}, "reduced_by": 3})
        rows = self._shape(rows)
        rec["resp"] = {"ok": rows}
        return rows

    def batch_create_orders(self, bodies):
        rec = {"kind": "create", "req": json.dumps(bodies)}
        self.calls.append(rec)
        cost = k.ORDER_TOKEN_COST * len(bodies)
        if self.bucket is not None:
            self.bucket.debit(cost)
        self._fail(rec, cost)
        rows = []
        for _b in bodies:
            c = rnd.choices(["ok", "err", "noid", "nullerr"], [80, 10, 5, 5])[0]
            self.n_new += 1
            if c == "ok":
                rows.append({"order_id": f"new-{self.n_new}", "status": "resting"})
            elif c == "err":
                rows.append({"error": {"code": "post_only_cross", "message": "would cross"}})
            elif c == "noid":
                rows.append({})
            else:
                rows.append({"order_id": f"new-{self.n_new}", "error": None})
        rows = self._shape(rows)
        rec["resp"] = {"ok": rows}
        return rows


class NoK:
    """A client that must never be reached -- the dry path."""
    order_body = staticmethod(k.order_body)
    fp = staticmethod(k.fp)

    def __getattr__(self, name):
        raise AssertionError(f"dry path touched the client: {name}")


# --- book <-> JSON -------------------------------------------------------------------------------

STATE_FIELDS = ("theo", "best_bid", "best_ask", "zombie_bid", "zombie_ask", "zombie_bid_touch",
                "zombie_ask_touch", "fill_bid", "fill_ask", "position")


def dump_book(book):
    out = []
    for t, st in book._m.items():
        d = {"ticker": t, "bid": st.bid, "ask": st.ask}
        for f in STATE_FIELDS:
            d[f] = getattr(st, f)
        out.append(d)
    return out


def load_book(js):
    b = O.RestingBook()
    for d in js:
        st = b.state(d["ticker"])
        st.bid = [dict(o) for o in d["bid"]]
        st.ask = [dict(o) for o in d["ask"]]
        for f in STATE_FIELDS:
            setattr(st, f, d[f])
    return b


def row(t, side, price, count, oid=None, coid=None):
    d = {"ticker": t, "side": side, "price": price, "count": count}
    if oid is not None:
        d["order_id"] = oid
    if coid is not None:
        d["client_order_id"] = coid
    return d


COUNTS = [("20.00", "20.00", "20.00"), ("10.00", "20.00", "50.00"), ("20.00",), ("7.50", "12.25"),
          ("20.00", "20.00"), ("1.00", "2.00", "3.00")]
_oid = [0]


def new_oid():
    _oid[0] += 1
    return f"o{_oid[0]}"


def tick(x):
    return round(round(x / 0.01) * 0.01, 4)


def rand_price(center, spread=0.06):
    p = tick(center + rnd.uniform(-spread, spread))
    if rnd.random() < 0.05:
        p = round(p + 0.005, 4)                   # an off-grid price
    return min(0.99, max(0.01, p))


def rand_book(n_mkts=None, max_per_side=4, state_noise=True):
    b = O.RestingBook()
    for i in range(n_mkts if n_mkts is not None else rnd.randint(0, 6)):
        t = f"M{i}"
        st = b.state(t)
        center = rnd.uniform(0.05, 0.95)
        st.theo = None if rnd.random() < 0.05 else round(center, 4)
        cn = rnd.choice(COUNTS)
        for side in ("bid", "ask"):
            rows = []
            for j in range(rnd.randint(0, max_per_side)):
                p = rand_price(center - 0.02 if side == "bid" else center + 0.02)
                rows.append(row(t, side, p, cn[min(j, len(cn) - 1)],
                                oid=(None if rnd.random() < 0.05 else new_oid())))
            st.set_side(side, rows)
        if state_noise:
            st.set_touch(rnd.choice([None, tick(center - 0.01)]),
                         rnd.choice([None, tick(center + 0.01)]))
            if rnd.random() < 0.3:
                st.zombie_bid, st.zombie_bid_touch = 990.0, st.best_bid
            if rnd.random() < 0.3:
                st.zombie_ask, st.zombie_ask_touch = 980.0, st.best_ask
            if rnd.random() < 0.2:
                st.fill_bid = 995.5
            st.position = rnd.choice([0.0, 20.0, -40.0])
    return b


def rand_ladder(side, theo, cn):
    """A decided ladder: usually decide_ladder's own, sometimes a hand grid."""
    if theo is not None and rnd.random() < 0.7:
        bb = rnd.choice([None, tick(theo - rnd.uniform(0, 0.05))])
        ba = rnd.choice([None, tick(theo + rnd.uniform(0, 0.05))])
        if bb is not None and ba is not None and bb >= ba:
            ba = round(bb + 0.01, 4)
        rungs = Q.decide_ladder(side, theo, bb, ba, cn,
                                min_edge_micros=rnd.choice([None, 5000, 10000, 23000]),
                                scale_pos=rnd.choice([0.0, 0.25, 0.5, 1.0]), tiers_enabled=True)
        return list(rungs)
    n = rnd.randint(0, 3)
    base = theo if theo is not None else 0.5
    step = rnd.choice([0.01, 0.02, 0.005])
    out = []
    for i in range(n):
        p = round(base - 0.01 - i * step, 4) if side == "bid" else round(base + 0.01 + i * step, 4)
        out.append(Q.Rung(i, p, cn[min(i, len(cn) - 1)], 0.01, "t"))
    return out


def rungs_js(rungs):
    return [[r.price, r.count] for r in rungs]


def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--out", default="tests/data/golden")
    out = pathlib.Path(ap.parse_args().out)
    L = []

    # --- pysum -------------------------------------------------------------------------------
    for _ in range(20000):
        n = rnd.randint(0, 12)
        kind = rnd.random()
        if kind < 0.3:
            xs = [float(rnd.choice(["20.00", "10.00", "7.50", "50.00", "12.25"])) for _ in range(n)]
        elif kind < 0.6:
            xs = [rnd.uniform(0, 1) * rnd.choice([20.0, 10.0, 7.5]) for _ in range(n)]
        elif kind < 0.8:
            xs = [rnd.uniform(-1, 1) * 10 ** rnd.randint(-20, 20) for _ in range(n)]
        else:
            xs = [rnd.choice([1e16, 1.0, -1e16, 0.1, 0.2, 0.3, -0.0, 1e-300]) for _ in range(n)]
        L.append(f"pysum {jx(xs)} {h(sum(xs))}")
    L.append(f"pysum {jx([1e308, 1e308, -1e308])} {h(sum([1e308, 1e308, -1e308]))}")

    # --- order_value / pair_id / zombie_held ---------------------------------------------------
    for _ in range(5000):
        side = rnd.choice(["bid", "ask", "x"])
        price = rnd.choice([None, rnd.uniform(0, 1), tick(rnd.uniform(0, 1))])
        count = rnd.choice([None, "20.00", "7.50", "0.00", "1.25"])
        L.append(f"ov {side} {on(price)} {sx(count) if count is not None else 'None'} "
                 f"{h(O.order_value(side, price, count))}")
    for _ in range(5000):
        t = rnd.choice(["T", "KXINX-26AUG21H1600-B7687", "M3"])
        side = rnd.choice(["bid", "ask"])
        p = rnd.choice([tick(rnd.uniform(0, 1)), rnd.uniform(0, 1), round(rnd.uniform(0, 1), 5)])
        L.append(f"pid {t} {side} {h(p)} {sx(O._pair_id(t, side, p))}")
    for _ in range(20000):
        side = rnd.choice(["bid", "ask"])
        touch = rnd.choice([None, tick(rnd.uniform(0.1, 0.9))])
        zt = rnd.choice([None, touch, None if touch is None else round(touch + rnd.choice(
            [-0.01, 0.01, 1e-10, -1e-10, 1e-9, -1e-9, 2e-9]), 12)])
        za = rnd.choice([None, 1000.0])
        now = rnd.choice([None, 1000.0 + rnd.choice([0.0, 1.0, 29.999, 30.0, 30.001, 100.0,
                                                     rnd.uniform(0, 40), -1.0])])
        hold = rnd.choice([None, None, 0.0, -1.0, 10.0, rnd.uniform(0, 60)])
        L.append(f"zh {side} {on(touch)} {on(za)} {on(zt)} {on(now)} {on(hold)} "
                 f"{int(O.zombie_held(side, touch, za, zt, now, hold_s=hold))}")

    # --- plan_side ---------------------------------------------------------------------------
    for _ in range(12000):
        t = "M"
        side = rnd.choice(["bid", "ask"])
        theo = None if rnd.random() < 0.05 else round(rnd.uniform(0.02, 0.98), rnd.choice([2, 3, 4]))
        cn = rnd.choice(COUNTS)
        rungs = rand_ladder(side, theo, cn)
        base = theo if theo is not None else 0.5
        live = []
        for j in range(rnd.randint(0, 4)):
            p = rand_price(base - 0.02 if side == "bid" else base + 0.02, 0.04)
            live.append(row(t, side, p, rnd.choice(cn + ("5.00",)), oid=new_oid()))
        touch = rnd.choice([None, tick(base)])
        za = rnd.choice([None, None, 1000.0])
        zt = rnd.choice([None, touch, tick(base - 0.01)])
        now = rnd.choice([None, 1001.0, 1030.5])
        ft, bt = rnd.choice([(1, 2), (1, 2), (1, 2), (0, 0), (2, 3), (1, 1)])
        inp = {"ticker": t, "side": side, "theo": theo, "rungs": rungs_js(rungs), "live": live,
               "counts": list(cn), "front": ft, "back": bt, "touch": touch, "zat": za, "zt": zt,
               "now": now}
        try:
            c, p, kp_ = O.plan_side(t, side, theo, rungs, live, cn, front_ticks=ft, back_ticks=bt,
                                    touch=touch, zombie_at=za, zombie_touch=zt, now=now)
            L.append(f"plan {jx(inp)} {jx([c, p, kp_])}")
        except TypeError:
            L.append(f"plan {jx(inp)} ERR")

    # --- reconcile -----------------------------------------------------------------------------
    for _ in range(1500):
        b = rand_book(rnd.randint(0, 5))
        tks = list(b._m) + [f"N{i}" for i in range(rnd.randint(0, 2))]
        desired = []
        for tk_ in tks:
            for side in ("bid", "ask"):
                if rnd.random() < 0.6:
                    theo = b.state(tk_).theo if tk_ in b._m else None
                    if theo is None and rnd.random() < 0.7:
                        continue            # an unpriced market is never desired
                    cn = rnd.choice(COUNTS)
                    desired.append([tk_, side, rand_ladder(side, theo, cn)])
        # The book is dumped BEFORE the N* markets are touched, so reconcile's own state creation
        # is part of what is compared.
        book_js = dump_book(b)
        mode = rnd.choice(["none", "tuple", "map"])
        counts_js = None
        if mode == "tuple":
            counts_js = list(rnd.choice(COUNTS))
            counts = tuple(counts_js)
        elif mode == "map":
            counts_js = {tk_: list(rnd.choice(COUNTS)) for tk_ in tks if rnd.random() < 0.7}
            counts = {kk: tuple(v) for kk, v in counts_js.items()}
        else:
            counts = None
        now = rnd.choice([1000.5, 1025.0, 1100.0])
        des = {(tk_, side): r for tk_, side, r in desired}
        inp = {"book": book_js, "desired": [[tk_, s, rungs_js(r)] for (tk_, s), r in des.items()],
               "mode": mode, "counts": counts_js, "now": now}
        try:
            pl, cn_, kept = O.reconcile(b, des, counts=counts, now=now)
            L.append(f"recon {jx(inp)} {jx([pl, cn_, kept, dump_book(b)])}")
        except TypeError:
            L.append(f"recon {jx(inp)} ERR")

    # --- within_risk_limits --------------------------------------------------------------------
    for i_risk in range(1500):
        b = rand_book(rnd.randint(0, 8), max_per_side=rnd.choice([3, 6]), state_noise=False)
        big = rnd.random() < 0.1
        place, cancel = [], []
        for _j in range(rnd.randint(0, 12) if not big else rnd.randint(100, 300)):
            t = rnd.choice(list(b._m) + ["NEW1", "NEW2"]) if b._m else "NEW1"
            side = rnd.choice(["bid", "ask"])
            pr = None if rnd.random() < 0.05 else rand_price(rnd.uniform(0.05, 0.95))
            d = {"ticker": t, "side": side, "count": rnd.choice(["20.00", "50.00", "10.00", "7.50"])}
            if pr is not None:
                d = {"ticker": t, "side": side, "price": pr, "count": d["count"]}
            place.append(d)
        for t, st in b._m.items():
            for side in ("bid", "ask"):
                for o in st.side(side):
                    if rnd.random() < 0.3:
                        cancel.append({**o, "reason": rnd.choice(["edge_negative", "improve_1t"])})
        if big:
            for t in list(b._m):
                b.state(t).bid.append(row(t, "bid", 0.95, "900.00", oid=new_oid()))
        ok, viol = O.within_risk_limits(b, place, cancel)
        L.append(f"risk {jx({'book': dump_book(b), 'place': place, 'cancel': cancel})} "
                 f"{int(ok)} {jx(sorted(viol))}")
        if i_risk % 3 == 0:
            L.append(f"reduce {jx(cancel)} {jx(O.reducing_only(cancel))}")
    # The reference's own review cluster, at the real caps: a full side requoting one rung.
    b = O.RestingBook()
    b.state("T").bid = [row("T", "bid", p, "20.00", oid=f"o{i}")
                        for i, p in enumerate((0.41, 0.40, 0.39))]
    c6, p6, _ = O.plan_side("T", "bid", 0.42, [Q.Rung(i, p, "20.00", 0.01, "t") for i, p in
                                               enumerate((0.42, 0.41, 0.40))], b.state("T").bid,
                            ("20.00",) * 3)
    for cc in ([], c6):
        ok, viol = O.within_risk_limits(b, p6, cc)
        L.append(f"risk {jx({'book': dump_book(b), 'place': p6, 'cancel': cc})} {int(ok)} "
                 f"{jx(sorted(viol))}")

    # --- apply_fill ---------------------------------------------------------------------------
    for _ in range(1000):
        b = rand_book(rnd.randint(1, 5))
        L.append(f"fbook {jx(dump_book(b))}")
        ids = [o.get("order_id") for o in b.all_orders() if o.get("order_id")]
        for _j in range(rnd.randint(1, 6)):
            oid = rnd.choice(ids + ["nope", None, "", 7]) if ids else rnd.choice(["nope", None])
            f = {}
            if oid is not None or rnd.random() < 0.5:
                f["order_id"] = oid
            cf = rnd.choice(["20.00", "5.00", "999.00", "0.00", "10", 7.5, 0, None, "abc", "-1",
                             "19.999999999", "20.000000001", [1]])
            if cf is not None or rnd.random() < 0.5:
                f["count_fp"] = cf
            mt = rnd.choice([None, "", "M0", "M1", "M9", "ZZ"])
            if mt is not None or rnd.random() < 0.5:
                f["market_ticker"] = mt
            now = rnd.choice([100.0, 200.5])
            r = O.apply_fill(b, f, now=now)
            L.append(f"fill {jx(f)} {h(now)} {r} {jx(dump_book(b))}")

    # --- apply ----------------------------------------------------------------------------------
    real_k, real_uuid, real_time = O.k, O.uuid, k.time
    try:
        for sc in range(2000):
            b = rand_book(rnd.randint(0, 5))
            book_js = dump_book(b)
            to_cancel, to_place = [], []
            for t, st in b._m.items():
                for side in ("bid", "ask"):
                    for o in st.side(side):
                        u = rnd.random()
                        if u < 0.25:
                            to_cancel.append({**o, "reason": rnd.choice(
                                ["edge_negative", "no_longer_quoting"])})
                        elif u < 0.5:
                            pid = O._pair_id(t, side, o["price"])
                            to_cancel.append({**o, "reason": "improve_1t", "pair": pid})
                            if rnd.random() < 0.9:
                                np_ = round(o["price"] + (0.01 if side == "bid" else -0.01), 4)
                                to_place.append({"ticker": t, "side": side, "price": np_,
                                                 "count": o["count"], "reason": "improved",
                                                 "pair": pid})
            for _j in range(rnd.randint(0, 8)):
                t = rnd.choice(list(b._m) + ["NEWM"]) if b._m else "NEWM"
                side = rnd.choice(["bid", "ask"])
                to_place.append({"ticker": t, "side": side,
                                 "price": rand_price(rnd.uniform(0.05, 0.95)),
                                 "count": rnd.choice(["20.00", "10.00", "7.50"]),
                                 "reason": rnd.choice(["top_up", "cold_start"])})
            if rnd.random() < 0.05:
                to_place.append({"ticker": "Z", "side": "bid", "price": 0.3, "count": "1.00",
                                 "reason": "improved", "pair": "Z|bid|0.2900"})   # orphan
            rnd.shuffle(to_place)
            live = rnd.random() < 0.8
            mb = rnd.choice([1, 2, 3, 5, 30])
            now = rnd.choice([1000.0, 2000.5])
            budget_js = None
            bucket = None
            ft = FakeTime(rnd.uniform(0, 1e4), rnd.choice([0.0, 0.01, 0.1]))
            k.time = ft
            if rnd.random() < 0.5:
                budget_js = {"cap": rnd.choice([0.0, 20.0, 60.0, 900.0, rnd.uniform(0, 200)]),
                             "rate": rnd.choice([0.0, 300.0, 50.0]), "t0": ft.t, "step": ft.step}
                bucket = k.TokenBucket(budget_js["cap"], budget_js["rate"])
            ids = Ids(f"cid{sc}")
            O.uuid = ids
            fake = ScriptK(bucket, p429=rnd.choice([0.0, 0.1, 0.3]), p500=rnd.choice([0.0, 0.1]),
                           pexc=rnd.choice([0.0, 0.1]))
            O.k = fake if live else NoK()
            inp = {"book": book_js, "place": to_place, "cancel": to_cancel, "live": live,
                   "max_batch": mb, "now": now, "budget": budget_js, "ids": f"cid{sc}"}
            res = O.apply(to_place, to_cancel, b, live=live, max_batch=mb, budget=bucket, now=now)
            L.append(f"apply {jx(inp)} {jx(fake.calls)} {jx(res)} {jx(dump_book(b))} "
                     f"{jx(None if bucket is None else bucket.detail())}")
    finally:
        O.k, O.uuid, k.time = real_k, real_uuid, real_time

    # --- run_04's cycle, end to end --------------------------------------------------------------
    gen_cycles(L)
    emit(out, "orders", "strategy/orders.py", L)


def gen_cycles(L):
    """Persistent sessions: every step's inputs and outputs, in run_04.cycle's order."""
    real_k, real_uuid, real_time = O.k, O.uuid, k.time
    try:
        for sid in range(100):
            n = rnd.randint(3, 14)
            tks = [f"KXINXU-S{sid}-T{7600 + 5 * i}" for i in range(n)]
            fair = {t: rnd.uniform(0.08, 0.92) for t in tks}
            live = rnd.random() < 0.7
            mb = rnd.choice([2, 5, 30])
            ft = FakeTime(rnd.uniform(0, 1e4), 0.001)
            k.time = ft
            t0 = ft.t
            bucket = k.TokenBucket(rnd.choice([900.0, 200.0, 60.0]), rnd.choice([300.0, 50.0]))
            ids = Ids(f"s{sid}")
            O.uuid = ids
            book = O.RestingBook()
            fake = ScriptK(bucket, p429=0.05, p500=0.03, pexc=0.02)
            O.k = fake if live else NoK()
            counts_map = {t: (("20.00", "20.00", "20.00") if rnd.random() < 0.5 else ("20.00",))
                          for t in tks}
            L.append(f"cy_new {sid} {int(live)} {mb} {h(t0)} {h(ft.step)} {h(bucket.capacity)} "
                     f"{h(bucket.refill_rate)} s{sid} {jx({t: list(v) for t, v in counts_map.items()})}")
            mono = 5000.0
            for cyc in range(rnd.randint(8, 25)):
                mono += 2.0
                fake.calls = []
                # FILLS FIRST
                resting = [o for o in book.all_orders() if o.get("order_id")]
                for _ in range(rnd.randint(0, 3)):
                    if not resting:
                        break
                    o = rnd.choice(resting)
                    f = {"order_id": o["order_id"], "market_ticker": o["ticker"],
                         "count_fp": rnd.choice(["20.00", "5.00", "10.00"])}
                    r = O.apply_fill(book, f, now=mono)
                    L.append(f"cy_fill {sid} {jx(f)} {h(mono)} {r}")
                # the theo walk and the touch
                stamps = []
                for t in tks:
                    fair[t] = min(0.97, max(0.03, fair[t] + rnd.gauss(0, 0.012)))
                    theo = None if rnd.random() < 0.03 else round(fair[t], 6)
                    bb = rnd.choice([None, tick(fair[t] - rnd.uniform(0.005, 0.04))])
                    ba = rnd.choice([None, tick(fair[t] + rnd.uniform(0.005, 0.04))])
                    if bb is not None and ba is not None and bb >= ba:
                        ba = round(bb + 0.01, 4)
                    stamps.append([t, theo, bb, ba])
                chosen = [s for s in stamps if rnd.random() < 0.8]
                desired = {}
                for t, theo, bb, ba in chosen:
                    if theo is None:
                        continue
                    for side in ("bid", "ask"):
                        rungs = Q.decide_ladder(side, theo, bb, ba, counts_map[t],
                                                min_edge_micros=rnd.choice([5000, 8000, 15000]),
                                                scale_pos=rnd.choice([0.0, 0.5]),
                                                tiers_enabled=True)
                        if rungs:
                            desired[(t, side)] = rungs
                for t, theo, bb, ba in stamps:
                    if theo is not None:
                        book.state(t).theo = theo
                    book.state(t).set_touch(bb, ba)
                L.append(f"cy_stamp {sid} {jx(stamps)}")
                place, cancel, kept = O.reconcile(book, desired, counts=counts_map, now=mono)
                L.append(f"cy_recon {sid} "
                         f"{jx([[t, s, rungs_js(r)] for (t, s), r in desired.items()])} "
                         f"{h(mono)} {jx([place, cancel, kept])}")
                theos = {t: book.state(t).theo for t in book.tickers()}
                em_map = {t: {"risk_u": rnd.random(), "R": rnd.random() * 0.1, "quotable": True}
                          for t in tks if rnd.random() < 0.9}
                tokens = bucket.available()
                place, cancel, bdet = B.admit(place, cancel, tokens, theos=theos, em_map=em_map,
                                              resting=book.resting_sides())
                L.append(f"cy_admit {sid} {h(tokens)} {jx(theos)} {jx(em_map)} "
                         f"{jx([place, cancel, bdet])}")
                ok, viol = O.within_risk_limits(book, place, cancel)
                if not ok:
                    cancel = O.reducing_only(cancel)
                    place = []
                L.append(f"cy_risk {sid} {int(ok)} {jx(sorted(viol))}")
                res = O.apply(place, cancel, book, live=live, max_batch=mb, budget=bucket,
                              now=mono)
                L.append(f"cy_apply {sid} {h(mono)} {jx(fake.calls if live else [])} {jx(res)} "
                         f"{jx(dump_book(book))} {jx(bucket.detail())}")
    finally:
        O.k, O.uuid, k.time = real_k, real_uuid, real_time


if __name__ == "__main__":
    main()
