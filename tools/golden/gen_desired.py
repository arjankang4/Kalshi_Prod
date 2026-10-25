#!/usr/bin/env python3
"""run_04.build_desired and quoting.top_of_book, driven by the real Python.

One case per line, TAB-separated:  desired <input json> <desired json> <notes json>
The input carries everything build_desired reads (markets, fairs, books, edge records, lean rows,
band shapes, positions, last-fill ages, mono_now, tiers) so the C++ rebuilds the identical call.
Books come in every shape `ladders` accepts; edge records cover quotable, refused and partial
(missing floor/bonus) records; positions straddle the inventory limit on both sides.
"""
import argparse, json, pathlib, random, sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
from gen_golden import emit, captures  # noqa: E402
from strategy import orders as omgr, quoting  # noqa: E402
from strategy import run_04_quote_loop as r4  # noqa: E402

rnd = random.Random(2604)
rows = []
for f in captures("*/markets_iter*.json"):
    try:
        r = json.loads(f.read_text())
    except Exception:
        continue
    rows.extend(r.get("markets", []) if isinstance(r, dict) else r)
rows = [m for m in rows if m.get("ticker")][:400]


def price():
    return round(rnd.uniform(0.01, 0.99), 2)


def levels():
    n = rnd.choice([0, 0, 1, 2, 5])
    ps = sorted({round(rnd.uniform(0.01, 0.99), 2) for _ in range(n)})
    return [[f"{p:.4f}" if rnd.random() < 0.7 else f"{p:.2f}", f"{rnd.randint(1, 900)}.00"] for p in ps]


def book_entry():
    y, n = levels(), levels()
    shape = rnd.randint(0, 4)
    if shape == 0:
        return {"orderbook_fp": {"yes_dollars": y, "no_dollars": n}}
    if shape == 1:
        return {"yes": y, "no": n}
    if shape == 2:
        return {"orderbook": {"yes": y, "no": n}}
    if shape == 3:
        return {}
    return {"orderbook_fp": {}, "yes_dollars": y, "no_dollars": n}


def edge_rec():
    k = rnd.random()
    if k < 0.15:
        return {"edge_micros": 10000, "edge": 0.01, "floor_micros": 10000, "bonus_micros": 0,
                "risk_u": 0.0, "scale_pos": 0.0, "R": None, "delta": None, "vega": None,
                "theta": None, "quotable": False, "source": "no_R", "reason": rnd.choice(["no_R", "no_settlement_ts"])}
    if k < 0.2:
        return {"edge_micros": None, "edge": None, "floor_micros": None, "bonus_micros": None,
                "risk_u": 1.0, "R": rnd.random(), "scale_pos": 1.0, "quotable": False,
                "source": "curve", "reason": "adverse_selection"}
    bonus = rnd.choice([0, rnd.randint(0, 50000)])
    rec = {"edge_micros": 5000 + bonus, "edge": (5000 + bonus) / 1e6, "floor_micros": 5000,
           "bonus_micros": bonus, "risk_u": rnd.random(), "scale_pos": rnd.choice([0.0, rnd.random()]),
           "R": rnd.random() * 0.1, "delta": rnd.random(), "vega": rnd.random(), "theta": -rnd.random(),
           "quotable": True, "source": "curve", "reason": None}
    if rnd.random() < 0.1:
        del rec["floor_micros"]
    if rnd.random() < 0.1:
        rec["bonus_micros"] = None
    return rec


class EM:
    def __init__(self, recs):
        self.recs = recs

    def get(self, t):
        return self.recs.get(t) or {"edge_micros": 10000, "edge": 0.01, "floor_micros": 10000,
                                    "bonus_micros": 0, "risk_u": 0.0, "scale_pos": 0.0, "R": None,
                                    "delta": None, "vega": None, "theta": None, "quotable": False,
                                    "source": "no_R", "reason": "unknown_ticker"}


L = []
for case in range(1500):
    ms = rnd.sample(rows, rnd.randint(1, 12))
    ms = [{k: m.get(k) for k in ("ticker", "event_ticker", "strike_type", "floor_strike",
                                 "cap_strike", "close_time")} for m in ms]
    fairs = {}
    for m in ms:
        if rnd.random() < 0.9:
            if rnd.random() < 0.1:
                fairs[m["ticker"]] = {"fair": None, "reason": rnd.choice(["unpriceable", None])}
            else:
                d = {"fair": rnd.choice([price(), rnd.uniform(0.001, 0.999)])}
                if rnd.random() < 0.4:
                    d.update({"fair_vol": rnd.random(), "delta_adj": rnd.uniform(-0.01, 0.01),
                              "delta_adj_reason": rnd.choice([None, "cap", "no_delta"]),
                              "f_move": rnd.uniform(-3, 3)})
                fairs[m["ticker"]] = d
    books = {m["ticker"]: book_entry() for m in ms if rnd.random() < 0.9}
    recs = {m["ticker"]: edge_rec() for m in ms if rnd.random() < 0.95}
    lean = {}
    for m in ms:
        if rnd.random() < 0.6:
            lean[m["ticker"]] = {"p_ref": rnd.choice([None, rnd.random()]),
                                 "p_rv": rnd.choice([None, rnd.random()])}
    bands = None
    if rnd.random() < 0.9:
        bands = {}
        for m in ms:
            if rnd.random() < 0.9:
                inb = rnd.random() < 0.4
                bd = {"inband": inb, "counts": ("20.00", "20.00", "20.00") if inb else ("20.00",),
                      "position_limit": 100.0 if inb else 20.0}
                if rnd.random() < 0.85:
                    bd.update({"reason": None, "distance": rnd.random(), "half_width": rnd.random(),
                               "limit": rnd.random(), "sigmas": rnd.random() * 4, "band_frac": 0.0,
                               "incumbent": False, "_detail": True})
                else:
                    bd.update({"reason": "no_width", "distance": None, "half_width": None})
                bands[m["ticker"]] = bd
    positions = {m["ticker"]: rnd.choice([0, 0.0, -0.0, 20.0, -20.0, 19.0, -100.0, 100.0, 5.0])
                 for m in ms if rnd.random() < 0.5}
    mono = 1000.0 + rnd.random() * 100
    fills = {}
    for m in ms:
        for side in ("bid", "ask"):
            if rnd.random() < 0.2:
                fills.setdefault(m["ticker"], {})[side] = mono - rnd.choice([0.0, 2.5, 5.0, 7.3, 10.0, 30.0])
    tiers = rnd.random() < 0.8
    # the hourly cutoff's clock: 2026-08-21 between 10:00 and 16:05 ET, so each hourly event is
    # seen far from, just inside and just past its HOURLY_MIN_MINUTES_TO_CLOSE window
    import datetime as _dt
    import config as _cfg
    now = (_dt.datetime(2026, 8, 21, 10, 0, tzinfo=_cfg.ET)
           + _dt.timedelta(seconds=rnd.randint(0, 6 * 3600 + 300), microseconds=rnd.randint(0, 999999))
           ).astimezone(_dt.timezone.utc)
    now_ns = ((now - _dt.datetime(1970, 1, 1, tzinfo=_dt.timezone.utc)) // _dt.timedelta(microseconds=1)) * 1000
    book = omgr.RestingBook()
    for tk, sd in fills.items():
        for side, t in sd.items():
            book.state(tk).mark_fill(side, t)
    desired, notes = r4.build_desired(ms, fairs, books, EM(recs), lean, tiers,
                                      {k: {kk: vv for kk, vv in v.items() if kk != "_detail"}
                                       for k, v in bands.items()} if bands is not None else None,
                                      positions=positions, book=book, mono_now=mono, now=now)
    inp = {"markets": ms, "fairs": fairs, "books": books, "em": recs, "lean": lean,
           "bands": bands, "positions": positions, "fills": fills, "mono": mono, "tiers": tiers,
           "now_ns": now_ns}
    des = [[tk, side, [[r.index, r.price, r.count, r.edge, r.reason, r.min_edge] for r in rungs]]
           for (tk, side), rungs in desired.items()]
    L.append("\t".join(["desired", json.dumps(inp), json.dumps(des), json.dumps(notes)]))

# top_of_book on its own, every shape
for _ in range(3000):
    e = rnd.choice([None, {}, book_entry(), book_entry(), book_entry()])
    L.append("\t".join(["tob", json.dumps(e), json.dumps(list(quoting.top_of_book(e)))]))
ap = argparse.ArgumentParser(); ap.add_argument("--out", default="tests/data/golden")
emit(pathlib.Path(ap.parse_args().out), "desired", "strategy/run_04_quote_loop.py", L)
