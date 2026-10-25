#!/usr/bin/env python3
"""Drive the Python reference's run_04.cycle over a scripted session and write its trace.

    python tools/replay/py_replay.py SESSION.json TRACE.jsonl

The reference runs UNMODIFIED: every value it would read from outside -- clocks, sockets, REST,
the exchange -- is replaced by the session's scripted value at the module attribute it is read
through, and its decisions are captured by wrapping (never replacing) build_desired and
orders.apply. apps/replay does the same to the C++ port and writes the same trace; any difference
between the two traces is a difference between the two implementations.

Nothing here touches the network or the reference tree: the journal and risk tape write to a
temporary directory.
"""
import contextlib, datetime as _dt, io, json, os, pathlib, sys, tempfile, time as _time, types, uuid as _uuid

HERE = pathlib.Path(__file__).resolve().parent
REF = pathlib.Path(os.environ.get("KP_REF", HERE.parent.parent / "ref" / "current")).resolve()
sys.path.insert(0, str(REF))
import config  # noqa: E402
from kalshi import client as k  # noqa: E402
from kalshi.client import KalshiError  # noqa: E402
from massive import index as ix  # noqa: E402
from massive import futures as fut  # noqa: E402
from pricing import basis as basismod  # noqa: E402
from strategy import deltaadj, edgemgr, journal as jnl, orders as omgr  # noqa: E402
from strategy import leanchain, risklimit, risktape, skewstate  # noqa: E402
from massive import history as mhist  # noqa: E402
from rvol import live as rlive  # noqa: E402
from strategy import run_04_quote_loop as r4  # noqa: E402

UTC = _dt.timezone.utc


class Clock:
    ns = 0
    mono = 0.0


CLK = Clock()


def _now_dt(tz=None):
    t = _dt.datetime(1970, 1, 1, tzinfo=UTC) + _dt.timedelta(microseconds=CLK.ns // 1000)
    return t.astimezone(tz) if tz is not None else t.replace(tzinfo=None)


class _DT(_dt.datetime):
    @classmethod
    def now(cls, tz=None):
        return _now_dt(tz)


shim = types.SimpleNamespace(**{k_: getattr(_dt, k_) for k_ in dir(_dt) if not k_.startswith("__")})
shim.datetime = _DT
r4.datetime = shim
ix.datetime = shim
risklimit.datetime = shim
rlive.datetime = shim

_time.monotonic = lambda: CLK.mono
_time.time = lambda: CLK.ns / 1e9
_time.time_ns = lambda: CLK.ns
_time.perf_counter = lambda: 0.0

_ids = {"n": 0}


def _uuid4():
    _ids["n"] += 1
    return f"00000000-0000-4000-8000-{_ids['n']:012d}"


omgr.uuid = types.SimpleNamespace(uuid4=_uuid4)


def main():
    sess = json.loads(pathlib.Path(sys.argv[1]).read_text())
    out = open(sys.argv[2], "w")
    tmp = pathlib.Path(tempfile.mkdtemp(prefix="pyreplay-"))
    jnl.JOURNAL_DIR = tmp
    config.RISK_STATE_DIR = tmp / "risk"

    # the write bucket, on the scripted monotonic clock
    k.write_bucket = k.TokenBucket(sess["bucket"]["capacity"], sess["bucket"]["refill"])

    cyc = {"c": None}

    # --- discovery / index / REST books ---------------------------------------------------
    by_series = {}
    for u, d in sess["discover"].items():
        for m in d["daily"]:
            by_series.setdefault(("d", m["event_ticker"].split("-")[0]), []).append(m)
        for m in d["hourly"]:
            by_series.setdefault(("h", m["event_ticker"].split("-")[0]), []).append(m)

    def discover_boards(daily=None, hourly=None, date=None):
        dm = [dict(m) for s in daily for m in by_series.get(("d", s), [])]
        hm = [dict(m) for s in hourly for m in by_series.get(("h", s), [])]
        return dm, hm

    r4.discover_boards = discover_boards

    def iq_of(u, d):
        return ix.IndexQuote(u, d["ticker"], d["value"], d["ts_ns"], d.get("market_status"), d.get("name"))

    r4._index_quotes = lambda us=None, feed=None: {u: iq_of(u, d) for u, d in cyc["c"]["index"].items()
                                                   if u in (us or ())}
    k.orderbooks = lambda tickers, depth=None: {t: cyc["c"]["books"][t] for t in tickers
                                                if t in cyc["c"]["books"]}

    # --- the exchange (live scenario only) ------------------------------------------------
    def batch_create(orders):
        mode = cyc["c"].get("api", "ok")
        if mode == "http429":
            raise KalshiError(429, '{"error":{"code":"too_many_requests"}}')
        res = []
        for i, o in enumerate(orders):
            if mode == "partial" and i % 3 == 2:
                res.append({"error": {"code": "insufficient_balance", "message": "no"}})
            else:
                res.append({"order_id": "oid-" + o["client_order_id"]})
        return res

    def batch_cancel(ids):
        mode = cyc["c"].get("api", "ok")
        if mode == "http429":
            raise KalshiError(429, '{"error":{"code":"too_many_requests"}}')
        res = []
        for i, oid in enumerate(ids):
            if mode == "not_found" and i % 2 == 0:
                res.append({"order_id": oid, "error": {"code": "not_found"}})
            else:
                res.append({"order_id": oid, "reduced_by": "20.00"})
        return res

    k.batch_create_orders = batch_create
    k.batch_cancel_orders = batch_cancel

    # --- fakes for the three sockets --------------------------------------------------------
    class Collector:
        reconnects = 0

        def health(self):
            c = cyc["c"]["col"]
            return {"alive": c["alive"], "pump_error": c["pump_error"], "quote_age_s": c["quote_age_s"]}

        def reconnect(self):
            self.reconnects += 1
            return cyc["c"]["reconnect"]

        def snapshot(self):
            snap = {}
            for sym, (bp, ap, bsz, asz, t) in cyc["c"]["snap"].items():
                q = {"ev": "Q", "sym": sym}
                if bp is not None:
                    q["bp"] = bp
                q["ap"] = ap
                if bsz is not None:
                    q["bs"] = bsz
                q["as"] = asz
                q["t"] = t
                snap[sym] = q
            return snap

        def recenter(self, want):
            r = cyc["c"]["recenter"]
            if isinstance(r, str) and r.startswith("error:"):
                raise type(r.split(":")[1], (Exception,), {})(r.split(":", 2)[2])
            return None if r is None else tuple(r)

    class MdFeed:
        @property
        def pump_error(self):
            return cyc["c"]["pump_error"]

        @property
        def alive(self):
            return cyc["c"]["feed_alive"]

        def drain_fills(self):
            return [dict(f) for f in cyc["c"]["fills"]] + cyc.get("picked", [])

        def health(self):
            return {"coverage": cyc["c"]["coverage"], "pump_error": self.pump_error}

        def books(self, tickers):
            b = cyc["c"]["books"]
            return {t: b[t] for t in tickers if t in b}

        def positions(self):
            return dict(cyc["c"]["positions"])

        def entry(self, t):
            return cyc["c"]["books"].get(t)

    class Acct:
        def latest(self):
            a = cyc["c"].get("acct")
            return None if a is None else (a[0], a[1], dict(a[2]))

    class Futures:
        def mid(self, u):
            # first read per underlying per cycle = "fut", every later one = "fut2" (see make_session)
            n = cyc["fut_reads"].get(u, 0)
            cyc["fut_reads"][u] = n + 1
            v = cyc["c"]["fut" if n == 0 else "fut2"].get(u)
            return (v[0], v[1]) if v else (None, None)

        def move_scale(self, u, window, sigmas):
            return cyc["c"]["move_scale"].get(u)

        def health(self):
            return None

    # --- the boards --------------------------------------------------------------------------
    boards = []
    for bj in sess["boards"]:
        b = r4.Board(bj["underlying"], bj["daily_series"], bj["hourly_series"])
        b.root, b.expiry, b.strikes, b.tickers = bj["root"], bj["expiry"], bj["strikes"], bj["tickers"]
        b.em = edgemgr.EdgeManager(b.underlying, enabled=True)
        b.lc = None
        if sess.get("lean"):
            ld = sess["lean"][b.underlying]
            ix.closes_ms = (lambda u, d, _c=ld["closes"]: [tuple(r) for r in _c])
            mhist.second_bars = (lambda *a_, _p=ld["proxy"], **kw: [dict(x) for x in _p])
            b.lc = leanchain.LeanChain(b.underlying)
            CLK.ns, CLK.mono = sess["cycles"][0]["now_ns"], sess["cycles"][0]["mono"]
            b.lc.start()
        b.sk = skewstate.SkewState(b.underlying) if sess["smooth_skew"] else None
        if sess["delta_adj"]:
            ft = deltaadj.FuturesTicker(b.underlying)
            ft.ticker = {"SPX": "ESU6", "NDX": "NQU6"}[b.underlying]
            b.da = deltaadj.DeltaAdjuster(ft)
        b.trk = basismod.BasisTracker(b.underlying) if sess["has_futures"] else None
        boards.append(b)
    edges = r4.EdgeUnion(boards)

    book = omgr.RestingBook()
    jrnl = jnl.open_journal("replay", name="quotes.jsonl")
    rjrnl = jnl.open_journal("replay", name="risk.jsonl")
    rtape = risktape.RiskTape(rjrnl)
    halt = tmp / "HALT"
    risk = risklimit.SessionRisk(halt_path=halt)
    col = Collector()
    state = {"tickers": list(sess["state_tickers"]), "max_batch": sess["max_batch"], "risk": risk,
             "acct": Acct(),
             "mdfeed": MdFeed() if sess["has_mdfeed"] else None, "tee": None,
             "futures": Futures() if sess["has_futures"] else None, "indexfeed": None,
             "spot_tape": None, "guard": None, "risktape": rtape, "apply_lock": __import__("threading").RLock()}
    args = types.SimpleNamespace(live=sess["live"], tiers=sess["tiers"],
                                 no_reprice_at_send=sess["no_reprice_at_send"])

    cap = {}
    real_bd, real_apply = r4.build_desired, omgr.apply

    def bd_wrap(markets, fairs, books, em, lean_map, tiers_enabled, bands, positions=None, book=None,
                mono_now=None, now=None, **extra):
        # **extra passes through keywords newer references add (e.g. spike_map) unchanged.
        desired, notes = real_bd(markets, fairs, books, em, lean_map, tiers_enabled, bands,
                                 positions=positions, book=book, mono_now=mono_now, now=now, **extra)
        cap["chosen"] = sorted(m["ticker"] for m in markets)
        cap["fairs"] = {t: [d.get("fair"), d.get("fair_vol"), d.get("delta_adj"), d.get("f_move"),
                            d.get("fair_at_price")] for t, d in fairs.items()}
        cap["desired"] = [[tk, side, [[r.index, r.price, r.count, r.edge, r.reason, r.min_edge]
                                      for r in rungs]] for (tk, side), rungs in desired.items()]
        cap["notes"] = notes
        return desired, notes

    def apply_wrap(place, cancel, book_, live=False, max_batch=None, budget=None, now=None):
        cap["place"] = [dict(p) for p in place]
        cap["cancel"] = [dict(c) for c in cancel]
        res = real_apply(place, cancel, book_, live=live, max_batch=max_batch, budget=budget, now=now)
        cap["apply"] = res
        return res

    r4.build_desired = bd_wrap
    omgr.apply = apply_wrap

    jpath, rpath = pathlib.Path(jrnl.path), pathlib.Path(rjrnl.path)
    jseen = rseen = 0
    for i, c in enumerate(sess["cycles"]):
        cyc["c"] = c
        cyc["fut_reads"] = {}
        CLK.ns, CLK.mono = c["now_ns"], c["mono"]
        cap.clear()
        # the fills the live scenario takes off the resting book, before the cycle drains them
        cyc["picked"] = []
        orders_now = book.all_orders()
        for kk, count, px in c.get("fill_picks", []):
            if orders_now:
                o = orders_now[kk % len(orders_now)]
                cyc["picked"].append({"market_ticker": o["ticker"], "order_id": o.get("order_id"),
                                      "count_fp": count, "yes_price_dollars": px,
                                      "side": "yes" if o["side"] == "bid" else "no",
                                      "outcome_side": "yes" if o["side"] == "bid" else "no",
                                      "fee_cost": "0.0000"})
        # the basis thread's folds since the last cycle, and the futures poll
        for u, fmid, iq, mono in c.get("basis_obs", []):
            b = next(b for b in boards if b.underlying == u)
            CLK.mono = mono
            b.trk.observe(fmid, iq_of(u, iq))
        CLK.mono = c["mono"]
        for b in boards:
            if b.da is not None:
                row = c["ft_rows"].get(b.underlying)
                fut._get = (lambda *a_, _r=row, **kw: {"results": [] if _r is None else [_r]})
                t = b.da.ticker
                t._stop = types.SimpleNamespace(is_set=iter([False, True]).__next__,
                                                wait=lambda s: None)
                t._poll()
        err = io.StringIO()
        with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
            try:
                rec = r4.cycle(state, col, boards, edges, book, args, jrnl)
            except Exception as e:                                   # noqa: BLE001
                rec = {"error": type(e).__name__, "msg": str(e)[:200]}
        jrnl._fh.flush()
        rjrnl._fh.flush()
        jl = jpath.read_text().splitlines()
        rl = rpath.read_text().splitlines()
        rec.pop("ms", None)
        rec.pop("reprice_ms", None)
        tr = {"i": i, "rec": rec, "stderr": err.getvalue().splitlines(),
              "book": [o for o in book.all_orders()], "risk": risk.summary(),
              "journal": jl[jseen:], "risktape": rl[rseen:], **cap}
        jseen, rseen = len(jl), len(rl)
        out.write(json.dumps(tr, default=str) + "\n")
    out.close()


if __name__ == "__main__":
    main()
