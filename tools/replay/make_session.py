#!/usr/bin/env python3
"""Build a scripted quote-loop session for the parity gate.

    python tools/replay/make_session.py --scenario fast --out /tmp/x/fast.json

A SESSION is everything run_04.cycle reads from outside, cycle by cycle: the clocks, the Massive
option board, the index and futures levels, the Kalshi books, fills and positions, the collector's
health and re-centre answers, market discovery, and (for the live-against-a-fake-exchange
scenario) what the exchange answers. Both the Python reference (tools/replay/py_replay.py) and the
C++ port (apps/replay) consume the SAME file, so any difference in what they decide is a
difference in the code, not the inputs.

WHERE THE DATA COMES FROM. The option board is the real SPXW capture in data/fixtures (2026-08-21,
294 quotes). Each strike's implied vol is backed out of it once and the board is re-priced with
Black-Scholes along a random-walk spot, so every cycle's board is internally coherent. The NDX
board is the same surface carried to an NDX-sized spot and NDXP symbols -- synthetic, but it
exercises the second underlying, its own calendar, curve and band, and the merge. Kalshi books are
synthetic ladders around each market's rough fair. Markets are the real KXINX / KXINXU rows,
and their NDX twins.

The session starts at 11:57 ET so the H1200 hourly event closes mid-session: that exercises
`_still_open`, the 60 s discovery cache and the band/selection reshuffle a closing event causes.
"""
import argparse, copy, datetime as dt, json, math, pathlib, random, sys

HERE = pathlib.Path(__file__).resolve().parent
REF = pathlib.Path(__import__("os").environ.get("KP_REF", HERE.parent.parent / "ref" / "current")).resolve()
sys.path.insert(0, str(REF))
import config  # noqa: E402
from pricing import bs  # noqa: E402

FIX = REF / "data" / "fixtures" / "skew-20260824-rebaseline"
UTC = dt.timezone.utc
NDX_MULT = 3.8


def ns_of(t):
    d = t - dt.datetime(1970, 1, 1, tzinfo=UTC)
    return (d.days * 86400 + d.seconds) * 10**9 + d.microseconds * 1000


def occ(root, expiry, right, k):
    return f"O:{root}{expiry}{right}{int(round(k * 1000)):08d}"


def load_rows(name):
    r = json.loads((FIX / name).read_text())
    rows = r.get("markets", r) if isinstance(r, dict) else r
    keep = ("ticker", "event_ticker", "strike_type", "floor_strike", "cap_strike", "close_time")
    return [{k: m.get(k) for k in keep} for m in rows]


def ndx_twin(rows, daily):
    out = []
    for m in rows:
        n = dict(m)
        for k in ("floor_strike", "cap_strike"):
            if n[k] is not None:
                n[k] = round(n[k] * NDX_MULT / 25.0) * 25.0 - (0.0001 if str(m[k]).endswith(".9999") else 0.0)
        ser_old = "KXINX" if daily else "KXINXU"
        ser_new = "KXNASDAQ100" if daily else "KXNASDAQ100U"
        n["event_ticker"] = m["event_ticker"].replace(ser_old, ser_new, 1)
        n["ticker"] = n["event_ticker"] + "-" + m["ticker"].rsplit("-", 1)[1][0] + (
            f"{n['floor_strike'] if n['floor_strike'] is not None else n['cap_strike']:g}")
        out.append(n)
    # distinct tickers only (rounding can collide)
    seen, uniq = set(), []
    for n in out:
        if n["ticker"] not in seen:
            seen.add(n["ticker"])
            uniq.append(n)
    return uniq


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--scenario", choices=["fast", "rest", "live"], required=True)
    ap.add_argument("--cycles", type=int, default=120)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    rnd = random.Random(1000 + a.seed + {"fast": 0, "rest": 1, "live": 2}[a.scenario])

    snap0 = json.loads((FIX / "quotes_snapshot_iter1.json").read_text())
    t_cap = dt.datetime.fromtimestamp(max(q["t"] for q in snap0.values()) / 1000, UTC)
    close = dt.datetime(2026, 8, 21, 16, 0, tzinfo=config.ET)
    T_cap = (close - t_cap).total_seconds() / (252 * 23400.0)

    # the SPX strip, and each strike's vol backed out once from the capture's mids
    calls, puts = {}, {}
    for sym, q in snap0.items():
        k = int(sym[-8:]) / 1000.0
        (calls if sym[-9] == "C" else puts)[k] = q
    strikes = sorted(set(calls) & set(puts))
    parity = [(k, (calls[k]["bp"] + calls[k]["ap"]) / 2 - (puts[k]["bp"] + puts[k]["ap"]) / 2 + k)
              for k in strikes if calls[k].get("bp") and puts[k].get("bp")]
    parity.sort(key=lambda p: abs(p[1] - p[0]))
    F0 = parity[0][1]
    vols = {}
    for k in strikes:
        q = calls[k] if k >= F0 else puts[k]
        mid = ((q.get("bp") or 0) + (q.get("ap") or 0)) / 2
        w = bs.implied_w(F0, k, mid, "C" if k >= F0 else "P") if mid > 0 else None
        vols[k] = (w / math.sqrt(T_cap)) if w else None
    good = [v for v in vols.values() if v]
    for k in strikes:
        if not vols[k]:
            vols[k] = max(good) if k < F0 else min(good)
    half = {k: max(0.05, ((calls[k].get("ap") or 0) - (calls[k].get("bp") or 0)) / 2) for k in strikes}

    boards = [
        {"underlying": "NDX", "daily_series": ["KXNASDAQ100"], "hourly_series": ["KXNASDAQ100U"],
         "root": "NDXP", "expiry": "260821", "mult": NDX_MULT},
        {"underlying": "SPX", "daily_series": ["KXINX"], "hourly_series": ["KXINXU"],
         "root": "SPXW", "expiry": "260821", "mult": 1.0},
    ]
    daily = load_rows("markets_iter1.json")
    hourly = load_rows("markets_iter2.json")
    disc = {"SPX": {"daily": daily, "hourly": hourly},
            "NDX": {"daily": ndx_twin(daily, True), "hourly": ndx_twin(hourly, False)}}
    for b in boards:
        m = b["mult"]
        b["strikes"] = sorted({round(k * m / 5.0) * 5.0 if m != 1.0 else k for k in strikes})
        b["kmap"] = {(round(k * m / 5.0) * 5.0 if m != 1.0 else k): k for k in strikes}
        b["tickers"] = [occ(b["root"], b["expiry"], r, k) for k in b["strikes"] for r in ("C", "P")]
    state_tickers = [m["ticker"] for u in ("NDX", "SPX") for m in disc[u]["daily"] + disc[u]["hourly"]]

    start = dt.datetime(2026, 8, 21, 11, 57, 0, tzinfo=config.ET)
    S = F0 - 3.0

    # --- the realized-vol lean's backfill: today's index seconds from the open, and the ETF
    #     proxy's extended-hours seconds, both ending where the session starts. What
    #     massive.index.closes_ms / massive.history.second_bars would have returned.
    lean = {}
    for b in boards:
        u, m = b["underlying"], b["mult"]
        t0 = int(dt.datetime(2026, 8, 21, 9, 30, tzinfo=config.ET).timestamp() * 1000)
        t1 = int(start.timestamp() * 1000)
        lvl, closes = S * m * 0.998, []
        for t in range(t0, t1, 1000):
            lvl += rnd.gauss(0, 0.18 * m)
            if rnd.random() < 0.995:                       # the odd missing second
                closes.append([t, round(lvl, 2)])
        p0 = int(dt.datetime(2026, 8, 21, 7, 0, tzinfo=config.ET).timestamp() * 1000)
        px, proxy = (S * m / 10.0 if u == "SPX" else S * m / 41.0), []
        for t in range(p0, t1, 1000):
            px += rnd.gauss(0, 0.018)
            if rnd.random() < 0.6:
                proxy.append({"t": t, "c": round(px, 3) if rnd.random() > 0.01 else None})
        lean[u] = {"closes": closes, "proxy": proxy}
    basis = {"SPX": 21.5, "NDX": 88.0}
    mono0 = 50000.0
    cycles = []
    fair_centre = {}
    for i in range(a.cycles):
        now = start + dt.timedelta(seconds=2 * i, microseconds=rnd.randint(0, 999) * 1000)
        mono = mono0 + 2.0 * i + rnd.random() * 0.01
        S += rnd.gauss(0, 0.9)
        T = max((close - now).total_seconds(), 60.0) / (252 * 23400.0)
        c = {"now_ns": ns_of(now), "mono": mono}

        # --- the option board, re-priced at this spot
        snap = {}
        for b in boards:
            Fb = S * b["mult"]
            for kb in b["strikes"]:
                k0 = b["kmap"][kb]
                w = vols[k0] * math.sqrt(T)
                for right in ("C", "P"):
                    if rnd.random() < 0.01:
                        continue                                  # a quote that did not arrive
                    p = bs.bs_price(Fb, kb, w, right)
                    h = half[k0] * b["mult"] * (1 + 0.3 * rnd.random())
                    bp = round(max(0.0, p - h), 2)
                    ap_ = round(max(p + h, bp + 0.05), 2)
                    q = [bp if bp > 0 else None, ap_, rnd.randint(1, 300) if bp > 0 else None,
                         rnd.randint(1, 300), int(now.timestamp() * 1000) - rnd.randint(0, 1500)]
                    snap[occ(b["root"], b["expiry"], right, kb)] = q
        c["snap"] = snap

        # --- index levels; every so often one is missing or stale
        idx = {}
        for b in boards:
            if rnd.random() < 0.03:
                continue
            lvl = round(S * b["mult"] + rnd.gauss(0, 0.2), 2)
            age = rnd.choice([0.3, 0.8, 1.2, 2.0] + ([30.0] if rnd.random() < 0.05 else []))
            idx[b["underlying"]] = {"ticker": "I:" + b["underlying"], "value": lvl,
                                    "ts_ns": ns_of(now) - int(age * 1e9),
                                    "market_status": "open", "name": b["underlying"]}
        c["index"] = idx

        # --- futures (fast-spot scenarios): what the socket says, and what the basis thread folds
        if a.scenario in ("fast", "live"):
            c["fut"] = {b["underlying"]: [round(S * b["mult"] + basis[b["underlying"]] + rnd.gauss(0, 0.25), 2),
                                          round(rnd.uniform(0.02, 0.4), 3)] for b in boards}
            # the SECOND read of the futures mid in a cycle is the re-price-at-send one: the
            # futures have moved since the board was priced, and that move is what it re-prices.
            c["fut2"] = {u: [round(v[0] + rnd.choice([0.0, 0.25, -0.25, 0.5, -0.75, 1.25]) *
                                    (NDX_MULT if u == "NDX" else 1.0), 2), v[1]]
                         for u, v in c["fut"].items()}
            c["move_scale"] = {b["underlying"]: (None if rnd.random() < 0.1 else
                                                 round(abs(rnd.gauss(0.25, 0.1)) * b["mult"], 4))
                               for b in boards}
            c["basis_obs"] = []
            for j in range(2):
                for b in boards:
                    u = b["underlying"]
                    if u in idx:
                        iqj = dict(idx[u])
                        iqj["ts_ns"] -= (1 - j) * 10**9
                        c["basis_obs"].append([u, c["fut"][u][0] + rnd.gauss(0, 0.1), iqj,
                                               mono - 1.0 + j * 0.5])
        # --- the REST futures poll (delta-adjust scenario)
        if a.scenario == "rest":
            c["ft_rows"] = {b["underlying"]: (None if rnd.random() < 0.05 else
                                              {"bid_price": round(S * b["mult"] + basis[b["underlying"]] - 0.125, 2),
                                               "ask_price": round(S * b["mult"] + basis[b["underlying"]] + 0.125, 2),
                                               "timestamp": ns_of(now) - rnd.randint(50, 900) * 10**6})
                            for b in boards}

        # --- the collector: mostly alive, a few dead / stale cycles to exercise the gates
        g = rnd.random()
        c["col"] = {"alive": g > 0.02, "pump_error": None if g > 0.02 else "WebSocketConnectionClosedException: gone",
                    "quote_age_s": (None if i == 0 else (40.0 if 0.02 < g < 0.04 else round(rnd.uniform(0.01, 2.0), 3)))}
        c["reconnect"] = rnd.random() < 0.7
        r = rnd.random()
        c["recenter"] = None if r < 0.8 else ("error:MassiveError:1100 tickers over the 1000 cap" if r < 0.83 else
                                             [rnd.randint(0, 12), rnd.randint(0, 12)])

        # --- Kalshi books, feed health and positions
        books = {}
        for u in ("NDX", "SPX"):
            for m in disc[u]["daily"] + disc[u]["hourly"]:
                tk = m["ticker"]
                cen = fair_centre.setdefault(tk, rnd.uniform(0.03, 0.97))
                cen = min(0.98, max(0.02, cen + rnd.gauss(0, 0.004)))
                fair_centre[tk] = cen
                if rnd.random() < 0.05:
                    continue
                spread = rnd.choice([0.01, 0.02, 0.03, 0.05, 0.1])
                yb = round(max(0.01, cen - spread / 2), 2)
                ya = round(min(0.99, max(yb + 0.01, cen + spread / 2)), 2)
                yes = [[f"{max(0.01, yb - 0.01 * j):.4f}", f"{rnd.randint(1, 500)}.00"] for j in range(rnd.randint(0, 3))][::-1]
                no = [[f"{max(0.01, 1 - ya - 0.01 * j):.4f}", f"{rnd.randint(1, 500)}.00"] for j in range(rnd.randint(0, 3))][::-1]
                books[tk] = {"yes": yes, "no": no}
        c["books"] = books
        c["feed_alive"] = rnd.random() > 0.03
        c["coverage"] = 0.95 if rnd.random() > 0.08 else 0.5
        c["pump_error"] = None if c["feed_alive"] else "ConnectionResetError: peer"
        c["positions"] = {}
        for tk in rnd.sample(state_tickers, 4):
            c["positions"][tk] = rnd.choice([0.0, 20.0, -20.0, 40.0, -100.0, 100.0])
        # --- the account the loss limit marks: REST balance + positions, polled; sometimes stale
        #     or unreadable, and holding a market we do not quote (read via feed.entry)
        if rnd.random() < 0.08:
            c["acct"] = None
        else:
            acct_bal = round(3500.0 + 40 * math.sin(i / 9.0) + rnd.gauss(0, 3), 4)
            held = {tk: rnd.choice([20.0, -20.0, 40.0, -60.0]) for tk in rnd.sample(state_tickers, 3)}
            held["KXINX-26AUG20H1600-B7700"] = -20.0
            # a snapshot past RISK_ACCOUNT_STALE_HALT_S halts the session for good, so it is placed
            # only near the END of the fast session: the halt path is exercised without costing the
            # other scenarios (or the rest of this one) their coverage.
            stale = 40.0 if (a.scenario == "fast" and i == a.cycles - 6) else rnd.choice([0.3, 0.9, 1.5, 0.5])
            c["acct"] = [mono - stale, acct_bal, held]
        c["fills"] = []
        if rnd.random() < 0.15:
            c["fills"].append({"market_ticker": rnd.choice(state_tickers), "order_id": None,
                               "count_fp": rnd.choice(["20.00", "5.00", "bad"]),
                               "yes_price_dollars": f"{rnd.uniform(0.02, 0.98):.4f}",
                               "outcome_side": rnd.choice(["yes", "no", "YES", None]),
                               "side": rnd.choice(["yes", "no"]), "fee_cost": "0.0000"})
        if a.scenario == "live" and rnd.random() < 0.25:
            c["fill_picks"] = [[rnd.randint(0, 50), rnd.choice(["5.00", "20.00"]),
                                f"{rnd.uniform(0.02, 0.98):.4f}"]]
        if a.scenario == "live":
            c["api"] = rnd.choice(["ok"] * 8 + ["partial", "http429", "not_found"])
        cycles.append(c)

    sess = {"scenario": a.scenario, "live": a.scenario == "live", "tiers": True,
            "no_reprice_at_send": False, "has_mdfeed": True,
            "has_futures": a.scenario in ("fast", "live"), "delta_adj": a.scenario == "rest",
            "smooth_skew": True, "max_batch": 30, "bucket": {"capacity": 900.0, "refill": 300.0},
            "boards": [{k: v for k, v in b.items() if k not in ("mult", "kmap")} for b in boards],
            "lean": lean,
            "state_tickers": state_tickers, "discover": disc, "cycles": cycles}
    pathlib.Path(a.out).parent.mkdir(parents=True, exist_ok=True)
    pathlib.Path(a.out).write_text(json.dumps(sess))
    print(f"{a.scenario}: {len(cycles)} cycles, {len(state_tickers)} markets, "
          f"{sum(len(b['tickers']) for b in boards)} option tickers -> {a.out}")


if __name__ == "__main__":
    main()
