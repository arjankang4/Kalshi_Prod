#!/usr/bin/env python3
"""massive/* -- every Massive client the live quote loop reaches -- replayed against canned data.

Nothing here touches the network. The reference's transports are replaced underneath it:

  * `urllib.request.urlopen` returns canned (status, body) pairs in order and records every URL it
    is asked for, so the C++ is checked on the EXACT request sequence (query encoding, apiKey
    placement, paging, bisection order, retry count), not only on what it parsed;
  * `websocket.create_connection` hands out scripted fake sockets (frames, timeouts, errors, and a
    `yield` marker that pauses the pump so the state can be inspected mid-stream);
  * `time.monotonic` / `time.time_ns` / `time.sleep` are one scripted clock, so rate-limiter waits,
    retry backoffs, quote ages and `silent_s` are exact numbers rather than wall-clock noise;
  * `threading.Thread` / `threading.Event` are inert fakes, so each pump runs synchronously on this
    thread and a pump that would have died on an exception is observed dying.

Every case is ONE line, `<suite> <hex(json)>`, carrying its inputs AND the reference's outputs; the
C++ replays the inputs through the same scripted transports and must reproduce the outputs as
json.dumps-identical text. Floats therefore compare by bits (shortest repr round-trips).

THE ARITHMETIC THAT IS PRICING-RELEVANT is `FuturesFeed.move_scale` -- it gates quoting through the
edge manager's adverse-selection check -- and it is swept hard: 3.12's compensated `sum()` and
libm `pow(x, 2)` (which is NOT x*x on ~0.085% of doubles) are both load-bearing, so the sweep feeds
off-grid mids where every square rounds, not only the 0.25-tick grid on which every term is exact
and a naive fold would pass by accident.

Suites (one .vec each, each hashed against its own source):
    massive_client   massive/client.py     QuoteCollector frames/recenter/reconnect + REST idiom
    massive_history  massive/history.py    RateLimiter, get_url retries, /v2/aggs paging, pm root
    massive_index    massive/index.py      snapshot(), closes_ms()
    massive_futures  massive/futures.py    _get, aggs bisection + window guard, session_volume
    massive_streams  massive/streams.py    FuturesFeed/IndexFeed frames, mid, move_scale, tee
    massive_universe pricing/universe.py   option_root_and_strikes
"""
import argparse, copy, io, json, pathlib, random, sys, time, types
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
from gen_golden import emit, config, REF  # noqa: E402

import threading  # noqa: E402
import urllib.error  # noqa: E402
import urllib.request  # noqa: E402

import websocket  # noqa: E402

from massive import client, futures as fut, history, index as ix, streams  # noqa: E402
from pricing import universe  # noqa: E402

KEY = "TESTKEY"
config.MASSIVE_API_KEY = KEY


# --- the scripted clock ------------------------------------------------------------------------

class C:
    mono = 1000.0
    mono_step = 0.0
    ns = 1_787_327_400_000_000_000
    ns_step = 0
    sleeps = []


def f_mono():
    v = C.mono
    C.mono += C.mono_step
    return v


def f_ns():
    v = C.ns
    C.ns += C.ns_step
    return v


def f_sleep(s):
    C.sleeps.append(s)
    C.mono += s


time.monotonic = f_mono
time.time_ns = f_ns
time.sleep = f_sleep


def hexj(o):
    return json.dumps(o).encode().hex()


# --- REST --------------------------------------------------------------------------------------

RESP, URLS = [], []


class FakeResp:
    def __init__(self, body):
        self.body = body

    def read(self):
        return self.body

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False


def fake_urlopen(url, timeout=None):
    URLS.append(url)
    if not RESP:
        raise AssertionError(f"no canned response for {url}")
    kind, status, body = RESP.pop(0)
    if kind == "err":
        raise urllib.error.URLError("canned transport failure")
    if 200 <= status < 300:
        return FakeResp(body.encode())
    raise urllib.error.HTTPError(url, status, "canned", {}, io.BytesIO(body.encode()))


urllib.request.urlopen = fake_urlopen


def label(e):
    if isinstance(e, client.MassiveError):
        return "MassiveError"
    if isinstance(e, history.MassiveHistoryError):
        return "MassiveHistoryError"
    if isinstance(e, json.JSONDecodeError):
        return "error"
    if isinstance(e, ValueError):
        return "ValueError"
    if isinstance(e, KeyError):
        return "KeyError"
    return "error"


def iq_out(d):
    return {u: [q.value, q.ts_ns, q.ticker, q.market_status, q.name] for u, q in sorted(d.items())}


FNS = {
    "option_contracts": lambda u, e: client.option_contracts(underlying=u, expiration_date=e),
    "root_strikes": lambda e, u: (lambda r: [r[0], [float(k) for k in r[1]], r[2]])(
        universe.option_root_and_strikes(expiry_date=e, underlying=u)),
    "rest": lambda path, params: client.rest(path, **params),
    "get_url": lambda url: history.get_url(url),
    "hist_get": lambda path, params: history.get(path, **params),
    "index_snapshot": lambda us: iq_out(ix.snapshot(tuple(us))),
    "closes_ms": lambda u, d: [[t, float(c)] for t, c in ix.closes_ms(u, d)],
    "second_bars": lambda tk, a, b: history.second_bars(tk, a, b),
    "fut_get": lambda path, params: fut._get(path, **params),
    "quote_page": lambda c: fut._get(f"/futures/v1/quotes/{c}", limit=1),
    "aggs": lambda tk, res, a, b, lim: fut.aggs(tk, res, a, b, limit=lim),
    "session_volume": lambda tk, d: float(fut.session_volume(
        tk, __import__("datetime").date.fromisoformat(d))),
    "pm_settled_root": lambda cs: history.pm_settled_root(cs),
}


SEQ = [0]


def rest_case(L, fn, args, resps, msg=True):
    RESP[:] = [list(r) for r in resps]
    URLS.clear()
    C.sleeps.clear()
    out = {"result": None, "raised": None, "msg": None}
    try:
        out["result"] = FNS[fn](*args)
    except Exception as e:                                        # noqa: BLE001
        out["raised"] = label(e)
        if msg and out["raised"] != "error":
            out["msg"] = str(e)
    out["urls"] = list(URLS)
    out["sleeps"] = list(C.sleeps)
    out["left"] = len(RESP)
    # `seq` is the global call order: the module-level rate limiters carry state from one case to
    # the next, so the C++ must replay the cases in exactly this order across all five suites.
    SEQ[0] += 1
    L.append("rest " + hexj({"seq": SEQ[0], "fn": fn, "args": args, "resps": resps, "expect": out}))


def ok(o):
    return ["ok", 200, json.dumps(o)]


def http(code, body):
    return ["http", code, body]


ERR = ["err", 0, ""]
BASE = config.MASSIVE_REST_BASE


def contract(root, exp, right, k, und):
    yymmdd = exp[2:4] + exp[5:7] + exp[8:10]
    return {"cfi": "OCEICS", "contract_type": "call" if right == "C" else "put",
            "exercise_style": "european", "expiration_date": exp, "primary_exchange": "BATO",
            "shares_per_contract": 100, "strike_price": k,
            "ticker": f"O:{root}{yymmdd}{right}{round(k * 1000):08d}", "underlying_ticker": und}


def board(roots, exp, und, strikes):
    return [contract(r, exp, right, k, und) for r in roots for k in strikes for right in "CP"]


def pages(items, n, url_base):
    """Split into vendor pages of n, each but the last carrying a next_url cursor."""
    out = []
    for i in range(0, len(items), n):
        p = {"results": items[i:i + n], "status": "OK", "request_id": f"r{i}"}
        if i + n < len(items):
            p["next_url"] = f"{url_base}?cursor=YXA9{i + n}JmxpbWl0PTEwMDA"
        out.append(ok(p))
    return out


def gen_rest(rnd):
    Lc, Lh, Li, Lf, Lu = [], [], [], [], []
    # Module-level limiters start fresh, on the scripted clock, exactly as the C++ Rest does.
    history._limiter = history.RateLimiter()
    history._agg_limiter = history.RateLimiter(per_sec=history.AGG_RATE_PER_SEC)
    fut._limiter = history.RateLimiter(per_sec=fut.FUTURES_RATE_PER_SEC)
    C.mono = 1000.0

    # ---- client.rest / option_contracts --------------------------------------------------------
    spx = [7000 + 5 * i for i in range(60)] + [7300.5, 7600, 8000.0]
    exp = "2026-08-21"
    cu = f"{BASE}/v3/reference/options/contracts"
    cs = board(["SPXW"], exp, "SPX", spx)
    rest_case(Lc, "option_contracts", ["SPX", exp], pages(cs, 50, cu))
    rest_case(Lc, "option_contracts", ["NDX", None], pages(board(["NDXP"], exp, "NDX", [29000, 29100]), 3, cu))
    rest_case(Lc, "option_contracts", ["SPX", exp], [ok({"results": None, "status": "OK"})])
    rest_case(Lc, "option_contracts", ["SPX", exp], [ok({"status": "OK", "next_url": ""})])
    rest_case(Lc, "option_contracts", ["SPX", exp], [http(404, "x" * 400)])
    rest_case(Lc, "option_contracts", ["SPX", exp], [http(403, '{"status":"NOT_AUTHORIZED","message":"no"}')])
    rest_case(Lc, "option_contracts", ["SPX", exp], pages(cs, 50, cu)[:1] + [http(500, "boom")])
    rest_case(Lc, "option_contracts", ["SPX", exp], [ERR])
    rest_case(Lc, "option_contracts", ["SPX", exp], [["ok", 200, "not json"]])
    rest_case(Lc, "rest", ["/v2/last/nbbo/SPY", {"a b": "c&d", "x": None, "n": 5, "u": "é/?"}],
              [ok({"results": {"p": 1.5, "P": 1.75}})])

    # ---- universe.option_root_and_strikes ------------------------------------------------------
    opex = "2026-09-18"
    mix = board(["SPX", "SPXW"], opex, "SPX", [7600, 7605, 7610.0, 7612.5])
    rnd.shuffle(mix)
    rest_case(Lu, "root_strikes", [opex, "SPX"], pages(mix, 5, cu))
    ndx = board(["NDX", "NDXP"], opex, "NDX", [29000, 29010, 29020, 29050, 30000])
    rest_case(Lu, "root_strikes", [opex, "NDX"], pages(ndx, 7, cu))
    rest_case(Lu, "root_strikes", [exp, "SPX"], pages(cs, 1000, cu))
    rest_case(Lu, "root_strikes", [exp, "SPX"], [ok({"results": [], "status": "OK"})])
    rest_case(Lu, "root_strikes", [exp, "RUT"], [ok({"results": board(["RUTW"], exp, "RUT", [2000, 2010])})])
    rest_case(Lu, "root_strikes", [exp, "XYZ"], [ok({"results": board(["AAA", "BBB"], exp, "XYZ", [10])})], msg=False)
    dup = board(["SPXW"], exp, "SPX", [7700, 7700.0, 7705])
    rest_case(Lu, "root_strikes", [exp, "SPX"], [ok({"results": dup})])

    # ---- history.get_url: the retry ladder --------------------------------------------------------
    u = f"{BASE}/v3/quotes/O:SPXW260821C07700000?limit=1"
    for seq in ([ok({"a": 1})], [http(500, "e"), http(502, "e"), ok({"b": 2})],
                [http(429, "slow")] * 5, [http(429, "slow")] * 4 + [ok({"c": 3})],
                [http(404, "gone" * 80)], [http(401, "no")], [ERR, ok({"d": 4})],
                [ERR] * 5, [ERR, http(503, "x"), http(504, "y"), ERR, http(429, "z")],
                [["ok", 200, "{bad"]]):
        rest_case(Lh, "get_url", [u], seq, msg=not any(r[0] == "err" for r in seq))
    rest_case(Lh, "get_url", [f"{BASE}/v2/aggs/x"], [ok({"e": 5})])
    rest_case(Lh, "hist_get", ["/v3/snapshot/indices", {"ticker.any_of": "I:NDX,I:SPX", "z": None}],
              [ok({"results": []})])

    # ---- history: /v2/aggs paging, second_bars ----------------------------------------------------
    t0 = 1787318400000
    bars = [{"t": t0 + 1000 * i, "o": 1.0, "h": 1.0, "l": 1.0, "c": 7700 + 0.01 * i, "v": 0, "vw": 0}
            for i in range(40)]
    agg_u = f"{BASE}/v2/aggs/ticker/SPY/range/1/second/2026-08-20/2026-08-20"
    p1 = {"results": bars[:15], "next_url": agg_u + "?cursor=a"}
    p2 = {"results": bars[14:30] + [bars[3]], "next_url": agg_u + "?cursor=b"}      # repeats: dedup
    p3 = {"results": list(reversed(bars[30:])), "next_url": agg_u + "?cursor=c"}   # unsorted
    p4 = {"results": [], "next_url": agg_u + "?cursor=d"}                           # empty ends it
    rest_case(Lh, "second_bars", ["SPY", "2026-08-20", "2026-08-20"], [ok(p1), ok(p2), ok(p3), ok(p4)])
    rest_case(Lh, "second_bars", ["SPY", "2026-08-20", "2026-08-20"],
              [ok({"results": bars[:5]}), ok({"status": "ERROR", "error": "nope"})])
    rest_case(Lh, "second_bars", ["QQQ", "2026-08-20", "2026-08-21"],
              [ok({"results": bars[:5], "next_url": None})])
    rest_case(Lh, "second_bars", ["QQQ", "2026-08-20", "2026-08-21"],
              [ok({"status": "ERROR", "error": None, "message": "m" * 300})])
    rest_case(Lh, "second_bars", ["QQQ", "2026-08-20", "2026-08-21"], [ok({"status": "ERROR"})])

    # ---- history.pm_settled_root (pure) -----------------------------------------------------------
    for roots in (["SPX", "SPXW"], ["NDX", "NDXP"], ["SPXW"], ["RUTW"], [], ["NDXP", "SPXW"]):
        rest_case(Lh, "pm_settled_root", [board(roots, exp, "X", [100, 200])], [])
    rest_case(Lh, "pm_settled_root", [board(["AAA", "BBB"], exp, "X", [1])], [], msg=False)
    rest_case(Lh, "pm_settled_root", [[{"ticker": "O:X"}, {"ticker": "short"}]], [])

    # ---- index.snapshot / closes_ms -------------------------------------------------------------
    snap_rows = [{"ticker": "I:SPX", "value": 7691.23, "last_updated": 1787327428284000000,
                  "market_status": "open", "name": "Standard & Poor's 500", "type": "indices",
                  "session": {"change": 1.5}, "timeframe": "REAL-TIME"},
                 {"ticker": "I:NDX", "value": 29301, "last_updated": 1787327428001000000,
                  "market_status": "open", "name": "Nasdaq 100"}]
    for us, rows in ((["SPX", "NDX"], snap_rows), (["NDX"], snap_rows[1:]), (["SPX"], []),
                     (["SPX", "NDX"], [dict(snap_rows[0], value=None), snap_rows[1]]),
                     (["SPX", "SPX", "NDX", "FOO"], snap_rows + [dict(snap_rows[0], value=7700.5)]),
                     (["NDX", "SPX"], [dict(snap_rows[0], last_updated=None, market_status=None),
                                       {"ticker": "I:NDX"}, {}]),
                     (["SPX"], [{"ticker": "I:SPX", "value": 7000}])):
        rest_case(Li, "index_snapshot", [us], [ok({"results": rows, "status": "OK"})])
    rest_case(Li, "index_snapshot", [["FOO"]], [])
    rest_case(Li, "index_snapshot", [[]], [])
    rest_case(Li, "index_snapshot", [["SPX"]], [ok({"status": "OK"})])
    rest_case(Li, "index_snapshot", [["SPX", "NDX"]], [http(403, '{"status":"NOT_AUTHORIZED"}')])
    rest_case(Li, "index_snapshot", [["SPX"]], [http(502, "x")] * 5, msg=True)
    cbars = [{"t": t0 + 1000 * i, "c": c} for i, c in enumerate(
        [7700.25, 7700.5, 0, -1, None, 7701, 7700.75, 7702.125])] + [{"t": t0 + 99_000}]
    rest_case(Li, "closes_ms", ["SPX", "2026-08-20"], [ok({"results": cbars[:5], "next_url": "u?x"}),
                                                       ok({"results": cbars[5:]}), ok({"results": []})])
    rest_case(Li, "closes_ms", ["NDX", "2026-08-21"], [ok({"results": []})])
    rest_case(Li, "closes_ms", ["FOO", "2026-08-21"], [])

    # ---- futures: _get, quote page, aggs bisection + window guard, session_volume ----------------
    q = {"results": [{"ticker": "ESZ6", "bid_price": 6650.25, "ask_price": 6650.5, "bid_size": 12,
                      "ask_size": 9, "timestamp": 1787327428284123456}], "status": "OK"}
    rest_case(Lf, "quote_page", ["ESZ6"], [ok(q)])
    rest_case(Lf, "quote_page", ["NQZ6"], [ok({"status": "ERROR", "error": "not entitled"})])
    rest_case(Lf, "quote_page", ["NQZ6"], [http(403, '{"status":"ERROR"}')])
    rest_case(Lf, "fut_get", ["/futures/v1/contracts", {"product_code": "ES", "date": None, "limit": 10}],
              [ok({"results": []})])
    rest_case(Lf, "fut_get", ["/futures/v1/x", {}], [ok({"status": "ERROR", "message": {"k": 1}})])

    def fbars(start_ns, n, step_ns, vol=lambda i: 100 + i):
        return [{"ticker": "ESZ6", "window_start": start_ns + i * step_ns, "open": 1, "high": 1,
                 "low": 1, "close": 6650 + 0.25 * i, "volume": vol(i), "transactions": 3}
                for i in range(n)]

    def serve_aggs(all_bars, lim):
        """What the vendor would answer to each bisected request, in the reference's order."""
        out = []

        def walk(a, b):
            rows = [x for x in all_bars if a <= x["window_start"] < b][:lim]
            out.append(ok({"results": rows, "status": "OK", "next_url": "https://broken/cursor"}))
            if len(rows) >= lim and b - a > fut.MIN_SPLIT_NS:
                m = a + (b - a) // 2
                walk(a, m)
                walk(m, b)
        return out, walk

    s0 = 1787318400 * 10**9
    for n, lim, span in ((10, 50000, 3600), (130, 20, 3600), (64, 16, 1800), (5, 5, 600)):
        allb = fbars(s0, n, 25 * 10**9)
        out, walk = serve_aggs(allb, lim)
        walk(s0, s0 + span * 10**9)
        rest_case(Lf, "aggs", ["ESZ6", "1min", s0, s0 + span * 10**9, lim], out)
    rest_case(Lf, "aggs", ["ESZ6", "1min", s0, s0 + 60 * 10**9, 3],
              [ok({"results": fbars(s0, 3, 10**9)})])                     # cannot split further
    rest_case(Lf, "aggs", ["ESZ6", "1m", s0, s0 + 60 * 10**9, 3], [])        # rejected client-side
    rest_case(Lf, "aggs", ["ESZ6", "1sec", s0, s0 + 3600 * 10**9, 100],
              [ok({"results": fbars(s0 - 10**9, 4, 10**9)})])             # vendor ignored window
    rest_case(Lf, "aggs", ["ESZ6", "1sec", s0, s0 + 3600 * 10**9, 100],
              [ok({"results": fbars(s0 + 3598 * 10**9, 3, 10**9)})])      # a bar ON the open end
    rest_case(Lf, "aggs", ["ESZ6", "1sec", s0, s0 + 3600 * 10**9, 100],
              [ok({"results": fbars(s0, 1, 10**9) + fbars(s0 + 3599 * 10**9, 1, 10**9)})])
    rest_case(Lf, "aggs", ["ESZ6", "1second", s0, s0 + 3600 * 10**9, 100],
              [ok({"results": fbars(s0, 4, 10**9) + fbars(s0, 2, 10**9) + [{"window_start": None}]})])
    rest_case(Lf, "aggs", ["ESZ6", "1minute", s0, s0 + 3600 * 10**9, 100],
              [ok({"status": "ERROR", "error": "e" * 250})])
    for d, vol in (("2026-08-20", lambda i: 1000 + i), ("2026-08-21", lambda i: None if i % 3 else 7),
                   ("2026-11-02", lambda i: [5, 2.5, 1e16, 1.0, 1, 0.1][i % 6]),
                   ("2026-03-09", lambda i: 0.1 * i)):
        dd = __import__("datetime").date.fromisoformat(d)
        lo = fut._et_ns(dd, __import__("datetime").time(9, 30))
        rest_case(Lf, "session_volume", ["ESZ6", d], [ok({"results": fbars(lo, 12, 60 * 10**9, vol)})])
    return Lc, Lh, Li, Lf, Lu


# --- WebSockets, threads -------------------------------------------------------------------------

class FakeEvent:
    def __init__(self):
        self.flag = False

    def set(self):
        self.flag = True

    def clear(self):
        self.flag = False

    def is_set(self):
        return self.flag

    def wait(self, s=None):
        f_sleep(s)
        return self.flag


class FakeThread:
    def __init__(self, target=None, daemon=None):
        self.alive = False

    def start(self):
        self.alive = True

    def is_alive(self):
        return self.alive

    def join(self, timeout=None):
        pass


_fake_threading = types.SimpleNamespace(Thread=FakeThread, Event=FakeEvent, Lock=threading.Lock)
client.threading = _fake_threading
streams.threading = _fake_threading


class P:
    conns, sends = [], []
    opened = 0
    on_yield = None
    on_exhausted = None


class FakeWS:
    def __init__(self, idx, script):
        self.idx, self.script, self.pos = idx, script, 0

    def recv(self):
        if self.pos >= len(self.script):
            raise websocket.WebSocketConnectionClosedException("script exhausted")
        it = self.script[self.pos]
        self.pos += 1
        if isinstance(it, dict):
            return it["f"]
        if it == "timeout":
            raise websocket.WebSocketTimeoutException("timed out")
        if it == "yield":
            P.on_yield()
            raise websocket.WebSocketTimeoutException("yield")
        raise websocket.WebSocketConnectionClosedException("Connection to remote host was lost.")

    def send(self, s):
        P.sends.append([self.idx, s])

    def settimeout(self, t):
        pass

    def close(self):
        pass


def fake_create_connection(url, timeout=None, **kw):
    if not P.conns:
        if P.on_exhausted:
            P.on_exhausted()
        raise ConnectionRefusedError("no more connections")
    script = P.conns.pop(0)
    idx = P.opened
    P.opened += 1
    if script == "connfail":
        raise ConnectionRefusedError("connection refused")
    return FakeWS(idx, script)


websocket.create_connection = fake_create_connection


def reset(conns):
    C.mono, C.mono_step, C.ns, C.ns_step = 1000.0, 0.0, 1_787_327_400_000_000_000, 0
    C.sleeps.clear()
    P.conns = copy.deepcopy(conns)
    P.sends = []
    P.opened = 0
    P.on_yield = P.on_exhausted = None


def st_frame(status, message):
    return {"f": json.dumps([{"ev": "status", "status": status, "message": message}])}


CONNECTED = st_frame("connected", "Connected Successfully")
AUTH_OK = st_frame("auth_success", "authenticated")
MAXCONN = st_frame("max_connections", "Maximum number of websocket connections exceeded. You have "
                                       "reached the connection limit for your account.")
AUTH_FAIL = st_frame("auth_failed", "Your plan doesn't include websocket access. Visit "
                                    "https://massive.com/pricing to upgrade.")


def norm_sends(raw):
    out = []
    for idx, s in P.sends:
        if raw:
            out.append([idx, s])
            continue
        d = json.loads(s)
        if d.get("action") in ("subscribe", "unsubscribe"):
            d = {"action": d["action"], "params": sorted(d["params"].split(","))}
        out.append([idx, d])
    P.sends = []
    return out


def take_sleeps():
    out = list(C.sleeps)
    C.sleeps.clear()
    return out


# --- QuoteCollector --------------------------------------------------------------------------------

def col_op(col, st):
    op = st["op"]
    if op == "clock":
        C.mono, C.mono_step = st["t"], st.get("step", 0.0)
        return None
    if op == "start":
        try:
            col.start(st["attempts"], st["backoff"])
            return {"raised": None, "msg": None}
        except Exception as e:                                    # noqa: BLE001
            return {"raised": label(e), "msg": str(e) if isinstance(e, client.MassiveError) else None}
    if op == "pump":
        col._stop.clear()
        died = False
        try:
            col._pump()
        except Exception:                                         # noqa: BLE001
            died = True                   # the thread would have died with a traceback
        if not col._stop.is_set():
            col._thread.alive = False
        return {"died": died}
    if op == "state":
        return {"frame_count": col.frame_count, "msg_count": col.msg_count,
                "n_latest": len(col.latest), "status_log": len(col.status_log),
                "status_dropped": col.status_dropped, "last_msg_t": col.last_msg_t,
                "pump_error": col.pump_error is not None,
                "reconnect_failed": (col.pump_error or "").startswith("reconnect failed: "),
                "alive": col.alive, "reconnects": col.reconnects,
                "recenter_count": col.recenter_count, "tickers": list(col.tickers),
                "subscribed": sorted(col._subscribed)}
    if op == "snapshot":
        def f(x):
            return float(x) if isinstance(x, (int, float)) else None
        out = {}
        for k, q in col.snapshot().items():
            key = k if isinstance(k, str) else "\x00" + repr(k)
            t = q.get("t")
            out[key] = [f(q.get("bp")), f(q.get("ap")), f(q.get("bs")), f(q.get("as")),
                        t if isinstance(t, int) and not isinstance(t, bool) else None]
        return dict(sorted(out.items()))
    if op == "health":
        h = col.health(st.get("now"))
        return {"alive": h["alive"], "pump_error": h["pump_error"] is not None,
                "quote_age_s": h["quote_age_s"], "n_symbols": h["n_symbols"],
                "reconnects": h["reconnects"], "frames": h["frames"]}
    if op in ("recenter", "subscribe", "unsubscribe"):
        try:
            if op == "recenter":
                r = col.recenter(st["want"], st.get("force", False))
                r = None if r is None else list(r)
            else:
                r = getattr(col, op)(st["tickers"])
            return {"ret": r, "raised": None, "msg": None}
        except client.MassiveError as e:
            return {"ret": None, "raised": "MassiveError", "msg": str(e)}
        except Exception:                                         # noqa: BLE001
            return {"ret": None, "raised": "error", "msg": None}
    if op == "reconnect":
        okc = col.reconnect(st["attempts"], st["backoff"])
        return {"ok": okc, "pump_error": col.pump_error is not None,
                "reconnect_failed": (col.pump_error or "").startswith("reconnect failed: ")}
    if op == "wait":
        return {"ret": col.wait_for_coverage(st["min"], st["timeout"], st["poll"])}
    if op == "stop":
        col.stop()
        return None
    if op == "sends":
        return norm_sends(st.get("raw", False))
    if op == "sleeps":
        return take_sleeps()
    raise ValueError(op)


def run_col(sc):
    reset(sc["conns"])
    col = client.QuoteCollector(sc["tickers"])
    P.on_yield = lambda: col._stop.set()
    for st in sc["steps"]:
        st["expect"] = col_op(col, st)
    return "col " + hexj(sc)


def fixture_quotes():
    out = []
    for f in sorted((REF / "data" / "fixtures").glob("*/quotes_snapshot_iter*.json")):
        out.extend(json.loads(f.read_text()).values())
    if not out:
        raise SystemExit("FATAL: no quote fixtures -- the collector sweep would be empty")
    return out


def chunks(msgs, rnd, max_per=40):
    out, i = [], 0
    while i < len(msgs):
        n = rnd.randint(1, max_per)
        out.append({"f": json.dumps(msgs[i:i + n], separators=(",", ":"))})
        i += n
    return out


def q_stream(rnd, quotes, rounds):
    """Q messages over `rounds` passes, per-symbol sequence numbers rising, locally shuffled so
    older sequence numbers regularly arrive after newer ones (the out-of-order rule)."""
    msgs = []
    for r in range(rounds):
        batch = []
        for q in rnd.sample(quotes, k=len(quotes) // 2):
            m = dict(q)
            m["q"] = q["q"] + 1000 * r + rnd.randint(0, 999)
            m["t"] = q["t"] + 250 * r
            x = rnd.random()
            if x < 0.05:
                m.pop("bp", None)
                m.pop("bs", None)                                 # no bid: Massive omits both
            elif x < 0.08:
                m.pop("q")                                        # no sequence: reads as 0
            elif x < 0.10:
                m["bp"] = round(m.get("bp", 0.05) + 0.05, 2)
            batch.append(m)
        for i in range(0, len(batch) - 1, 7):
            if rnd.random() < 0.5:
                batch[i], batch[i + 1] = batch[i + 1], batch[i]
        msgs.extend(batch)
        if r % 2:
            msgs.append({"ev": "status", "status": "success",
                         "message": f"subscribed to: Q.{rnd.choice(quotes)['sym']}"})
    return msgs


def gen_collector(rnd):
    L = []
    quotes = fixture_quotes()
    syms = sorted({q["sym"] for q in quotes})
    tick = syms[:120]
    held = [q for q in quotes if q["sym"] in set(tick)]
    frames = chunks(q_stream(rnd, held, 6), rnd)
    third = len(frames) // 3
    extra = ["O:SPXW260821C09990000", "O:SPXW260821P09990000"]
    want1 = syms[40:170] + extra
    conn1 = ([CONNECTED, AUTH_OK] + frames[:third] + ["timeout", "yield"] + frames[third:2 * third]
             + ["yield"] + frames[2 * third:]
             + [{"f": ""}, {"f": "not json"}, {"f": "[]"}, {"f": "{}"}, {"f": '""'}, "err"])
    conn2 = [CONNECTED, AUTH_OK] + chunks(q_stream(rnd, held[:50], 2), rnd) + ["yield"]
    steps = [{"op": "clock", "t": 5000.0, "step": 0.001},
             {"op": "start", "attempts": 6, "backoff": 5.0}, {"op": "sends", "raw": True},
             {"op": "state"}, {"op": "health"}, {"op": "pump"}, {"op": "state"}, {"op": "snapshot"},
             {"op": "health"}, {"op": "health", "now": 6000.25},
             {"op": "recenter", "want": want1}, {"op": "sends"}, {"op": "state"}, {"op": "snapshot"},
             {"op": "recenter", "want": want1}, {"op": "recenter", "want": want1, "force": True},
             {"op": "pump"}, {"op": "state"}, {"op": "snapshot"},
             {"op": "unsubscribe", "tickers": syms[40:60] + ["O:NOPE"]},
             {"op": "subscribe", "tickers": syms[40:45] + syms[40:42]},
             {"op": "subscribe", "tickers": [f"O:X{i:06d}" for i in range(900)]},
             {"op": "sends"}, {"op": "state"},
             {"op": "recenter", "want": [f"O:Y{i:06d}" for i in range(1001)], "force": True},
             {"op": "recenter", "want": [f"O:Y{i:06d}" for i in range(1000)] * 2, "force": True},
             {"op": "sends"}, {"op": "state"},
             {"op": "recenter", "want": syms[:80], "force": True}, {"op": "sends"},
             # A DIRECT subscribe leaves `tickers` stale (only recenter re-sorts it), so the
             # reconnect below must resubscribe sorted(_subscribed), not the old ticker list.
             {"op": "subscribe", "tickers": ["O:ZZZ", "O:AAA"]}, {"op": "sends"},
             {"op": "pump"}, {"op": "state"}, {"op": "health"},
             {"op": "reconnect", "attempts": 3, "backoff": 5.0}, {"op": "sends", "raw": True},
             {"op": "state"}, {"op": "snapshot"}, {"op": "pump"}, {"op": "state"},
             {"op": "snapshot"}, {"op": "health"},
             {"op": "wait", "min": 10, "timeout": 20.0, "poll": 0.25}, {"op": "sleeps"},
             {"op": "wait", "min": 10000, "timeout": 2.0, "poll": 0.3}, {"op": "sleeps"},
             {"op": "stop"}, {"op": "state"}, {"op": "health"}]
    L.append(run_col({"name": "session", "tickers": tick + tick[:3], "conns": [conn1, conn2],
                      "steps": steps}))

    # Start: the max_connections retry, and everything that must NOT be retried.
    for conns, att, bo in (
            ([[CONNECTED, MAXCONN], [CONNECTED, MAXCONN], [CONNECTED, AUTH_OK]], 3, 5.0),
            ([[CONNECTED, MAXCONN]] * 3, 3, 2.0),
            ([[CONNECTED, AUTH_FAIL], [CONNECTED, AUTH_OK]], 6, 5.0),
            (["connfail", [CONNECTED, AUTH_OK]], 6, 5.0),
            ([[CONNECTED, "timeout"]], 6, 5.0), ([[CONNECTED, {"f": "[]"}]], 2, 1.0),
            ([[CONNECTED, {"f": "{}"}]], 2, 1.0),
            ([[CONNECTED, {"f": '[{"status":"x"},{"status":"auth_success"}]'}]], 2, 1.0),
            ([[CONNECTED, {"f": '[{"status":"auth_success"}, 5]'}]], 2, 1.0),
            ([[CONNECTED, {"f": '[5, {"status":"auth_success"}]'}]], 2, 1.0),
            ([[CONNECTED, {"f": "5"}]], 2, 1.0), ([[CONNECTED, {"f": '"auth_success"'}]], 2, 1.0),
            ([[{"f": "garbage"}]], 2, 1.0), ([], 2, 1.0)):
        L.append(run_col({"name": "start", "tickers": ["O:A", "O:B"], "conns": conns,
                          "steps": [{"op": "start", "attempts": att, "backoff": bo},
                                    {"op": "sleeps"}, {"op": "sends", "raw": True},
                                    {"op": "state"}]}))

    # Reconnect failures, and a re-centre against the dead socket they leave behind.
    L.append(run_col({
        "name": "reconnect", "tickers": ["O:A", "O:B", "O:C"],
        "conns": [[CONNECTED, AUTH_OK, "err"], [CONNECTED, MAXCONN], [CONNECTED, AUTH_FAIL],
                  [CONNECTED, MAXCONN], [CONNECTED, MAXCONN], "connfail",
                  [CONNECTED, AUTH_OK, "yield"]],
        "steps": [{"op": "start", "attempts": 1, "backoff": 1.0}, {"op": "pump"},
                  {"op": "state"}, {"op": "reconnect", "attempts": 3, "backoff": 5.0},
                  {"op": "state"}, {"op": "sleeps"},
                  {"op": "reconnect", "attempts": 2, "backoff": 4.0}, {"op": "state"},
                  {"op": "sleeps"},
                  {"op": "recenter", "want": ["O:A", "O:B", "O:C"], "force": True},
                  {"op": "recenter", "want": ["O:A", "O:D"], "force": True},
                  {"op": "state"}, {"op": "reconnect", "attempts": 1, "backoff": 4.0},
                  {"op": "state"}, {"op": "reconnect", "attempts": 1, "backoff": 4.0},
                  {"op": "sends", "raw": True}, {"op": "pump"}, {"op": "state"},
                  {"op": "health"}]}))

    # The rate limit on its exact boundary, on a still clock.
    L.append(run_col({
        "name": "recenter_boundary", "tickers": ["O:A", "O:B"],
        "conns": [[CONNECTED, AUTH_OK], [CONNECTED, AUTH_OK]],
        "steps": [{"op": "start", "attempts": 1, "backoff": 1.0},
                  {"op": "clock", "t": 3.0}, {"op": "recenter", "want": ["O:A"]},
                  {"op": "clock", "t": 100.0}, {"op": "recenter", "want": ["O:A", "O:C"]},
                  {"op": "clock", "t": 104.999}, {"op": "recenter", "want": ["O:B"]},
                  {"op": "clock", "t": 105.0}, {"op": "recenter", "want": ["O:A", "O:C"]},
                  {"op": "clock", "t": 105.5}, {"op": "recenter", "want": ["O:B"], "force": True},
                  {"op": "clock", "t": 200.0}, {"op": "recenter", "want": []},
                  {"op": "sends"}, {"op": "state"},
                  # Nothing subscribed: `sorted(_subscribed) or self.tickers` keeps the list.
                  {"op": "reconnect", "attempts": 1, "backoff": 1.0}, {"op": "sends", "raw": True},
                  {"op": "state"}]}))

    # The sequence rule, symbol by symbol.
    seq = [{"ev": "Q", "sym": "O:S", "bp": 1.0, "ap": 1.1, "bs": 1, "as": 1, "q": 5, "t": 1},
           {"ev": "Q", "sym": "O:S", "bp": 2.0, "ap": 2.1, "q": 3, "t": 2},
           {"ev": "Q", "sym": "O:S", "bp": 3.0, "ap": 3.1, "q": 5, "t": 3},
           {"ev": "Q", "sym": "O:S", "bp": 4.0, "ap": 4.1, "t": 4},
           {"ev": "Q", "sym": "O:S", "bp": 5.0, "ap": 5.1, "q": 5.5, "t": 5.5},
           {"ev": "Q", "sym": "O:S", "bp": 6, "ap": True, "q": True},
           {"ev": "Q", "sym": "O:S", "bp": "7", "ap": None, "q": 6, "t": True},
           {"ev": "Q", "sym": "O:T", "q": "b", "bp": 1.5},
           {"ev": "Q", "sym": "O:T", "q": "a", "bp": 2.5},
           {"ev": "Q", "sym": "O:T", "q": "c", "bp": 3.5},
           {"ev": "Q", "bp": 9.0, "q": 1}, {"ev": "Q", "sym": None, "bp": 9.5, "q": 2},
           {"ev": "Q", "sym": 7, "bp": 8.0},
           {"ev": "q", "sym": "O:U", "bp": 1.0}, {"sym": "O:U"}, {"ev": None}]
    frames = []
    for m in seq:
        frames += [{"f": json.dumps([m])}, "yield"]
    steps = [{"op": "start", "attempts": 1, "backoff": 1.0}, {"op": "clock", "t": 50.0, "step": 0.5}]
    for _ in seq:
        steps += [{"op": "pump"}, {"op": "snapshot"}, {"op": "state"}]
    L.append(run_col({"name": "sequence", "tickers": ["O:S"],
                      "conns": [[CONNECTED, AUTH_OK] + frames], "steps": steps}))

    # Frames the reference's pump dies on -- no pump_error, just a dead thread -- and their
    # harmless neighbours.
    for bad in ('5', 'null', 'true', '"x"', '{"ev":"Q"}', '[5]', '[{"ev":"Q","sym":"O:S"}, 5]',
                '[{"ev":"Q","sym":["a"]}]', '[{"ev":"Q","sym":{"a":1}}]',
                '[{"ev":"Q","sym":"O:S","q":1},{"ev":"Q","sym":"O:S","q":null}]',
                '[{"ev":"Q","sym":"O:S","q":null},{"ev":"Q","sym":"O:S","q":1}]',
                '[{"ev":"Q","sym":"O:S","q":1},{"ev":"Q","sym":"O:S","q":"1"}]',
                '[{"ev":"Q","sym":"O:S","q":[1]},{"ev":"Q","sym":"O:S","q":[2]}]'):
        L.append(run_col({"name": "fatal", "tickers": ["O:S"],
                          "conns": [[CONNECTED, AUTH_OK, {"f": '[{"ev":"Q","sym":"O:Z","q":1}]'},
                                     {"f": bad}, {"f": '[{"ev":"Q","sym":"O:W"}]'}]],
                          "steps": [{"op": "start", "attempts": 1, "backoff": 1.0},
                                    {"op": "clock", "t": 7.0, "step": 1.0}, {"op": "pump"},
                                    {"op": "state"}, {"op": "snapshot"}, {"op": "health"}]}))

    # The bounded status log: a session's worth of per-ticker acks.
    acks = [{"ev": "status", "status": "success", "message": f"subscribed to: Q.O:{i}"}
            for i in range(2150)]
    L.append(run_col({"name": "status_flood", "tickers": ["O:S"],
                      "conns": [[CONNECTED, AUTH_OK] + chunks(acks, rnd, 90) + ["yield"]],
                      "steps": [{"op": "start", "attempts": 1, "backoff": 1.0}, {"op": "pump"},
                                {"op": "state"}]}))
    return L


# --- FuturesFeed / IndexFeed -----------------------------------------------------------------------

RESOLVE = {}
TEE_LOG = []


def fake_front_month(product, day, *a, **k):
    u = {v: k for k, v in fut.PRODUCT.items()}.get(product, product)
    r = RESOLVE.get(u)
    if r == "raise":
        raise RuntimeError("no volume")
    return r


streams.fut.front_month = fake_front_month


class Tee:
    def offer(self, recv_ns, m):
        TEE_LOG.append([recv_ns, json.dumps(m)])


def norm_err(le):
    """What of last_error can be compared: a `status:` error in full (it is our own repr of the
    vendor's frame), a `handle:` error to its exception class, anything else to where it arose
    (the transport's exception names are websocket-client's, not ours)."""
    if le is None:
        return None
    if le.startswith("status: "):
        return le
    parts = le.split(": ")
    return ": ".join(parts[:2]) if le.startswith("handle: ") else parts[0]


def stream_op(feed, st):
    op = st["op"]
    if op == "clock":
        C.mono, C.mono_step = st["t"], st.get("step", 0.0)
        return None
    if op == "ns":
        C.ns, C.ns_step = st["t"], st.get("step", 0)
        return None
    if op == "start":
        feed.start()
        return {"ok": feed.ok, "errors": feed.errors, "last_error": norm_err(feed.last_error)}
    if op == "pump":
        feed._stop.clear()
        died = False
        try:
            feed._pump()
        except Exception:                                         # noqa: BLE001
            died = True
        if not feed._stop.is_set() and feed._thread is not None:
            feed._thread.alive = False
        return {"died": died}
    if op == "state":
        h = feed.health(st.get("now"))
        out = {"name": h["name"], "ok": h["ok"], "frames": h["frames"], "updates": h["updates"],
               "reconnects": h["reconnects"], "errors": h["errors"],
               "last_error": norm_err(h["last_error"]), "silent_s": h["silent_s"],
               "subs": list(feed.subs)}
        if isinstance(feed, streams.FuturesFeed):
            out["contract"] = dict(feed.contract)
            out["ticks"] = {u: feed._ticks.get(u, 0) for u in feed.underlyings}
            out["resolve_error"] = sorted(feed.resolve_error)
        return out
    if op == "mid":
        mid, age = feed.mid(st["u"], st.get("max_age"))
        return [mid, age]
    if op == "move_scale":
        return feed.move_scale(st["u"], st["h"], st["sig"], st["min"])
    if op == "snapshot":
        return {u: [q.value, q.ts_ns, q.ticker, q.market_status, q.underlying]
                for u, q in sorted(feed.snapshot(st.get("us")).items())}
    if op == "tee":
        out = list(TEE_LOG)
        TEE_LOG.clear()
        return out
    if op == "sends":
        return norm_sends(True)
    if op == "sleeps":
        return take_sleeps()
    if op == "stop":
        feed.stop()
        return None
    raise ValueError(op)


def run_stream(sc):
    reset(sc["conns"])
    RESOLVE.clear()
    RESOLVE.update(sc.get("resolve", {}))
    TEE_LOG.clear()
    tee = Tee() if sc.get("tee") else None
    if sc["kind"] == "futures":
        feed = streams.FuturesFeed(sc["underlyings"], contracts=sc.get("contracts"), tee=tee)
    else:
        feed = streams.IndexFeed(sc["underlyings"], tee=tee)
    P.on_yield = lambda: feed._stop.set()
    P.on_exhausted = lambda: feed._stop.set()
    for st in sc["steps"]:
        st["expect"] = stream_op(feed, st)
    return "stream " + hexj(sc)


def fut_frames(rnd, n, t0):
    """ES on its 0.25 grid (every squared change EXACT) and NQ off-grid (every square ROUNDS), with
    the frame pathologies the handler must survive sprinkled through."""
    es, nq, t = 6650.0, 24500.0, t0
    msgs = []
    for i in range(n):
        t += rnd.choice([0, 0, 1, 2, 3, 5, 8, 13, 40, 120, 700])
        if rnd.random() < 0.5:
            if rnd.random() < 0.15:
                es += 0.25 * rnd.choice([-2, -1, 1, 2])
            sp = 0.25 * rnd.choice([1, 1, 1, 2])
            m = {"ev": "Q", "sym": "ESZ6", "bx": 1, "bp": es - sp / 2, "bs": rnd.randint(1, 60),
                 "ax": 1, "ap": es + sp / 2, "as": rnd.randint(1, 60), "t": t}
        else:
            nq += rnd.uniform(-1.3, 1.3)
            bp = round(nq - rnd.uniform(0, 0.6), rnd.randint(2, 9))
            m = {"ev": "Q", "sym": "NQZ6", "bp": bp, "bs": rnd.randint(1, 9),
                 "ap": round(bp + rnd.uniform(0, 0.9), rnd.randint(1, 9)), "as": 3, "t": t}
        x = rnd.random()
        if x < 0.02:
            m.pop("t")                                   # no vendor stamp: receipt time instead
        elif x < 0.03:
            m["t"] = 0
        elif x < 0.04:
            m.pop("bp")
        elif x < 0.05:
            m["ap"], m["bp"] = m["bp"] - 1.0, m["ap"]    # crossed
        elif x < 0.055:
            m["sym"] = "ESH7"
        elif x < 0.06:
            m["bp"] = "abc"
        elif x < 0.065:
            m["bp"] = str(m["bp"])
        elif x < 0.068:
            m["ap"] = [1]
        elif x < 0.07:
            m["sym"] = ["ESZ6"]
        elif x < 0.075:
            m["bp"] = int(m["bp"])
        elif x < 0.078:
            m["t"] = float(m["t"]) + 0.75
        msgs.append(m)
    return msgs


def gen_streams(rnd):
    L = []
    t0 = 1787327400000
    msgs = fut_frames(rnd, 7000, t0)
    frames, i = [], 0
    while i < len(msgs):
        k = rnd.randint(1, 6)
        batch = msgs[i:i + k]
        i += k
        if rnd.random() < 0.02:
            batch.append({"ev": "status", "status": "success", "message": "subscribed to: Q.ESZ6"})
        if rnd.random() < 0.01:
            batch.append({"ev": "status", "status": "error", "message": "rate limited", "n": 1.5})
        frames.append({"f": json.dumps(batch if rnd.random() < 0.97 or len(batch) > 1 else batch[0])})
    script = [CONNECTED, AUTH_OK]
    steps = [{"op": "clock", "t": 10.0, "step": 0.001}, {"op": "ns", "t": t0 * 10**6, "step": 7_000_000},
             {"op": "start"}, {"op": "sends"}, {"op": "state"}, {"op": "mid", "u": "SPX"},
             {"op": "move_scale", "u": "SPX", "h": 0.1, "sig": 1.0, "min": 30}]
    last_t = t0
    for j in range(0, len(frames), 3):
        script += frames[j:j + 3] + (["timeout"] if j % 51 == 0 else []) + ["yield"]
        for f in frames[j:j + 3]:
            for m in (lambda d: d if isinstance(d, list) else [d])(json.loads(f["f"])):
                if isinstance(m.get("t"), int) and m["t"] > last_t:
                    last_t = m["t"]
        steps.append({"op": "pump"})
        for u in ("SPX", "NDX"):
            steps.append({"op": "move_scale", "u": u, "h": 0.1, "sig": 1.0, "min": 30})
            steps.append({"op": "move_scale", "u": u, "h": rnd.choice([0.05, 1.0, 2.5]),
                          "sig": rnd.choice([1.0, 2.0, 0.5]), "min": rnd.choice([2, 30, 400])})
        if j % 9 == 0:
            steps.append({"op": "ns", "t": last_t * 10**6 + rnd.randint(-2 * 10**9, 5 * 10**9),
                          "step": rnd.choice([0, 1_000_000])})
            for u in ("SPX", "NDX", "RUT"):
                steps.append({"op": "mid", "u": u})
                steps.append({"op": "mid", "u": u, "max_age": rnd.choice([0.5, 3.0, 10.0])})
        if j % 30 == 0:
            steps += [{"op": "state"}, {"op": "state", "now": 12.5 + j}, {"op": "tee"}]
    script.append("err")
    steps += [{"op": "pump"}, {"op": "state"}, {"op": "tee"}, {"op": "sends"}, {"op": "sleeps"}]
    L.append(run_stream({"name": "futures_session", "kind": "futures", "underlyings": ["SPX", "NDX"],
                         "contracts": {"SPX": "ESZ6", "NDX": "NQZ6"}, "tee": True,
                         "conns": [script, [CONNECTED, AUTH_OK, "yield"]], "steps": steps}))

    # Reconnect with exponential backoff, resubscribing on every new connection.
    L.append(run_stream({
        "name": "futures_reconnect", "kind": "futures", "underlyings": ["SPX", "NDX", "RUT"],
        "contracts": {"SPX": "ESZ6"}, "resolve": {"NDX": "NQZ6", "RUT": "raise"}, "tee": False,
        "conns": [[CONNECTED, AUTH_OK, {"f": json.dumps([{"ev": "Q", "sym": "ESZ6", "bp": 1.0, "ap": 2.0, "t": 5}])}, "err"],
                  "connfail", [CONNECTED, AUTH_FAIL], "connfail", "connfail", "connfail", "connfail",
                  [CONNECTED, AUTH_OK, {"f": json.dumps({"ev": "Q", "sym": "NQZ6", "bp": 3, "ap": 4})}, "yield"]],
        "steps": [{"op": "clock", "t": 100.0}, {"op": "start"}, {"op": "state"}, {"op": "pump"},
                  {"op": "state"}, {"op": "sleeps"}, {"op": "sends"}, {"op": "mid", "u": "NDX"},
                  {"op": "ns", "t": 5_000_000 + 2 * 10**9}, {"op": "mid", "u": "SPX"},
                  {"op": "mid", "u": "SPX", "max_age": 1.0}, {"op": "pump"}, {"op": "state"},
                  {"op": "sleeps"}, {"op": "stop"}, {"op": "state"}]}))
    # THE pow(x, 2) PIN. Over a realistic history the per-term ulp differences between libm pow()
    # and x*x are absorbed by the compensated sum -- measured: the 2,673 session cases above PASS
    # with x*x -- so the rule is pinned where it cannot hide: one or two mid changes per contract,
    # each difference chosen so that d**2 != d*d, on a 1 s span with horizon 1, so the only
    # rounding after the square is the sqrt.
    n_pin, pin_frames, pin_steps = 400, [], []
    unds = [f"U{i}" for i in range(n_pin)]
    t_pin = t0
    for i, u in enumerate(unds):
        m1 = rnd.uniform(5000, 30000)
        while True:
            m2 = m1 + rnd.uniform(-3, 3)
            d = m2 - m1
            if d ** 2 != d * d:
                break
        rows = [{"ev": "Q", "sym": f"C{i}", "bp": m1, "ap": m1, "t": t_pin},
                {"ev": "Q", "sym": f"C{i}", "bp": m2, "ap": m2, "t": t_pin + 1000}]
        if i % 2:
            while True:
                m3 = m2 + rnd.uniform(-3, 3)
                if (m3 - m2) ** 2 != (m3 - m2) * (m3 - m2):
                    break
            rows.append({"ev": "Q", "sym": f"C{i}", "bp": m3, "ap": m3, "t": t_pin + 2000})
        pin_frames += [{"f": json.dumps(rows)}, "yield"]
        pin_steps += [{"op": "pump"}, {"op": "move_scale", "u": u, "h": 1.0, "sig": 1.0, "min": 2},
                      {"op": "move_scale", "u": u, "h": 0.1, "sig": 1.0, "min": 2}]
    L.append(run_stream({"name": "pow_pin", "kind": "futures", "underlyings": unds,
                         "contracts": {u: f"C{i}" for i, u in enumerate(unds)},
                         "conns": [[CONNECTED, AUTH_OK] + pin_frames],
                         "steps": [{"op": "start"}] + pin_steps}))

    # The staleness bound on its exact edges: age == max is fresh, a nanosecond more is not, and
    # a negative age (clock skew) is refused.
    edge = [{"op": "start"}, {"op": "pump"}]
    for dt_ns, mx in ((3 * 10**9, None), (3 * 10**9 + 1, None), (0, None), (-1, None),
                      (5 * 10**8, 0.5), (5 * 10**8 + 1, 0.5), (10**10, 10.0)):
        edge += [{"op": "ns", "t": 1_700_000_000_000 * 10**6 + dt_ns},
                 {"op": "mid", "u": "SPX", **({} if mx is None else {"max_age": mx})}]
    L.append(run_stream({"name": "mid_edges", "kind": "futures", "underlyings": ["SPX"],
                         "contracts": {"SPX": "ESZ6"},
                         "conns": [[CONNECTED, AUTH_OK, {"f": json.dumps(
                             [{"ev": "Q", "sym": "ESZ6", "bp": 6650.25, "ap": 6650.5,
                               "t": 1_700_000_000_000}])}, "yield"]],
                         "steps": edge}))

    for resolve in ({"SPX": None, "NDX": None}, {"SPX": "raise"}, {"SPX": "ESZ6", "NDX": "ESZ6"}):
        L.append(run_stream({"name": "futures_resolve", "kind": "futures", "underlyings": ["SPX", "NDX", "SPX"],
                             "resolve": resolve, "conns": [], "steps": [{"op": "start"}, {"op": "state"},
                                                                        {"op": "sleeps"}]}))
    # Start failures never raise; a pump that dies on a non-dict message leaves ok False.
    for conns in ([[CONNECTED, AUTH_FAIL]], [["timeout"]], [], [[CONNECTED, {"f": "auth_success but not json"}]]):
        L.append(run_stream({"name": "start_fail", "kind": "index", "underlyings": ["SPX"], "conns": conns,
                             "steps": [{"op": "start"}, {"op": "state"}, {"op": "sends"}]}))
    for bad in ("[5]", "5", '"x"', "null", '[{"ev":"V","T":"I:SPX","val":1.5,"t":9}, []]'):
        L.append(run_stream({"name": "fatal", "kind": "index", "underlyings": ["SPX", "NDX"], "tee": True,
                             "conns": [[CONNECTED, AUTH_OK, {"f": bad}, {"f": "[]"}]],
                             "steps": [{"op": "clock", "t": 3.0, "step": 0.25}, {"op": "start"},
                                       {"op": "pump"}, {"op": "state"}, {"op": "snapshot"}, {"op": "tee"}]}))

    # The index: one level a second per underlying, both message shapes, and the junk.
    vm, lv, t = [], {"SPX": 7691.23, "NDX": 29301.5}, t0
    for i in range(900):
        t += rnd.choice([0, 1, 2, 998, 1000, 1002, 1044])
        u = rnd.choice(["SPX", "NDX"])
        if rnd.random() < 0.85:
            lv[u] = round(lv[u] + rnd.uniform(-0.8, 0.8), 2)
        m = {"ev": "V", "val": lv[u], "T": f"I:{u}", "t": t}
        x = rnd.random()
        if x < 0.05:
            m = {"ev": "A", "sym": f"I:{u}", "val": lv[u], "t": t}
        elif x < 0.07:
            m["T"] = ""
            m["sym"] = f"I:{u}"
        elif x < 0.08:
            m["val"] = None
        elif x < 0.09:
            m["T"] = "I:VIX"
        elif x < 0.10:
            m["val"] = str(m["val"])
        elif x < 0.11:
            m.pop("t")
        elif x < 0.115:
            m["val"] = {"x": 1}
        elif x < 0.12:
            m["T"] = ["I:SPX"]
        elif x < 0.125:
            m["t"] = "12x"
        elif x < 0.13:
            m["val"] = int(m["val"])
        vm.append(m)
    script, steps = [CONNECTED, AUTH_OK], [{"op": "clock", "t": 77.0, "step": 0.01},
                                           {"op": "ns", "t": t0 * 10**6, "step": 3_000_000}, {"op": "start"},
                                           {"op": "sends"}]
    for j in range(0, len(vm), 4):
        script += [{"f": json.dumps(vm[j:j + 4])}, "yield"]
        steps += [{"op": "pump"}, {"op": "snapshot"}, {"op": "snapshot", "us": ["NDX"]},
                  {"op": "snapshot", "us": ["SPX", "RUT"]}]
        if j % 40 == 0:
            steps += [{"op": "state"}, {"op": "tee"}]
    script.append("err")
    steps += [{"op": "pump"}, {"op": "state"}, {"op": "tee"}, {"op": "sleeps"}]
    L.append(run_stream({"name": "index_session", "kind": "index", "underlyings": ["SPX", "NDX", "RUT"],
                         "tee": True, "conns": [script], "steps": steps}))
    return L


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="tests/data/golden")
    out = pathlib.Path(ap.parse_args().out)
    rnd = random.Random(20260923)
    Lc, Lh, Li, Lf, Lu = gen_rest(rnd)
    Lc += gen_collector(rnd)
    Ls = gen_streams(rnd)
    emit(out, "massive_client", "massive/client.py", Lc)
    emit(out, "massive_history", "massive/history.py", Lh)
    emit(out, "massive_index", "massive/index.py", Li)
    emit(out, "massive_futures", "massive/futures.py", Lf)
    emit(out, "massive_streams", "massive/streams.py", Ls)
    emit(out, "massive_universe", "pricing/universe.py", Lu)


if __name__ == "__main__":
    main()
