#!/usr/bin/env python3
"""strategy/risktape.py -- RiskTape rows, compared on the BYTES the real Journal writes.

The reference RiskTape writes through a real strategy.journal.Journal pointed at a temp dir
(never the reference tree), with time.time_ns patched so `ts_ns` is reproducible. Every
snapshot's written line is a vector, and so is the returned row for `write=False`.

Inputs are the real captured SPXW board and the real KXINX (daily) and KXINXU (hourly) rows, with
the per-market Greeks taken from a REAL EdgeManager.refresh on that board (emitted, so the C++
builds the identical em_map), plus random positions (signed, zero, None, NaN, other-underlying
tickers, repeated insertion orders). Swept: the pricer's lazy curve, an installed fitted curve, an
installed FLAT curve with and without an ATM vol, no pricer at all; the fallback ATM vol from
sigma and the calendar (and its absence: no sigma, sigma 0, no calendar, after the close); every
bucket boundary; the next hourly settlement across the day, ties, and none left; the cadence
(`due`) on its boundary. Pure helpers (reference_strike, vanilla_call_delta, bucket_of) are
swept directly too.

    board  <n> <R:K:bp:ap:bs:as>...
    mkt    <daily|hourly>\t<json row>
    em     <set>\t<ticker> <delta> <vega> <theta>          (one EdgeManager map per set)
    tape   <interval|None>
    snap   <underlying> <now_ns> <ts_ns> <mono> <write> <pricer> <cal> <sigma_impl> <sigma_rv>
           <spot> <fast_spot> <em set> <hourly 0/1> <npos> <tk=pos>... \t <expected row json> \t <written line or ->
    due    <mono> <0/1>
    det    \t <json>
    vcd    <F> <K> <mode> <w_atm> <delta>
    bkt    <delta> <index|None>
"""
import argparse, datetime as dt, json, math, pathlib, random, sys, tempfile, types
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
import gen_golden  # noqa: E402
from gen_golden import emit, h, captures, config  # noqa: E402
from pricing import edge as pedge, skew as skewmod, theo  # noqa: E402
from pricing import varcal as vc  # noqa: E402
from strategy import edgemgr as emod, journal as jnl, risktape as rt  # noqa: E402

UTC = dt.timezone.utc
EPOCH = dt.datetime(1970, 1, 1, tzinfo=UTC)


def on(v):
    return "None" if v is None else h(v)


def ns_of(t):
    d = t - EPOCH
    return (d.days * 86400 + d.seconds) * 1_000_000_000 + d.microseconds * 1000


class Clk:
    ts_ns = 0


rt.time = types.SimpleNamespace(time_ns=lambda: Clk.ts_ns, monotonic=lambda: 0.0)

KEEP = ("ticker", "strike_type", "floor_strike", "cap_strike", "close_time")


def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--out", default="tests/data/golden")
    out = pathlib.Path(ap.parse_args().out)
    rnd = random.Random(5150)
    L = []

    board, strikes = gen_golden._real_boards()[0]
    toks = [f"{r}:{h(k)}:{on(q.get('bp'))}:{on(q.get('ap'))}:{on(q.get('bs'))}:{on(q.get('as'))}"
            for (r, k), q in board.items()]
    L.append(f"board {len(toks)} " + " ".join(toks))
    daily = [{k: m.get(k) for k in KEEP} for m in json.loads(captures("*/markets_iter1.json")[0].read_text())]
    hourly = [{k: m.get(k) for k in KEEP} for m in json.loads(captures("*/markets_iter2.json")[0].read_text())]
    # a tie on the soonest settlement and a market with no close time
    hourly.append(dict(hourly[0], ticker="KXINXU-TIE-TWIN"))
    hourly.append(dict(hourly[1], ticker="KXINXU-NOCLOSE", close_time=None))
    for m in daily:
        L.append(f"mkt daily\t{json.dumps(m)}")
    for m in hourly:
        L.append(f"mkt hourly\t{json.dumps(m)}")
    markets = daily + hourly
    tickers = [m["ticker"] for m in markets]

    cal = vc.load("SPX")
    curve = pedge.Curve.load("SPX")
    p0 = theo.Pricer(board, strikes)
    F = p0.spot
    w_atm = skewmod.atm_vol(board, strikes, F)

    D = dt.date(2026, 8, 21)
    em_sets = []
    for si, (hh, mm, sig) in enumerate(((10, 15, 0.11), (13, 30, 0.14), (15, 58, 0.12))):
        now = dt.datetime.combine(D, dt.time(hh, mm), tzinfo=config.ET)
        em = emod.EdgeManager("SPX", curve=curve, calendar=cal)
        mp = em.refresh(markets, F, sig, now, force=True)
        for tk, r in mp.items():
            L.append(f"em {si}\t{tk} {on(r.get('delta'))} {on(r.get('vega'))} {on(r.get('theta'))}")
        em_sets.append(mp)
    # a synthetic set that exercises what the real one does not: None Greeks, huge vegas
    synth = {}
    for tk in tickers:
        synth[tk] = {"delta": rnd.choice((None, rnd.uniform(-0.2, 0.2))),
                     "vega": rnd.choice((None, rnd.uniform(-5, 5), rnd.uniform(-1e3, 1e3))),
                     "theta": rnd.choice((None, rnd.uniform(-1, 1)))}
        r = synth[tk]
        L.append(f"em 3\t{tk} {on(r['delta'])} {on(r['vega'])} {on(r['theta'])}")
    em_sets.append(synth)

    fitted = skewmod.fit(board, strikes, F, w_atm * 1.02)
    pricers = {
        "lazy": lambda: theo.Pricer(board, strikes, spot=F),
        "lazyoff": lambda: theo.Pricer(board, strikes, spot=F + 11.5),
        "inst": lambda: _with(theo.Pricer(board, strikes, spot=F), fitted),
        "flat": lambda: _with(theo.Pricer(board, strikes, spot=F),
                              skewmod.SkewFit(F, w_atm, None, [], "too_few_smoothed_buckets")),
        "flatnone": lambda: _with(theo.Pricer(board, strikes, spot=F),
                                  skewmod.SkewFit(F, None, None, [], "no_atm_fit")),
        "none": lambda: None,
    }
    L.append(f"fit {h(F)} {h(w_atm * 1.02)} {h(w_atm)}")

    with tempfile.TemporaryDirectory() as td:
        real_dir = jnl.JOURNAL_DIR
        jnl.JOURNAL_DIR = pathlib.Path(td) / "var" / "journal"
        try:
            j = jnl.open_journal("20260821T143000Z", name="risk.jsonl")
            for interval in (None, 2.5):
                tape = rt.RiskTape(j, interval_s=interval)
                L.append(f"tape {on(interval)}")
                mono = 100.0
                for k in range(160):
                    mono += rnd.choice((0.5, 1.0, 1.0, 2.0, 2.5, 0.999999))
                    for probe in (mono - 1.0, mono, mono + 0.5):
                        L.append(f"due {h(probe)} {1 if tape.due(probe) else 0}")
                    now = dt.datetime.combine(D, dt.time(9, 25), tzinfo=config.ET) + dt.timedelta(
                        microseconds=rnd.randrange(0, 7 * 3600 * 10**6))
                    if k % 23 == 0:
                        now = now.replace(hour=16, minute=rnd.choice((0, 5)), second=0,
                                          microsecond=0)
                    Clk.ts_ns = ns_of(now) + rnd.randrange(0, 10**6)
                    pmode = rnd.choice(list(pricers))
                    pr = pricers[pmode]()
                    use_cal = rnd.random() > 0.1
                    sigma_impl = rnd.choice((None, 0.0, rnd.uniform(0.05, 0.4), 0.13))
                    sigma_rv = rnd.choice((None, rnd.uniform(0.05, 0.4)))
                    spot = rnd.choice((None, F, F + rnd.gauss(0, 2)))
                    fast = rnd.choice((None, F + rnd.gauss(0, 1)))
                    es = rnd.randrange(0, len(em_sets))
                    use_h = rnd.random() > 0.15
                    pos = {}
                    for _ in range(rnd.randrange(0, 40)):
                        tk = rnd.choice(tickers + ["KXNASDAQ100-OTHER", "KXINX-GONE"])
                        v = rnd.choice((None, 0.0, -0.0, float(rnd.randint(-60, 60)),
                                        float(rnd.randint(-60, 60)), rnd.uniform(-100, 100)))
                        pos[tk] = float("nan") if rnd.random() < 0.004 else v
                    write = rnd.random() > 0.1
                    before = j.path.stat().st_size if j.path.exists() else 0
                    row = tape.snapshot("SPX", now, pos, em_sets[es], markets, pr,
                                        cal if use_cal else None, sigma_impl=sigma_impl,
                                        sigma_rv=sigma_rv, spot=spot, fast_spot=fast,
                                        hourly_markets=(hourly if use_h else ()),
                                        now_mono=mono, write=write)
                    j._fh.flush()
                    line = j.path.read_bytes()[before:].decode("ascii").rstrip("\n")
                    L.append(f"snap SPX {ns_of(now)} {Clk.ts_ns} {h(mono)} {1 if write else 0} "
                             f"{pmode} {1 if use_cal else 0} {on(sigma_impl)} {on(sigma_rv)} "
                             f"{on(spot)} {on(fast)} {es} {1 if use_h else 0} {len(pos)} "
                             + " ".join(f"{tk}={on(v)}" for tk, v in pos.items())
                             + f"\t{json.dumps(row, separators=(',', ':'))}\t{line or '-'}")
                L.append(f"det\t{json.dumps(tape.detail(), separators=(',', ':'))}")
            j.close()
        finally:
            jnl.JOURNAL_DIR = real_dir

    # the pure helpers
    for _ in range(3000):
        K = rnd.choice((F, F * rnd.uniform(0.9, 1.1), 0.0, -5.0, None))
        Fq = rnd.choice((F, 0.0, None, F * 1.001))
        mode = rnd.choice(("fit", "flat", "flatnone", "none"))
        sf = {"fit": fitted, "flat": skewmod.SkewFit(F, w_atm, None, [], "x"),
              "flatnone": skewmod.SkewFit(F, None, None, [], "x"), "none": None}[mode]
        wa = rnd.choice((None, 0.0, w_atm, -0.001, w_atm * 3))
        L.append(f"vcd {on(Fq)} {on(K)} {mode} {on(wa)} {on(rt.vanilla_call_delta(Fq, K, sf, w_atm=wa))}")
    for d in [None, -1.0, -0.0, 0.0, 1.0, 1.5, float("nan"), float("inf")] + \
             [i / 10 for i in range(11)] + [rnd.uniform(-0.2, 1.2) for _ in range(500)] + \
             [math.nextafter(i / 10, 1) for i in range(11)] + [math.nextafter(i / 10, -1) for i in range(11)]:
        b = rt.bucket_of(d)
        L.append(f"bkt {on(d)} {'None' if b is None else b}")
    for m in markets[:40] + [{"ticker": "X", "floor_strike": None, "cap_strike": None}]:
        L.append(f"rs {m['ticker']} {on(rt.reference_strike(m))}")
    emit(out, "risktape", "strategy/risktape.py", L)


_LAST = [None]


def _with(p, fit):
    p.use_skew(fit)
    return p


def _last():
    return _LAST[0]


if __name__ == "__main__":
    main()
