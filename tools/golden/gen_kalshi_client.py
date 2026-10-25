#!/usr/bin/env python3
"""kalshi/client.py (+ strategy/cancel_all.py) -- the request layer, replayed against a script.

NOTHING HERE TOUCHES THE NETWORK, and nothing here could send an order. The reference's own
`_request` runs unmodified; only its three seams are replaced -- `_connection` (a scripted fake that
answers each attempt with a status + body or raises a transport error, and says whether the socket
was "reused"), `time` (a fake clock that advances only when something sleeps), and `_sign` (a
deterministic stand-in: RSA-PSS is randomized, and it is tested separately by test_kalshi_sign).
The C++ Client replays the same script through a fake Transport, and the full TRACE must match:
every request line, body and header, every sleep, the write bucket after the call, the read rate,
and the result or the exception.

What the traces pin, because each was a real failure:
  - GETs retry on 429/5xx with 0.4 * 2^n backoff, and immediately on a dropped REUSED socket;
  - writes NEVER retry, on any status or transport failure, and a 429 on a write penalises the
    bucket (a debit before the send, a second one on the 429);
  - pagination follows `cursor` until falsy, `{**params, "cursor": None}` puts a bare "?" on the
    first page, and resting_orders/markets/fills keep the reference's param order;
  - orderbooks chunks at 100 with REPEATED `tickers` params;
  - open_positions/endpoint_costs/write_budget/set_*_budget never raise and say why;
  - cancel_all names 18 events, chunks them at 10, and verifies by RE-READING.
"""
import argparse, contextlib, datetime as dt, io, json, pathlib, sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
from gen_golden import emit  # noqa: E402
import config  # noqa: E402
from kalshi import client as k  # noqa: E402
from strategy import cancel_all  # noqa: E402

J = lambda v: json.dumps(v, separators=(",", ":"))  # noqa: E731


class HttpError(OSError):
    """Stands in for any transport failure; the C++ side raises net::HttpError."""


class Clock:
    def __init__(self):
        self.mono, self.wall, self.sleeps = 0.0, 0.0, []

    def monotonic(self):
        return self.mono

    def time(self):
        return self.wall

    def sleep(self, d):
        self.sleeps.append(d)
        self.mono += d
        self.wall += d


class Resp:
    def __init__(self, status, body):
        self.status, self._body, self.will_close = status, body, False

    def read(self):
        return self._body


class Conn:
    def __init__(self, h):
        self.h = h

    def request(self, method, target, body=None, headers=None):
        self.h.reqs.append({"method": method, "target": target,
                            "body": None if body is None else body.decode(),
                            "headers": [[a, b] for a, b in (headers or {}).items()],
                            "timeout": self.h.timeout})

    def getresponse(self):
        item = self.h.cur
        if "error" in item:
            raise HttpError(item["error"])
        body = bytes.fromhex(item["body_hex"]) if "body_hex" in item else item["body"].encode()
        return Resp(item["status"], body)


class Harness:
    def __init__(self):
        self.clock = Clock()
        self.reqs, self.script, self.cur, self.timeout = [], [], None, None

    def connection(self, scheme, netloc, timeout):
        self.cur = self.script.pop(0)
        self.timeout = timeout
        return Conn(self), bool(self.cur.get("reused"))


H = Harness()
k.time = H.clock
k._connection = H.connection
k._drop_connection = lambda: None
k._sign = lambda ts, m, p: f"SIG|{ts}|{m}|{p}"
# The reference may build its headers through kalshi.auth (key id, key path and its own `time`) rather
# than through client._sign; stub the same seam there so either layout signs deterministically.
try:
    from kalshi import auth as _auth  # noqa: E402
except ImportError:
    _auth = None
if _auth is not None:
    _auth.time = H.clock
    _auth.load_private_key = lambda path: None
    _auth.sign = lambda key, ts, m, p: f"SIG|{ts}|{m}|{p}"
config.KALSHI_KEY_ID = "KEYID"
config.KALSHI_API_BASE = "https://example.test/trade-api/v2"

L = []


def ok(status=200, body=None, reused=True):
    return {"status": status, "body": body if isinstance(body, str) else J(body if body is not None else {}),
            "reused": reused}


def err(msg, reused):
    return {"error": msg, "reused": reused}


def call(name, fn, args, script, t=(1000.0, 1790000000.25), stdout=False):
    """Run one client call against `script`; record the call and its full trace."""
    H.reqs, H.script = [], list(script)
    H.clock.mono, H.clock.wall = t
    H.clock.sleeps = []
    out, errs = io.StringIO(), io.StringIO()
    try:
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(errs):
            r = fn()
        res = {"ok": r}
    except k.KalshiError as e:
        res = {"KalshiError": [e.status, e.body]}
    except HttpError as e:
        res = {"HttpError": str(e)}
    except Exception as e:                                   # noqa: BLE001
        res = {"error": type(e).__name__}
    trace = {"requests": H.reqs, "sleeps": H.clock.sleeps, "result": res,
             "bucket": [k.write_bucket._level, k.write_bucket.spent, k.write_bucket.denied,
                        k.write_bucket.capacity, k.write_bucket.refill_rate],
             "rate": k._RATE_PER_SEC, "unused_script": len(H.script)}
    if stdout:
        trace["stdout"], trace["stderr"] = out.getvalue(), errs.getvalue()
    L.append("call " + J({"name": name, "args": args, "script": script, "t": list(t)}))
    L.append("trace " + J(trace))


def reset(rate=8.0):
    L.append(f"reset {rate!r}")
    k.write_bucket = k.TokenBucket(900, 300)
    k.write_bucket._t = H.clock.mono = 1000.0
    k._next_slot[0] = 0.0
    k._RATE_PER_SEC = rate


def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--out", default="tests/data/golden")
    out = pathlib.Path(ap.parse_args().out)

    # --- markets: pagination, params order, the bare "?" --------------------------------------
    reset()
    pages = [ok(body={"markets": [{"ticker": "A"}, {"ticker": "B"}], "cursor": "c1"}, reused=False),
             ok(body={"markets": [{"ticker": "C"}], "cursor": "c/2+="}),
             ok(body={"markets": None, "cursor": ""})]
    call("markets", lambda: k.markets(event_ticker="KXINX-26SEP23H1600"),
         {"params": [["event_ticker", "KXINX-26SEP23H1600"]]}, pages)
    call("markets", lambda: k.markets(series_ticker="KXINXU", min_close_ts=1790000000, status=None),
         {"params": [["series_ticker", "KXINXU"], ["min_close_ts", 1790000000], ["status", None]]},
         [ok(body={"markets": [{"ticker": "Z"}]})])
    call("paged", lambda: k.paged("/portfolio/fills", "fills", signed=True),
         {"path": "/portfolio/fills", "key": "fills", "signed": True, "params": []},
         [ok(body={"fills": [1, 2], "cursor": "x"}), ok(body={"fills": [3]})])
    call("get", lambda: k.get("/exchange/status"), {"path": "/exchange/status", "params": []},
         [ok(body={"exchange_active": True, "trading_active": False})])

    # --- GET retry policy ------------------------------------------------------------------------
    reset()
    call("get", lambda: k.get("/markets", limit=5),
         {"path": "/markets", "params": [["limit", 5]]},
         [ok(429, "slow down"), ok(500, "boom"), ok(502, ""), ok(body={"markets": []})])
    call("get", lambda: k.get("/markets"), {"path": "/markets", "params": []},
         [ok(400, '{"error":{"code":"bad_request"}}')])
    call("get", lambda: k.get("/markets"), {"path": "/markets", "params": []},
         [ok(503, "a")] * 6)
    call("get", lambda: k.get("/markets"), {"path": "/markets", "params": []},
         [err("Remote end closed connection without response", True), ok(body={"x": 1})])
    call("get", lambda: k.get("/markets"), {"path": "/markets", "params": []},
         [err("connection refused", False)])
    call("get", lambda: k.get("/markets"), {"path": "/markets", "params": []},
         [err("reset", True)] * 5 + [err("reset again", True)])
    call("get", lambda: k.get("/markets"), {"path": "/markets", "params": []},
         [ok(200, "not json")])
    call("get", lambda: k.get("/markets"), {"path": "/markets", "params": []},
         [ok(404, "café \xff".encode("latin-1").decode("latin-1"))])

    # --- orderbooks: 100-ticker chunks, repeated params ------------------------------------------
    reset(rate=20.0)
    tk = [f"KXINX-26SEP23H1600-B{6000 + 5 * i}" for i in range(250)]
    bodies = [ok(body={"orderbooks": [{"ticker": t, "orderbook_fp": {"yes_dollars": [["0.4000", "10.00"]]}}
                                      for t in tk[i:i + 100]]}) for i in range(0, 250, 100)]
    bodies[1] = ok(body={"orderbooks": [{"ticker": tk[0], "dup": True}]
                         + [{"ticker": t} for t in tk[100:200]]})
    call("orderbooks", lambda: k.orderbooks(tk, depth=10), {"tickers": tk, "depth": 10}, bodies)
    call("orderbooks", lambda: k.orderbooks(tk[:3]), {"tickers": tk[:3], "depth": None},
         [ok(body={"orderbooks": None})])

    # --- portfolio reads --------------------------------------------------------------------------
    reset()
    call("resting_orders", lambda: k.resting_orders(event_ticker="E1,E2"),
         {"params": [["event_ticker", "E1,E2"]]},
         [ok(body={"orders": [{"order_id": "o1"}], "cursor": "n"}), ok(body={"orders": [{"order_id": "o2"}]})])
    call("resting_orders", lambda: k.resting_orders(limit=5, status="canceled"),
         {"params": [["limit", 5], ["status", "canceled"]]}, [ok(body={"orders": []})])
    call("balance", lambda: k.balance(), {}, [ok(body={"balance": 12345})])
    call("fills", lambda: k.fills(ticker="T", min_ts=1), {"params": [["ticker", "T"], ["min_ts", 1]]},
         [ok(body={"fills": [{"trade_id": "t1"}], "cursor": "q"}), ok(body={"fills": [], "cursor": None})])
    call("open_positions", lambda: k.open_positions(), {},
         [ok(body={"market_positions": [
             {"ticker": "A", "position_fp": "20.00"}, {"ticker": "B", "position_fp": "0.00"},
             {"ticker": "C", "position": -5}, {"ticker": "D", "position_fp": None, "position": 7},
             {"ticker": "A", "position_fp": "-3.50"}], "cursor": "p2"}),
          ok(body={"market_positions": [{"ticker": "E", "position_fp": "1"}]})])
    call("open_positions", lambda: k.open_positions(), {},
         [ok(body={"market_positions": [{"ticker": "A", "position_fp": "2"},
                                         {"position_fp": "3"}, {"ticker": "Z", "position_fp": "9"}]})])
    call("open_positions", lambda: k.open_positions(), {},
         [ok(body={"market_positions": [{"ticker": "A", "position_fp": "2"}], "cursor": "c"}),
          ok(401, "unauthorized")])
    call("open_positions", lambda: k.open_positions(), {},
         [ok(body={"market_positions": [{"ticker": "A", "position_fp": "banana"}]})])

    # --- WRITES: never retried, charged before the send, a 429 penalises -----------------------
    reset()
    call("cancel_order", lambda: k.cancel_order("ord-1"), {"order_id": "ord-1"},
         [ok(body={"order": {"order_id": "ord-1", "status": "canceled"}, "reduced_by": 20})])
    call("cancel_order", lambda: k.cancel_order("ord-2"), {"order_id": "ord-2"}, [ok(429, "rate")])
    call("cancel_order", lambda: k.cancel_order("ord-3"), {"order_id": "ord-3"}, [ok(500, "boom")])
    call("cancel_order", lambda: k.cancel_order("ord-4"), {"order_id": "ord-4"}, [err("dropped", True)])
    orders = [k.order_body("KXINX-26SEP23H1600-B6600", "bid", k.fp(0.41), "20.00", client_order_id="c1"),
              k.order_body("KXINX-26SEP23H1600-B6600", "ask", k.fp(0.47), "20.00", client_order_id="c2"),
              k.order_body("KXINX-26SEP23H1600-T6700", "bid", k.fp(0.05), "20.00")]
    call("batch_create_orders", lambda: k.batch_create_orders([]), {"orders": []}, [])
    call("batch_create_orders", lambda: k.batch_create_orders(orders), {"orders": orders},
         [ok(201, {"orders": [{"order": {"order_id": "x1"}}, {"error": {"code": "post_only"}}, {}]})])
    call("batch_create_orders", lambda: k.batch_create_orders(orders), {"orders": orders},
         [ok(429, '{"error":"too_many_requests"}')])
    call("batch_create_orders", lambda: k.batch_create_orders(orders[:1]), {"orders": orders[:1]},
         [ok(503, "unavailable")])
    call("batch_create_orders", lambda: k.batch_create_orders(orders[:1]), {"orders": orders[:1]},
         [err("Remote end closed connection without response", True)])
    call("batch_create_orders", lambda: k.batch_create_orders(orders[:2]), {"orders": orders[:2]},
         [ok(200, {"orders": None})])
    call("batch_cancel_orders", lambda: k.batch_cancel_orders([]), {"order_ids": []}, [])
    call("batch_cancel_orders", lambda: k.batch_cancel_orders(["a", "b", "c"]), {"order_ids": ["a", "b", "c"]},
         [ok(200, {"orders": [{"order_id": "a", "reduced_by": 20}, {"order_id": "b", "reduced_by": 0,
                                                                     "error": {"code": "not_found"}}]})])
    call("batch_cancel_orders", lambda: k.batch_cancel_orders(["a"]), {"order_ids": ["a"]},
         [ok(429, "no")])
    # a burst deep into debt, then time passes and the bucket refills
    call("batch_cancel_orders", lambda: k.batch_cancel_orders([f"o{i}" for i in range(500)]),
         {"order_ids": [f"o{i}" for i in range(500)]}, [ok(200, {"orders": []})], t=(1000.5, 1790000000.75))
    call("balance", lambda: k.balance(), {}, [ok(body={"balance": 1})], t=(1004.0, 1790000004.0))

    # --- budgets ----------------------------------------------------------------------------------
    adv = {"usage_tier": "advanced", "read": {"bucket_capacity": 900, "refill_rate": 300},
           "write": {"bucket_capacity": 900, "refill_rate": 300}}
    basic = {"usage_tier": "basic", "read": {"bucket_capacity": 400, "refill_rate": 200},
             "write": {"bucket_capacity": "400", "refill_rate": 100}}
    reset()
    call("write_budget", lambda: k.write_budget(), {}, [ok(body=adv)])
    call("write_budget", lambda: k.write_budget(), {}, [ok(403, "nope")])
    call("write_budget", lambda: k.write_budget(), {}, [ok(body={"usage_tier": "x", "write": None})])
    call("write_budget", lambda: k.write_budget(), {}, [ok(body={"write": {"bucket_capacity": "abc"}})])
    call("set_write_budget", lambda: k.set_write_budget(), {"refill_rate": None, "samples": 3},
         [ok(body=adv), ok(body=basic), ok(body=adv)])
    reset()
    call("set_write_budget", lambda: k.set_write_budget(), {"refill_rate": None, "samples": 3},
         [ok(body=adv), ok(body=adv), ok(500, "x")] + [ok(500, "x")] * 5)
    reset()
    call("set_write_budget", lambda: k.set_write_budget(samples=2), {"refill_rate": None, "samples": 2},
         [ok(401, "a"), ok(401, "b")])
    reset()
    call("set_write_budget", lambda: k.set_write_budget(refill_rate=250), {"refill_rate": 250, "samples": 3},
         [ok(body=adv)])
    call("set_write_budget", lambda: k.set_write_budget(samples=0), {"refill_rate": None, "samples": 0},
         [ok(body={"usage_tier": "premier", "write": {"bucket_capacity": 3000, "refill_rate": 1000}})])
    reset()
    call("set_read_budget", lambda: k.set_read_budget(share=config.READ_SHARE_TRADING),
         {"refill_rate": None, "cap": 20.0, "samples": 3, "share": config.READ_SHARE_TRADING},
         [ok(body=adv), ok(body=basic), ok(body=adv)])
    reset()
    call("set_read_budget", lambda: k.set_read_budget(),
         {"refill_rate": None, "cap": 20.0, "samples": 3, "share": 1.0},
         [ok(body=adv), ok(401, "x"), ok(body={"usage_tier": "advanced", "read": {"refill_rate": "banana"}})])
    reset(rate=12.0)
    call("set_read_budget", lambda: k.set_read_budget(refill_rate=5.0),
         {"refill_rate": 5.0, "cap": 20.0, "samples": 3, "share": 1.0}, [])
    reset(rate=12.0)
    call("set_read_budget", lambda: k.set_read_budget(refill_rate=5.0, share=0.2),
         {"refill_rate": 5.0, "cap": 20.0, "samples": 3, "share": 0.2}, [])
    reset()
    call("set_read_budget", lambda: k.set_read_budget(cap=7.5, samples=1),
         {"refill_rate": None, "cap": 7.5, "samples": 1, "share": 1.0}, [ok(body=adv)])
    call("endpoint_costs", lambda: k.endpoint_costs(), {},
         [ok(body={"default_cost": 10, "endpoint_costs": [{"method": "DELETE", "path": "/x", "cost": 2}]})])
    call("endpoint_costs", lambda: k.endpoint_costs(), {}, [ok(404, "x" * 500)])

    # --- strategy/cancel_all ----------------------------------------------------------------------
    reset()
    ev = cancel_all.today_events(date=dt.date(2026, 9, 23))
    L.append("events " + J({"date": "2026-09-23", "events": ev}))
    L.append("events " + J({"date": "2026-01-05", "events": cancel_all.today_events(date=dt.date(2026, 1, 5))}))
    rest1 = [{"order_id": "o1", "ticker": "KXINX-26SEP23H1600-B6600", "side": "bid", "price_dollars": "0.4100"},
             {"order_id": "o2", "ticker": "KXINXU-26SEP23H1100-B6600", "side": "ask", "yes_price": 47}]
    rest2 = [{"order_id": "o3", "ticker": "KXNASDAQ100U-26SEP23H1500-B24000", "side": "bid"}]
    call("cancel_events", lambda: cancel_all.cancel_events(ev), {"events": ev, "verbose": True},
         [ok(body={"orders": rest1}), ok(body={"orders": rest2}),
          ok(200, {"order": {"order_id": "o1"}, "reduced_by": 20}),
          ok(404, '{"error":{"code":"not_found"}}'),
          ok(200, {"order": {"order_id": "o3"}, "reduced_by": 20}),
          ok(body={"orders": [{"order_id": "o2"}]}), ok(body={"orders": []})], stdout=True)
    call("cancel_events", lambda: cancel_all.cancel_events(ev[:3], verbose=False),
         {"events": ev[:3], "verbose": False},
         [ok(body={"orders": []}), ok(body={"orders": []})], stdout=True)
    call("cancel_events", lambda: cancel_all.cancel_events([]), {"events": [], "verbose": True}, [], stdout=True)
    call("resting_across", lambda: cancel_all.resting_across(ev), {"events": ev},
         [ok(body={"orders": rest1, "cursor": "more"}), ok(body={"orders": [{"order_id": "o9"}]}),
          ok(body={"orders": None})])
    emit(out, "kalshi_client", "kalshi/client.py", L)


if __name__ == "__main__":
    main()
