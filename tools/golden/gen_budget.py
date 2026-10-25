#!/usr/bin/env python3
"""strategy/budget.py, in full -- write admission and capital selection.

    lv / ms    ladder_value and the moneyness floors (their reason strings are f-strings of floats).
    items      build_items sorted by Item.key -- class, urgency descending, ticker, side -- with the
               record() of every unit, so the ORDER of admission is pinned, ties included.
    adm        admit over random plans: reducing cancels into debt, pairs deferred whole, orphaned
               cancels/places dropped, COLD vs TOPUP with and without `resting`, the reserve, and
               risk_u -> R -> contracts urgency fallbacks. Compared as json.dumps of
               (place, cancel, detail).
    sel        select_markets: two passes, the hysteresis, skipped reasons, per-ticker and global
               counts, duplicated tickers, realistic boards off the captured /markets rows, and
               strategy/test_budget.py's full 532-market board at the configured caps.
"""
import argparse, json, pathlib, random, sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
from gen_golden import emit, h, captures  # noqa: E402
from strategy import budget as B  # noqa: E402
from strategy import orders as O  # noqa: E402

rnd = random.Random(8080)


def sx(s):
    return s.encode("utf8").hex() or "-"


def jx(v):
    return sx(json.dumps(v))


def on(v):
    return "None" if v is None else h(v)


COUNTS = [("20.00", "20.00", "20.00"), ("20.00",), ("10.00", "20.00", "50.00"), ("7.50", "12.25")]
REASONS = ["edge_negative", "no_longer_quoting", "improve_1t", "improve_2t", "weird"]


def tick(x):
    return round(round(x / 0.01) * 0.01, 4)


def rand_plan(n_mkts):
    tks = [f"M{i}" for i in range(n_mkts)] or ["M0"]
    place, cancel = [], []
    for _ in range(rnd.randint(0, 25)):
        t, side = rnd.choice(tks), rnd.choice(["bid", "ask"])
        price = tick(rnd.uniform(0.02, 0.98))
        cnt = rnd.choice(["20.00", "10.00", "50.00", "7.50"])
        reason = rnd.choice(REASONS)
        c = {"ticker": t, "side": side, "price": price, "count": cnt, "order_id": f"o{rnd.randint(0, 999)}",
             "reason": reason}
        if reason.startswith("improve") or reason == "weird":
            pid = O._pair_id(t, side, price)
            c["pair"] = pid
            if rnd.random() < 0.85:
                for _k in range(1 if rnd.random() < 0.9 else 2):
                    place.append({"ticker": t, "side": side,
                                  "price": round(price + (0.01 if side == "bid" else -0.01), 4),
                                  "count": cnt, "reason": "improved", "pair": pid})
        cancel.append(c)
    for _ in range(rnd.randint(0, 15)):
        t, side = rnd.choice(tks), rnd.choice(["bid", "ask"])
        d = {"ticker": t, "side": side, "price": tick(rnd.uniform(0.02, 0.98)),
             "count": rnd.choice(["20.00", "10.00", "7.50"]),
             "reason": rnd.choice(["top_up", "cold_start"])}
        if rnd.random() < 0.05:
            d["pair"] = "orphan|bid|0.5000"
        place.append(d)
    rnd.shuffle(place)
    rnd.shuffle(cancel)
    return tks, place, cancel


def rand_em(tks):
    em = {}
    for t in tks:
        u = rnd.random()
        if u < 0.15:
            continue
        d = {}
        if rnd.random() < 0.7:
            d["risk_u"] = rnd.choice([rnd.random(), 0.5, 0.0, 1.0])
        if rnd.random() < 0.7:
            d["R"] = rnd.choice([rnd.random() * 0.3, 0.0645, 0.0])
        if rnd.random() < 0.8:
            d["quotable"] = rnd.random() < 0.9
        em[t] = d
    return em


def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--out", default="tests/data/golden")
    out = pathlib.Path(ap.parse_args().out)
    L = []

    for _ in range(5000):
        fair = rnd.choice([rnd.random(), tick(rnd.random()), 0.03, 0.97, 0.5])
        cn = rnd.choice(COUNTS + [()])
        sides = rnd.choice([("bid", "ask"), ("bid",), ("ask",)])
        L.append(f"lv {h(fair)} {jx(list(cn))} {jx(list(sides))} "
                 f"{h(B.ladder_value(fair, cn, sides))}")
    for _ in range(5000):
        st = rnd.choice(["between", "greater", "greater_or_equal", "less", "less_or_equal",
                         "custom", None])
        fair = rnd.choice([None, rnd.random(), 0.03, 0.05, 0.95, 0.0300000001, 0.97])
        L.append(f"ms {st or '-'} {on(fair)} {jx(B.moneyness_skip(st, fair))}")

    for _ in range(5000):
        tks, place, cancel = rand_plan(rnd.randint(1, 8))
        theos = None if rnd.random() < 0.2 else {t: rnd.choice([None, tick(rnd.random()),
                                                                rnd.random()]) for t in tks
                                                 if rnd.random() < 0.9}
        em = None if rnd.random() < 0.2 else rand_em(tks)
        resting = None
        if rnd.random() < 0.6:
            resting = [[t, s] for t in tks for s in ("bid", "ask") if rnd.random() < 0.4]
        tokens = rnd.choice([0.0, 12.0, 11.0, 100.0, 600.0, rnd.uniform(-50, 400), 10000.0])
        reserve = rnd.choice([None, 0.0, 88.0, 95.0, rnd.uniform(0, 300)])
        inp = {"place": place, "cancel": cancel, "tokens": tokens, "theos": theos, "em": em,
               "reserve": reserve, "resting": resting}
        rs = None if resting is None else {tuple(x) for x in resting}
        items = sorted(B.build_items(place, cancel, theos, em, resting=rs), key=B.Item.key)
        p, c, d = B.admit(place, cancel, tokens, theos=theos, em_map=em, reserve=reserve,
                          resting=rs)
        L.append(f"adm {jx(inp)} {jx([p, c, d])} {jx([i.record() for i in items])}")

    # The reference's own full board: 3,192 requotes against a 600-token cycle.
    pairs = []
    for i in range(532):
        for s in ("bid", "ask"):
            for r in range(3):
                pid = O._pair_id(f"KXINXU-{i}", s, 0.50 + 0.01 * r)
                # order_id BEFORE reason/pair: the shape plan_side's {**o, "reason"} copies of a
                # book row have. (test_budget.py builds it the other way round; the C++ Order has
                # no way to spell an id that follows the reason, and no production path makes one.)
                pairs.append(({"ticker": f"KXINXU-{i}", "side": s, "price": 0.50 + 0.01 * r,
                               "count": "10.00", "order_id": f"oid-{i}-{s}-{r}",
                               "reason": "improve_1t", "pair": pid},
                              {"ticker": f"KXINXU-{i}", "side": s,
                               "price": 0.51 + 0.01 * r, "count": "10.00", "reason": "improved",
                               "pair": pid}))
    plan_c = [x[0] for x in pairs]
    plan_p = [x[1] for x in pairs]
    red = [{"ticker": f"KXINXU-{i}", "side": "bid", "price": 0.5, "count": "10.00",
            "order_id": f"r{i}", "reason": "edge_negative"} for i in range(400)]
    for cc in (plan_c, plan_c + red):
        inp = {"place": plan_p, "cancel": cc, "tokens": 600.0, "theos": None, "em": None,
               "reserve": 200.0, "resting": None}
        p, c, d = B.admit(plan_p, cc, 600.0, reserve=200.0)
        items = sorted(B.build_items(plan_p, cc), key=B.Item.key)
        L.append(f"adm {jx(inp)} {jx([p, c, d])} {jx([i.record() for i in items[:50]])}")

    # --- select_markets ---------------------------------------------------------------------
    rows = []
    for f in captures("*/markets_iter*.json"):
        ms = json.loads(f.read_text())
        rows += ms if isinstance(ms, list) else ms.get("markets", [])
    real = [{"ticker": m["ticker"], "strike_type": m.get("strike_type")} for m in rows]

    def sel_case(markets, theos, em, mode, counts_js, incumbents, kw):
        if mode == "map":
            counts = {k_: tuple(v) for k_, v in counts_js.items()}
        elif mode == "tuple":
            counts = tuple(counts_js)
        else:
            counts = None
        chosen, d = B.select_markets(markets, theos, em, counts=counts, incumbents=incumbents,
                                     **kw)
        inp = {"markets": markets, "theos": theos, "em": em, "mode": mode, "counts": counts_js,
               "incumbents": sorted(incumbents), "kw": kw}
        L.append(f"sel {jx(inp)} {jx([sorted(chosen), d])}")
        return chosen

    for i in range(1800):
        if rnd.random() < 0.4 and real:
            markets = rnd.sample(real, min(len(real), rnd.randint(1, 120)))
        else:
            n = rnd.randint(0, 80)
            markets = [{"ticker": f"M{j:03d}", "strike_type": rnd.choice(
                ["between", "greater_or_equal", "greater", "less", None, "custom"])}
                for j in range(n)]
            if markets and rnd.random() < 0.1:
                markets.append(dict(rnd.choice(markets)))        # a duplicated ticker
        tks = [m["ticker"] for m in markets]
        theos = {t: rnd.choice([None, rnd.random(), 0.5, 0.02, 0.97, tick(rnd.random())])
                 for t in tks if rnd.random() < 0.95}
        em = rand_em(tks)
        mode = rnd.choice(["none", "tuple", "map", "map"])
        counts_js = None
        if mode == "tuple":
            counts_js = list(rnd.choice(COUNTS))
        elif mode == "map":
            counts_js = {t: list(rnd.choice(COUNTS[:2])) for t in tks if rnd.random() < 0.9}
        incumbents = {t for t in tks if rnd.random() < 0.3} | ({"GONE"} if rnd.random() < 0.2 else set())
        kw = {}
        if rnd.random() < 0.6:
            kw["max_value"] = rnd.choice([200.0, 500.0, 1000.0, 2500.0, rnd.uniform(0, 3000)])
        if rnd.random() < 0.4:
            kw["max_contracts"] = rnd.choice([400.0, 1000.0, 5000.0, rnd.uniform(0, 3000)])
        if rnd.random() < 0.4:
            kw["hysteresis"] = rnd.choice([0.7, 1.0, 0.0, rnd.random()])
        if rnd.random() < 0.4:
            kw["band_fraction"] = rnd.choice([0.6, 0.0, 1.0, rnd.random()])
        sel_case(markets, theos, em, mode, counts_js, incumbents, kw)

    # strategy/test_budget.py's boards, at the real caps.
    def board(n, stype="between", base_r=1.0):
        ms = [{"ticker": f"M{i:03d}", "strike_type": stype} for i in range(n)]
        return ms, {m["ticker"]: 0.50 for m in ms}, \
            {m["ticker"]: {"R": base_r - i * 0.001, "quotable": True} for i, m in enumerate(ms)}

    BAND = ["20.00", "20.00", "20.00"]
    ms, th, em = board(532)
    sel_case(ms, th, em, "tuple", BAND, set(), {})
    ms, th, em = board(45)
    first = sel_case(ms, th, em, "tuple", BAND, set(), {})
    em2 = dict(em)
    em2["M040"] = {"R": 0.9595, "quotable": True}
    em2["M044"] = {"R": 0.9600, "quotable": True}
    sel_case(ms, th, em2, "tuple", BAND, set(first), {})
    em3 = dict(em)
    em3["M000"] = {"R": 0.0001, "quotable": True}
    sel_case(ms, th, em3, "tuple", BAND, set(first), {})
    mixed = [{"ticker": f"B{i:03d}", "strike_type": "greater_or_equal"} for i in range(80)]
    cm = {m["ticker"]: (BAND if i % 3 == 0 else ["20.00"]) for i, m in enumerate(mixed)}
    thm = {m["ticker"]: 0.3 + 0.004 * i for i, m in enumerate(mixed)}
    emm = {m["ticker"]: {"risk_u": 1 - i / 100, "R": 0.01 * i, "quotable": True}
           for i, m in enumerate(mixed)}
    sel_case(mixed, thm, emm, "map", cm, set(), {})
    emit(out, "budget", "strategy/budget.py", L)


if __name__ == "__main__":
    main()
