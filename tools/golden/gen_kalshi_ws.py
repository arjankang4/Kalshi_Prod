#!/usr/bin/env python3
"""kalshi/ws.py MarketFeed -- the frame state machine, replayed with no socket.

The reference's own `_pump` runs unmodified against a fake socket whose `recv()` walks a script;
only `time` is replaced (a fake clock, so the command pacer's sleeps are exact and instantaneous).
At script checkpoints the whole observable state is dumped -- every book as the quote loop reads it
(`entry`), the validity map, positions, queued fills, health, sids, the command counter / pacer /
back-off, and everything SENT, SLEPT and TEED since the last checkpoint -- and the C++ MarketFeed,
fed the identical script, must produce the identical JSON.

Scenarios:
  tape       10,000 REAL frames of the 2026-08-26 quoter session (1,440 markets): the snapshot
             burst, deltas, the subscribe acks, the chunked subscribe the loop would send (1 account
             + 1 market subscribe + 2 add_markets), one delta made NEGATIVE (a refused book, one
             get_snapshot), one delta DROPPED (a subscription gap: every book invalid, 15 paced
             get_snapshot commands), and the socket dying at the end.
  basics     the reference's own kalshi/test_ws.py cases, as frames.
  errors     27 twice (back-off doubles, a snapshot clears it), 26, 25/10/17 (invalidate +
             resubscribe), and error frames whose `msg` is missing or not a dict.
  chunked    a terminal error on a 1,200-market feed: the resubscribe is chunked onto the old sid.
  nosid      a 1,200-market subscribe whose ack never comes: 10 s of 50 ms waits, then plain
             subscribes and a command error per chunk.
  sendfail   the socket refusing sends, and the feed with no socket at all.
  malformed  frames the exchange should not send but the pump must survive identically.
  positions  fills, market_position (singular), seed_positions, and junk values.
  coverage   wait_for_coverage against the fake clock.
  channels   book-only and account-only channel sets.

Big lists are digested (see `digest`) so a checkpoint stays a line, not megabytes.
"""
import argparse, collections, gzip, hashlib, json, pathlib, sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
from gen_golden import emit  # noqa: E402
import websocket  # noqa: E402
from kalshi import ws as W  # noqa: E402

FIXTURE = (pathlib.Path(__file__).resolve().parent.parent.parent
           / "tests/fixtures/kalshi_ws/quoter-20260826-bdae7194-head10k.jsonl.gz")
J = lambda v: json.dumps(v, separators=(",", ":"))  # noqa: E731


def digest(x):
    s = J(x)
    if len(s) <= 4000:
        return x
    return {"sha": hashlib.sha256(s.encode()).hexdigest()[:16], "len": len(x)}


class FakeTime:
    def __init__(self):
        self.ns, self.mono, self.sleeps = 0, 0.0, []

    def time_ns(self):
        return self.ns

    def monotonic(self):
        return self.mono

    def sleep(self, d):
        self.sleeps.append(d)
        self.mono += d
        self.ns += int(d * 1e9)


T = FakeTime()
W.time = T


class RecTee:
    def __init__(self):
        self.offers = []

    def offer(self, recv_ns, frame):
        self.offers.append([recv_ns, J(frame)])


def state(f, ws, tee):
    known = list(f.tickers) + [t for t in f._books if t not in set(f.tickers)]
    books = {t: f.entry(t) for t in known}
    step = max(1, len(known) // 8)
    st = {"books": digest(books),
          "sample": {t: books[t] for t in known[::step]},
          "valid": digest([[t, v] for t, v in f._valid.items()]),
          "positions": digest(f.positions()),
          "fills": digest(list(f._fills)),
          "health": f.health(),
          "sids": [[k, v] for k, v in f._sids.items()],
          "cmd": [f._cmd_id, f._next_cmd_t, f._cmd_backoff],
          "sent": digest(ws.sent if ws is not None else []),
          "sleeps": digest(T.sleeps),
          "tee": digest(tee.offers if tee is not None else [])}
    if ws is not None:
        ws.sent = []
    T.sleeps = []
    if tee is not None:
        tee.offers = []
    return st


def do_call(f, name, a):
    try:
        if name == "subscribe":
            r = f._subscribe(a.get("tickers") or f.tickers, chunk=a.get("chunk"),
                             sid_timeout=a.get("sid_timeout", 10.0))
        elif name == "request_snapshot":
            r = f.request_snapshot(a["tickers"])
        elif name == "drain_fills":
            r = f.drain_fills()
        elif name == "seed_positions":
            r = f.seed_positions(a["positions"])
        elif name == "books":
            r = digest(f.books(a.get("tickers")))
        elif name == "entry":
            r = f.entry(a["ticker"])
        elif name == "position":
            r = f.position(a["ticker"])
        elif name == "wait_for_coverage":
            r = f.wait_for_coverage(a["fraction"], a["timeout"], a["poll"])
        elif name == "health_at":
            r = f.health(a["now"])
        else:
            raise SystemExit(f"unknown call {name}")
        return {"ok": r}
    except Exception as e:                                   # noqa: BLE001
        return {"error": f"{type(e).__name__}: {e}"}


class FakeWS:
    """recv() walks the script; every non-frame event raises the timeout the pump ignores."""

    def __init__(self, f, events, tee, out):
        self.f, self.events, self.tee, self.out = f, collections.deque(events), tee, out
        self.sent, self.fail = [], None

    def send(self, s):
        if self.fail:
            raise BrokenPipeError(self.fail)
        self.sent.append(s)

    def recv(self):
        ev = self.events.popleft() if self.events else ["end"]
        kind = ev[0]
        if kind == "frame":
            T.ns, T.mono = ev[2], ev[3]
            return ev[1]
        if kind == "clock":
            T.ns, T.mono = ev[1], ev[2]
        elif kind == "check":
            self.out.append(("want", state(self.f, self, self.tee)))
        elif kind == "call":
            self.out.append(("want", do_call(self.f, ev[1], ev[2])))
        elif kind == "sendfail":
            self.fail = ev[1]
        elif kind == "recv_error":
            raise type(ev[1], (Exception,), {})(ev[2])
        elif kind == "end":
            self.f._stop.set()
        raise websocket.WebSocketTimeoutException("timeout")


L = []


def scenario(name, tickers, events, channels=W.CHANNELS, tee=True, ws=True, t0=(1_790_000_000_000_000_000, 500.0)):
    T.ns, T.mono = t0
    T.sleeps = []
    rt = RecTee() if tee else None
    f = W.MarketFeed(tickers, channels=channels, tee=rt)
    out = []
    fws = FakeWS(f, events, rt, out)
    if ws:
        f._ws = fws
    else:
        # No socket at all: `_ws` stays None, and the script is driven by calling the pump's body
        # directly on a stand-in whose sends are never reached.
        f._ws = None
    L.append("scenario " + J({"name": name, "tickers": tickers, "channels": list(channels), "tee": tee,
                              "ws": ws, "t0": list(t0)}))
    if ws:
        f._pump()
    else:
        while fws.events:
            try:
                raw = fws.recv()
            except websocket.WebSocketTimeoutException:
                continue
            except Exception:                                # noqa: BLE001
                break
            f._handle(json.loads(raw))
    # Replay the script with its expectations interleaved, in consumption order.
    wants = iter(out)
    for ev in events:
        L.append("ev " + J(ev))
        if ev[0] in ("check", "call"):
            L.append("want " + J(next(wants)[1]))
    L.append("final " + J(state(f, fws if ws else None, rt)))


def snap(t, yes=(), no=(), seq=None, sid=1):
    m = {"type": "orderbook_snapshot", "sid": sid, "msg": {"market_ticker": t}}
    if yes:
        m["msg"]["yes_dollars_fp"] = [list(x) for x in yes]
    if no:
        m["msg"]["no_dollars_fp"] = [list(x) for x in no]
    if seq is not None:
        m["seq"] = seq
    return m


def delta(t, side, px, d, seq=None, sid=1):
    m = {"type": "orderbook_delta", "sid": sid, "msg": {"market_ticker": t, "side": side,
                                                        "price_dollars": px, "delta_fp": d}}
    if seq is not None:
        m["seq"] = seq
    return m


class Clock:
    """Builds frame events on a steadily advancing clock."""

    def __init__(self, ns=1_790_000_000_000_000_000, mono=500.0):
        self.ns, self.mono = ns, mono

    def frame(self, msg, dt=0.01):
        self.ns += int(dt * 1e9)
        self.mono += dt
        raw = msg if isinstance(msg, str) else J(msg)
        return ["frame", raw, self.ns, self.mono]


def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--out", default="tests/data/golden")
    out = pathlib.Path(ap.parse_args().out)

    # --- tape --------------------------------------------------------------------------------------
    recs = [json.loads(l) for l in gzip.open(FIXTURE, "rt")]
    tickers = []
    for r in recs:
        f = r["frame"]
        if f.get("type") == "orderbook_snapshot":
            tickers.append(f["msg"]["market_ticker"])
    t0 = recs[0]["recv_ns"]
    ev = []
    negated = dropped = None
    for i, r in enumerate(recs):
        f = r["frame"]
        if i == 3:
            ev.append(["call", "subscribe", {}])
            ev.append(["check"])
        if i >= 5000 and negated is None and f.get("type") == "orderbook_delta":
            f = json.loads(J(f))
            f["msg"]["delta_fp"] = "-99999999.00"            # more than any level holds
            negated = i
        if i >= 8000 and dropped is None and f.get("type") == "orderbook_delta":
            dropped = i                                      # the frame that never arrives
            ev.append(["check"])
            continue
        ev.append(["frame", J(f), r["recv_ns"], 1000.0 + (r["recv_ns"] - t0) / 1e9])
        if i % 1000 == 999:
            ev.append(["check"])
    ev.append(["call", "books", {}])
    ev.append(["call", "entry", {"ticker": tickers[0]}])
    ev.append(["call", "entry", {"ticker": tickers[-1]}])
    ev.append(["recv_error", "WebSocketConnectionClosedException", "Connection to remote host was lost."])
    scenario("tape", tickers, ev, t0=(t0 - 1_000_000, 999.0))

    # --- basics: kalshi/test_ws.py as frames --------------------------------------------------------
    T1, T2 = "KXINX-26AUG25H1600-B7662", "KXINX-26AUG25H1600-B7675"
    c = Clock()
    ev = [c.frame({"type": "subscribed", "id": 1, "msg": {"channel": "orderbook_delta", "sid": 1}}),
          c.frame(snap(T1, yes=[("0.3900", "40.00"), ("0.4000", "10.00")],
                       no=[("0.5500", "25.00"), ("0.5800", "15.00")], seq=1)),
          ["check"],
          c.frame(delta(T1, "yes", "0.4100", "20.00", seq=2)),
          c.frame(delta(T1, "yes", "0.4100", "-20.00", seq=3)),
          ["check"],
          ["call", "entry", {"ticker": T2}],
          c.frame(snap(T2, yes=[("0.6000", "10.00")], seq=4)),
          c.frame(delta(T1, "yes", "0.4100", "5.00", seq=9)),          # 5..8 missing: a gap
          ["check"],
          c.frame(delta(T1, "yes", "0.4200", "5.00", seq=10)),         # discarded
          c.frame(snap(T1, yes=[("0.4300", "7.00")], seq=11)),
          ["check"],
          c.frame({"type": "ok", "seq": 12, "msg": {}}),               # no phantom gap
          c.frame(delta(T1, "yes", "0.4300", "-25.00", seq=13)),       # negative: refused
          ["check"],
          c.frame(snap(T1, no=[("0.9900", "1.00")], seq=14)),          # one-sided book
          c.frame({"type": "unsubscribed", "sid": 1, "seq": 15}),
          c.frame(delta(T1, "no", "0.9900", "-1.00", seq=16)),         # emptied: a book, not None
          ["check"],
          # deci- and centi-cent levels, and the "%.4f" / "%.2f" rounding of exact binary halves
          c.frame(snap("SUBCENT", yes=[("0.40125", "1.50"), ("0.123456", "0.01"), ("0.99995", "12.34"),
                                       ("0.00005", "100.00"), ("0.00015", "0.05"), ("0.000250", "7")],
                       no=[("0.500050", "1.25"), ("0.999999", "3.1"), ("0.000001", "0.07")], seq=17)),
          c.frame(delta("SUBCENT", "yes", "0.40125", "-0.75", seq=18)),
          ["check"],
          ["call", "entry", {"ticker": "SUBCENT"}],
          ["call", "books", {"tickers": [T2, T1, "UNKNOWN"]}],
          ["call", "request_snapshot", {"tickers": [f"T{i}" for i in range(250)]}],
          ["check"]]
    scenario("basics", [T1, T2], ev)

    # --- errors -------------------------------------------------------------------------------------
    c = Clock()
    ev = [c.frame({"type": "subscribed", "id": 1, "msg": {"channel": "orderbook_delta", "sid": 7}}),
          c.frame(snap(T1, yes=[("0.4000", "10.00")], seq=1)),
          c.frame({"type": "error", "id": 4, "msg": {"code": 27, "msg": "too many"}}),
          ["check"],
          ["call", "request_snapshot", {"tickers": [T1]}],
          c.frame({"type": "error", "msg": {"code": 27, "msg": "too many"}}),
          ["call", "request_snapshot", {"tickers": [T1, T2]}],
          ["check"],
          c.frame({"type": "error", "msg": {"code": 27.0, "msg": "again"}}),
          c.frame({"type": "error", "msg": {"code": 27, "msg": "and again"}}),
          c.frame({"type": "error", "msg": {"code": 27, "msg": "cap"}}),
          c.frame({"type": "error", "msg": {"code": 27, "msg": "cap"}}),
          ["check"],
          c.frame(snap(T1, yes=[("0.4000", "10.00")], seq=2)),
          ["check"],
          c.frame({"type": "error", "msg": {"code": 26, "msg": "too many markets"}}),
          c.frame({"type": "error", "msg": {"code": 25, "msg": "subscription buffer overflow " * 5}}),
          ["check"],
          c.frame(snap(T2, yes=[("0.5000", "1.00")], seq=3)),
          c.frame({"type": "error", "msg": {"code": 10, "msg": None}}),
          c.frame({"type": "error", "msg": {"code": 17}}),
          c.frame({"type": "error", "code": 99, "msg": None}),
          c.frame({"type": "error", "code": 25, "msg": {}}),
          c.frame({"type": "error", "msg": "a string msg"}),
          c.frame({"type": "error", "msg": {"code": [25], "msg": 1.5}}),
          c.frame({"type": "error", "msg": {"code": "27", "msg": ["x"]}}),
          ["check"]]
    scenario("errors", [T1, T2], ev)

    # --- chunked resubscribe on a terminal error ------------------------------------------------------
    big = [f"KXINXU-26SEP23H{1000 + 100 * (i % 7)}-B{5000 + i}" for i in range(1200)]
    c = Clock()
    ev = [c.frame({"type": "subscribed", "id": 1, "msg": {"channel": "fill", "sid": 1}}),
          c.frame({"type": "subscribed", "id": 2, "msg": {"channel": "orderbook_delta", "sid": 2}}),
          ["call", "subscribe", {}],
          ["check"],
          c.frame({"type": "error", "msg": {"code": 17, "msg": "internal"}}),
          ["check"],
          ["call", "subscribe", {"tickers": big[:7], "chunk": 3}],
          ["check"]]
    scenario("chunked", big, ev)

    # --- a subscribe whose ack never arrives -----------------------------------------------------------
    ev = [["call", "subscribe", {"sid_timeout": 1.0}], ["check"],
          ["call", "subscribe", {}], ["check"]]
    scenario("nosid", big, ev)

    # --- send failures, and no socket at all -----------------------------------------------------------
    c = Clock()
    ev = [c.frame({"type": "subscribed", "id": 1, "msg": {"channel": "orderbook_delta", "sid": 3}}),
          ["sendfail", "Broken pipe"],
          ["call", "request_snapshot", {"tickers": [T1, T2]}],
          ["call", "subscribe", {}],
          ["check"],
          ["sendfail", None],
          ["call", "request_snapshot", {"tickers": [T1]}],
          ["check"]]
    scenario("sendfail", [T1, T2], ev)
    # No socket at all (`_ws` is None): every send is an AttributeError, recorded, never raised,
    # and the sid wait gives up at once. Driven without the pump, which would itself die on recv.
    ev = [["call", "request_snapshot", {"tickers": [T1]}],
          ["call", "subscribe", {"tickers": big[:600]}],
          ["check"]]
    scenario("nosocket", [T1, T2], ev, ws=False)

    # --- malformed frames ------------------------------------------------------------------------------
    c = Clock()
    ev = [c.frame({"type": "subscribed", "id": 1, "msg": {"channel": "orderbook_delta", "sid": 1}}),
          c.frame(snap(T1, yes=[("0.4000", "10.00")], seq=1)),
          c.frame("not json"),
          c.frame("[1, 2]"),
          c.frame('"str"'),
          c.frame("null"),
          c.frame("{}"),
          ["check"],
          c.frame({"type": "orderbook_snapshot", "seq": 2, "msg": [1, 2]}),
          c.frame({"type": "orderbook_snapshot", "seq": 3, "msg": {"market_ticker": ""}}),
          c.frame({"type": "orderbook_snapshot", "seq": 4, "msg": {"market_ticker": T2,
                   "yes_dollars_fp": [["0.1000", "1.00"], ["0.12345678", "2.00"]]}}),
          ["check"],
          c.frame({"type": "orderbook_snapshot", "seq": 5, "msg": {"market_ticker": T1,
                   "yes_dollars_fp": [["0.2000", "3.00"], ["0.3000"]]}}),
          ["check"],
          c.frame(snap(T1, yes=[("0.4000", "10.00")], no=[("0.5000", "2.00")], seq=6)),
          c.frame(delta(T1, "yes", "0.4000123", "1.00", seq=7)),        # PrecisionError: refused
          c.frame(snap(T1, yes=[("0.4000", "10.00")], seq=8)),
          c.frame({"type": "orderbook_delta", "seq": 9, "msg": {"market_ticker": T1, "price_dollars": "0.4",
                                                               "delta_fp": "1.00"}}),   # KeyError side
          c.frame(delta(T1, "maybe", "0.4000", "1.00", seq=10)),        # AttributeError
          c.frame(delta(T1, "yes", "0.4000", None, seq=11)),            # TypeError
          c.frame(delta(T1, "yes", "0.4000", "12.345", seq=12)),        # count precision: refused
          ["check"],
          c.frame(snap(T1, yes=[("0.4000", "10.00")], seq=13)),
          c.frame({"type": "orderbook_delta", "seq": 14, "msg": {"market_ticker": T1, "side": 5,
                                                                "price_dollars": "0.4", "delta_fp": "1"}}),
          c.frame({"type": "orderbook_delta", "seq": "15", "msg": {}}),
          c.frame({"type": "ok", "seq": 16}),
          ["check"],
          c.frame({"type": ["x"], "seq": 1}),
          c.frame({"type": "subscribed", "msg": "notadict"}),
          c.frame({"type": "subscribed", "msg": None}),
          c.frame({"type": "subscribed", "msg": {"channel": "fill"}}),
          c.frame({"type": "orderbook_snapshot", "msg": {"market_ticker": T2, "yes_dollars_fp": "ab"}}),
          ["check"],
          c.frame({"type": "orderbook_snapshot", "msg": {"market_ticker": T2, "no_dollars_fp": 5}}),
          c.frame({"type": "orderbook_snapshot", "msg": {"market_ticker": T2, "no_dollars_fp": [7]}}),
          c.frame({"type": "orderbook_delta", "seq": 3.0, "msg": {"market_ticker": "NEW"}}),
          c.frame({"type": "orderbook_delta", "seq": 4, "msg": {"market_ticker": None}}),
          c.frame({"type": "whatever", "seq": 99, "msg": {"x": 1}}),
          c.frame(""),
          ["check"]]
    scenario("malformed", [T1, T2], ev)

    # --- positions and fills -------------------------------------------------------------------------
    c = Clock()
    ev = [["call", "seed_positions", {"positions": {T1: 5, "OTHER": "-2.5"}}],
          c.frame({"type": "market_position", "msg": {"market_ticker": T1, "position_fp": "10.00"}}),
          c.frame({"type": "market_position", "msg": {"market_ticker": T1, "position_fp": "-5.00"}}),
          c.frame({"type": "market_positions", "msg": {"market_ticker": T2, "position_fp": "99.00"}}),
          c.frame({"type": "fill", "msg": {"market_ticker": T2, "count_fp": "20.00", "post_position_fp": "20.00",
                                           "order_id": "o1", "yes_price_dollars": "0.4200"}}),
          c.frame({"type": "fill", "msg": {"market_ticker": T2, "count_fp": "20.00", "post_position_fp": " 4e1 ",
                                           "order_id": "o2"}}),
          ["check"],
          ["call", "seed_positions", {"positions": {T1: 1, T2: 2, "THIRD": 3.5}}],
          ["call", "position", {"ticker": T1}],
          ["call", "position", {"ticker": "NEVER"}],
          ["call", "drain_fills", {}],
          ["call", "drain_fills", {}],
          c.frame({"type": "market_position", "msg": {"market_ticker": T1, "position_fp": None}}),
          c.frame({"type": "market_position", "msg": {"market_ticker": T1, "position_fp": "banana"}}),
          c.frame({"type": "market_position", "msg": {"market_ticker": T1, "position_fp": [1]}}),
          c.frame({"type": "market_position", "msg": {"market_ticker": T1, "position_fp": "1_000"}}),
          c.frame({"type": "market_position", "msg": {"market_ticker": T1, "position_fp": "nan"}}),
          c.frame({"type": "market_position", "msg": {"market_ticker": T2, "position_fp": "-inf"}}),
          c.frame({"type": "market_position", "msg": {"position_fp": "7.00"}}),
          c.frame({"type": "market_position", "msg": {"market_ticker": "", "position_fp": "7.00"}}),
          c.frame({"type": "market_position", "msg": {"market_ticker": "Z", "position_fp": True}}),
          c.frame({"type": "fill"}),
          c.frame({"type": "fill", "msg": [1]}),
          ["check"],
          ["call", "seed_positions", {"positions": {"X": "junk"}}],
          ["call", "seed_positions", {"positions": {}}],
          ["check"]]
    scenario("positions", [T1, T2], ev)

    # --- coverage --------------------------------------------------------------------------------------
    c = Clock()
    tk = [f"C{i}" for i in range(10)]
    ev = [c.frame(snap("C0", yes=[("0.1000", "1.00")], seq=1)),
          c.frame(snap("C1", yes=[("0.1000", "1.00")], seq=2)),
          ["call", "wait_for_coverage", {"fraction": 0.9, "timeout": 2.0, "poll": 0.25}],
          ["call", "wait_for_coverage", {"fraction": 0.2, "timeout": 2.0, "poll": 0.25}],
          ["call", "health_at", {"now": 12345.5}],
          c.frame(snap("NOT_WANTED", yes=[("0.1000", "1.00")], seq=3)),
          ["check"]]
    scenario("coverage", tk, ev)
    scenario("coverage_empty", [], [["call", "wait_for_coverage", {"fraction": 0.0, "timeout": 1.0, "poll": 0.5}],
                                    ["check"]])

    # --- channel sets ----------------------------------------------------------------------------------
    scenario("book_only", [T1, T2], [["call", "subscribe", {}], ["check"]], channels=("orderbook_delta",))
    scenario("account_only", [T1, T2], [["call", "subscribe", {}], ["check"]], channels=("fill", "market_positions"))
    scenario("no_tee", [T1], [Clock().frame(snap(T1, yes=[("0.5000", "1.00")], seq=1)), ["check"]], tee=False)

    emit(out, "kalshi_ws", "kalshi/ws.py", L)


if __name__ == "__main__":
    main()
