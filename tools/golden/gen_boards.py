#!/usr/bin/env python3
"""pricing/validate_theo.py resolve_and_price_boards (+ universe.quotes_by_strike) -- the cycle's
pricing entry point, driven exactly as run_04.cycle() drives it.

Two vector files:

  boards.vec (source pricing/validate_theo.py) -- SESSIONS. Per underlying a SkewState is carried
  across cycles (as run_04's `b.sk`), the monotonic clock steps ~2s a cycle, the option snapshot is
  the real captured SPXW board (plus a synthetic NDXP strip sharing the same snapshot dict, because
  the collector spans both on one socket) perturbed each cycle, and the index quote cycles through
  every spot.resolve branch: fresh (-> the index wins and the daily Pricer is rebuilt at it),
  stale, future-dated, no timestamp, value None, absent. Every cycle dumps the SpotResolution, the
  daily pricer's spot and curve, whether a smoothed curve was installed, the hourly pricer's
  forward and w_4pm, and EVERY fair on the merged {ticker: (fair, detail)} in its dict order --
  daily first, hourly after, with the detail fields run_04 and the journal read.
  Calendar None and now None are swept (the hourly pricer must then not exist).

  universe_qbs.vec (source pricing/universe.py) -- quotes_by_strike over the same snapshot with
  wrong roots, wrong expiries, unlisted and off-grid strikes, and an EMPTY frame (Python `if q:`).

The index clock is driven by patching massive.index.datetime; the skew clock by patching
strategy.skewstate.time. Neither module is modified.

    snap  <id> <n> <sym:bp:ap:bs:as>...
    sess  <u> <n> <has SkewState 0/1>          (a fresh SkewState per session)
    mkt   <u> <daily|hourly> \t <json row>
    cycle <u> <snap> <root> <expiry> <now_ns|None> <cal 0/1> <mono> <iq> <at_ns> <smooth 0/1> <nstrikes> <K>...
          <iq> = None | <value|None>:<ts|None>:<status|->
    res   <value> <source> <index> <parity> <age> <status> <reason> <div> <divpct> <alarm>
    pr    <spot> <skew flat/fit/None> <skew reason> <smoothed 0/1> <hourly 0/1> <hp spot> <hp w4pm>
    fair  <ticker> <hourly 0/1> <fair> <reason> <prob_used> <p_lo> <p_hi> <w> <var_ratio> <fwd> <mts> <fwd_src> <idx-par> <par_fwd> <atm_reason>
    sdet  \t <skewstate detail json>
"""
import argparse, datetime as dt, json, math, pathlib, random, sys, types
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
import gen_golden  # noqa: E402
from gen_golden import emit, h, captures, config  # noqa: E402
from massive import client as mv  # noqa: E402
from massive import index as ix  # noqa: E402
from pricing import bs, universe, validate_theo  # noqa: E402
from pricing import varcal as vc  # noqa: E402
from strategy import skewstate  # noqa: E402

NS = 1_000_000_000
EXP = "260821"
D = dt.date(2026, 8, 21)


def on(v):
    return "None" if v is None else h(v)


def ostr(s):
    return "-" if s is None else str(s)


def et(hh, mm, ss=0, us=0):
    return dt.datetime.combine(D, dt.time(hh, mm, ss, us), tzinfo=config.ET)


EPOCH = dt.datetime(1970, 1, 1, tzinfo=dt.timezone.utc)


def ns(t):
    """EXACT epoch ns of a datetime (int(t.timestamp() * 1e9) loses the microseconds)."""
    d = t - EPOCH
    return (d.days * 86400 + d.seconds) * NS + d.microseconds * 1000


class Clock:
    wall = None      # datetime (UTC) for IndexQuote.age_s
    mono = 0.0


class _FakeDT(dt.datetime):
    @classmethod
    def now(cls, tz=None):
        return Clock.wall.astimezone(tz) if tz else Clock.wall


ix.datetime = types.SimpleNamespace(datetime=_FakeDT, timezone=dt.timezone)
skewstate.time = types.SimpleNamespace(monotonic=lambda: Clock.mono)


def ndx_strip(F, rnd):
    strikes = sorted(set([F + 5.0 * i for i in range(-80, 81)] +
                         [F + 25.0 * i for i in range(-60, 61)]))
    snap = {}
    for k in strikes:
        x = math.log(k / F) / 0.006
        w = 0.006 * max(0.4, 1.0 - 0.07 * x + 0.01 * x * x)
        for cp in ("C", "P"):
            mid = bs.bs_price(F, k, w, cp)
            hs = max(0.05, 0.02 * mid) + rnd.choice((0.0, 0.05, 0.5))
            fr = {"ev": "Q", "sym": mv.occ_ticker("NDXP", EXP, cp, k), "ap": round(mid + hs, 2),
                  "as": rnd.randint(1, 200)}
            if mid - hs > 0.05:
                fr["bp"] = round(mid - hs, 2)
                fr["bs"] = rnd.randint(1, 200)
            snap[fr["sym"]] = fr
    return snap, strikes


def ndx_markets(F):
    out = []
    lo = math.floor((F - 1500) / 250) * 250
    close = "2026-08-21T20:00:00Z"
    out.append({"ticker": "KXNASDAQ100-26AUG21H1600-B0", "strike_type": "less",
                "floor_strike": None, "cap_strike": lo - 0.01, "close_time": close})
    for i in range(12):
        a = lo + 250 * i
        out.append({"ticker": f"KXNASDAQ100-26AUG21H1600-B{a}", "strike_type": "between",
                    "floor_strike": float(a), "cap_strike": a + 249.99, "close_time": close})
    out.append({"ticker": "KXNASDAQ100-26AUG21H1600-T", "strike_type": "greater",
                "floor_strike": lo + 3000 - 0.01, "cap_strike": None, "close_time": close})
    hourly = []
    for hh, z in ((12, 16), (13, 17), (14, 18), (15, 19), (16, 20)):
        for j in range(-6, 7):
            k = round(F / 100) * 100 + 100 * j - 0.01
            hourly.append({"ticker": f"KXNASDAQ100U-26AUG21H{hh}00-T{k:.2f}",
                           "strike_type": "greater_or_equal", "floor_strike": k,
                           "cap_strike": None, "close_time": f"2026-08-21T{z}:00:00Z"})
    # odd rows every board eventually meets
    hourly.append({"ticker": "KXNASDAQ100U-NOCLOSE", "strike_type": "greater_or_equal",
                   "floor_strike": F, "cap_strike": None, "close_time": None})
    hourly.append({"ticker": "KXNASDAQ100U-PAST", "strike_type": "greater_or_equal",
                   "floor_strike": F, "cap_strike": None, "close_time": "2026-08-21T13:00:00Z"})
    return out, hourly


KEEP = ("ticker", "strike_type", "floor_strike", "cap_strike", "close_time", "event_ticker")


def trim(m):
    return {k: m.get(k) for k in KEEP if k in m}


def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--out", default="tests/data/golden")
    out = pathlib.Path(ap.parse_args().out)
    rnd = random.Random(20260821)
    L, Q = [], []

    raw = json.loads(captures("*/quotes_snapshot_iter1.json")[0].read_text())
    spx_strikes = sorted({int(s[-8:]) / 1000 for s in raw})
    daily_spx = [trim(m) for m in json.loads(captures("*/markets_iter1.json")[0].read_text())]
    hourly_spx = [trim(m) for m in json.loads(captures("*/markets_iter2.json")[0].read_text())]
    F_ndx = 29117.5
    ndx_snap, ndx_strikes = ndx_strip(F_ndx, rnd)
    daily_ndx, hourly_ndx = ndx_markets(F_ndx)
    # a duplicate ticker in both lists: dict.update keeps the FIRST position, hourly value wins
    dup = dict(hourly_spx[5]); dup["strike_type"] = "greater"; daily_spx.append(dup)

    for u, dl, hl in (("SPX", daily_spx, hourly_spx), ("NDX", daily_ndx, hourly_ndx)):
        for m in dl:
            L.append(f"mkt {u} daily\t{json.dumps(m)}")
        for m in hl:
            L.append(f"mkt {u} hourly\t{json.dumps(m)}")

    base = dict(raw); base.update(ndx_snap)
    nsnap = [0]

    def emit_snap(snap):
        sid = nsnap[0]; nsnap[0] += 1
        toks = []
        for sym, fr in snap.items():
            if not fr:
                toks.append(f"{sym}:EMPTY")
            else:
                toks.append(f"{sym}:{on(fr.get('bp'))}:{on(fr.get('ap'))}:"
                            f"{on(fr.get('bs'))}:{on(fr.get('as'))}")
        L.append(f"snap {sid} {len(toks)} " + " ".join(toks))
        return sid

    def perturb(snap, level):
        out_ = {}
        for sym, fr in snap.items():
            u_ = rnd.random()
            if u_ < level * 0.3:
                continue
            fr = dict(fr)
            if u_ < level * 0.3 + 0.03 and fr.get("ap") is not None:
                fr["ap"] = fr["ap"] + rnd.choice((0.05, 0.3, 2.0))
            elif u_ < level * 0.3 + 0.05 and fr.get("bp") is not None:
                fr["bp"] = max(0.05, fr["bp"] - rnd.choice((0.05, 0.3)))
            elif u_ < level * 0.3 + 0.06:
                fr.pop("bp", None); fr.pop("bs", None)
            out_[sym] = fr
        return out_

    cals = {"SPX": vc.load("SPX"), "NDX": vc.load("NDX")}
    for u, root, strikes, dl, hl, F0 in (("SPX", "SPXW", spx_strikes, daily_spx, hourly_spx, 7695.0),
                                         ("NDX", "NDXP", ndx_strikes, daily_ndx, hourly_ndx, F_ndx)):
        for sess in range(3):
            sk = skewstate.SkewState(u) if sess < 2 else None
            L.append(f"sess {u} {sess} {1 if sk is not None else 0}")
            mono = 1000.0 + 5000.0 * sess
            now = et(10, 30) + dt.timedelta(minutes=97 * sess)
            for cyc in range(30):
                mono += rnd.choice((2.0, 2.0, 2.0, 2.1, 1.9, 5.0, 0.5, 31.0))
                now += dt.timedelta(microseconds=rnd.randrange(1_500_000, 2_600_000))
                if cyc % 11 == 10:
                    now = now.replace(hour=rnd.choice((11, 13, 15)), minute=rnd.randrange(0, 60))
                level = rnd.choice((0.0, 0.0, 0.05, 0.2, 0.9, "noputs"))
                if level == "noputs":
                    # every put one-sided: parity cannot price, so the daily spot is the index or
                    # nothing and the hourly forward is the INDEX FALLBACK
                    snap = {sym: ({k: v for k, v in fr.items() if k not in ("bp", "bs")}
                                  if sym[-9] == "P" else fr) for sym, fr in base.items()}
                else:
                    snap = perturb(base, level) if level else base
                sid = emit_snap(snap)
                # the index quote, cycling the resolve branches
                kind = rnd.choice(("fresh", "fresh", "fresh", "stale", "future", "nots", "noval",
                                   "none"))
                Clock.wall = now.astimezone(dt.timezone.utc)
                at_ns = int(Clock.wall.timestamp() * NS)
                iq = None
                if kind != "none":
                    val = F0 + rnd.gauss(0, 2.0) if u == "SPX" else F0 + rnd.gauss(0, 8.0)
                    if rnd.random() < 0.1:
                        val = val * 1.01                       # trips the divergence alarm
                    age = {"fresh": rnd.uniform(0, 9.9), "stale": rnd.uniform(10.01, 60),
                           "future": -rnd.uniform(0.001, 2), "nots": 0.0, "noval": 1.0}[kind]
                    ts = None if kind == "nots" else at_ns - int(age * NS)
                    iq = ix.IndexQuote(u, ix.INDEX_TICKER[u], None if kind == "noval" else val,
                                       ts, rnd.choice(("open", None)))
                Clock.mono = mono
                use_cal = rnd.random() > 0.08
                use_now = rnd.random() > 0.05
                ks = strikes if rnd.random() > 0.1 else strikes[::2]
                pricers, fairs, res = validate_theo.resolve_and_price_boards(
                    dl, hl, snap, root, EXP, ks, now=(now if use_now else None),
                    calendar=(cals[u] if use_cal else None), index_quote=iq, skew_state=sk)
                iq_tok = ("None" if iq is None else
                          f"{on(iq.value)}:{'None' if iq.ts_ns is None else iq.ts_ns}:"
                          f"{iq.market_status or '-'}")
                L.append(f"cycle {u} {sid} {root} {EXP} {ns(now) if use_now else 'None'} "
                         f"{1 if use_cal else 0} {h(mono)} {iq_tok} {at_ns} "
                         f"{1 if sk is not None else 0} {len(ks)} " + " ".join(h(k) for k in ks))
                L.append(f"res {on(res.value)} {ostr(res.source)} {on(res.index)} "
                         f"{on(res.parity)} {on(res.index_age_s)} {ostr(res.market_status)} "
                         f"{ostr(res.reason)} {on(res.divergence)} {on(res.divergence_pct)} "
                         f"{1 if res.alarm else 0}")
                dp, hp = pricers["daily"], pricers["hourly"]
                installed = dp._skew_built and dp._skew is not None
                skf = dp.skew
                L.append(f"pr {on(dp.spot)} "
                         f"{'None' if skf is None else ('flat' if skf.flat else 'fit')} "
                         f"{ostr(skf.reason if skf is not None else None)} "
                         f"{1 if (sk is not None and installed and skf is not None and skf.ok and sk.curve is skf) else 0} "
                         f"{0 if hp is None else 1} "
                         f"{'None' if hp is None else on(hp.spot)} "
                         f"{'None' if hp is None else on(hp.w_4pm)}")
                for tk, (fair, d) in fairs.items():
                    hr = "w_4pm" in d
                    L.append(f"fair {tk} {1 if hr else 0} {on(fair)} {ostr(d.get('reason'))} "
                             f"{ostr(d.get('prob_used'))} {on(d.get('p_above_lo'))} "
                             f"{on(d.get('p_above_hi'))} {on(d.get('w'))} "
                             f"{on(d.get('var_ratio'))} {on(d.get('forward'))} "
                             f"{on(d.get('minutes_to_settlement'))} "
                             f"{ostr(d.get('forward_source'))} {on(d.get('index_minus_parity'))} "
                             f"{on(d.get('parity_forward'))} {ostr(d.get('atm_reason'))}")
                if sk is not None:
                    L.append("sdet\t" + json.dumps(sk.detail(), separators=(",", ":")))

    emit(out, "boards", "pricing/validate_theo.py", L)

    # --- quotes_by_strike ---
    snap = dict(base)
    snap[mv.occ_ticker("SPXW", EXP, "C", 7702.5)] = {}             # an EMPTY frame: `if q:` false
    snap[mv.occ_ticker("SPXW", EXP, "P", 7702.5)] = {"ap": 1.5, "as": 3}
    sid = None
    toks = []
    for sym, fr in snap.items():
        toks.append(f"{sym}:EMPTY" if not fr else
                    f"{sym}:{on(fr.get('bp'))}:{on(fr.get('ap'))}:{on(fr.get('bs'))}:{on(fr.get('as'))}")
    Q.append(f"snap 0 {len(toks)} " + " ".join(toks))
    cases = [("SPXW", EXP, spx_strikes), ("SPXW", EXP, spx_strikes[::7] + [7702.5, 7703.0, 1.0]),
             ("SPX", EXP, spx_strikes), ("SPXW", "260822", spx_strikes),
             ("NDXP", EXP, ndx_strikes), ("NDXP", EXP, [F_ndx + 0.0005, F_ndx + 0.0015, F_ndx]),
             ("NDXP", EXP, []), ("SPXW", EXP, [7700.0, 7700.0])]
    for _ in range(40):
        pool = spx_strikes if rnd.random() < 0.5 else ndx_strikes
        cases.append((rnd.choice(("SPXW", "NDXP")), EXP,
                      rnd.sample(pool, rnd.randrange(0, 40)) + [rnd.uniform(7000, 30000)]))
    for root, exp, ks in cases:
        b = universe.quotes_by_strike(snap, root, exp, ks)
        Q.append(f"qbs {root} {exp} {len(ks)} " + " ".join(h(k) for k in ks) + f"\t{len(b)} " +
                 " ".join(f"{r}:{h(k)}:{on(q.get('bp'))}:{on(q.get('ap'))}:{on(q.get('bs'))}:"
                          f"{on(q.get('as'))}" for (r, k), q in sorted(b.items())))
    emit(out, "universe_qbs", "pricing/universe.py", Q)


if __name__ == "__main__":
    main()
