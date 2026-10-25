#!/usr/bin/env python3
"""Vectors for kp::json::dumps -- json.dumps in the three styles the live path uses.

Each line is TAB-separated (the payloads contain spaces):
    dumps <style> <input json> <python json.dumps output>
The C++ parses <input> with kp::json::parse, dumps it in <style>, and must reproduce the output
byte for byte. The input is itself Python's default dumps, so parse -> dumps is a round trip of
Python's own text: ints stay ints (0 vs 0.0), floats keep their repr, key order is insertion.
"""
import argparse, json, pathlib, random, sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
from gen_golden import emit  # noqa: E402

STYLES = {"default": {}, "compact": {"separators": (",", ":")}, "indent1": {"indent": 1},
          "indent2": {"indent": 2}}
rnd = random.Random(7)
ALPH = ["a", "Z", "0", " ", '"', "\\", "/", "\n", "\t", "\r", "\b", "\f", "\x00", "\x1f", "\x7f",
        "é", "€", "中", "😀", " ", "~", "{", "}"]


def rstr():
    return "".join(rnd.choice(ALPH) for _ in range(rnd.randint(0, 8)))


def rval(depth=0):
    k = rnd.randint(0, 9 if depth < 3 else 5)
    if k == 0: return None
    if k == 1: return rnd.random() < 0.5
    if k == 2: return rnd.randint(-10**18, 10**18)
    if k == 3: return rnd.choice([0, 1, -1, 1787241600000000000, 2**62])
    if k == 4: return rnd.choice([0.0, -0.0, 1.0, 0.1, 1e16, 1e-5, 2.5, rnd.uniform(-1e6, 1e6),
                                  round(rnd.uniform(0, 1), 2), 1e300])
    if k == 5: return rstr()
    if k in (6, 7): return [rval(depth + 1) for _ in range(rnd.randint(0, 4))]
    return {rstr() + str(i): rval(depth + 1) for i in range(rnd.randint(0, 4))}


cases = [{"v": 2, "ts_ns": 1787241600123456789, "live": False, "ticker": "KXINX-26AUG20H1600-B7700",
          "side": "bid", "rung": 0, "price": 0.42, "count": "20.00", "edge": 0.015, "theo": None},
         {"action": "auth", "params": "k"}, {"id": 1, "cmd": "subscribe",
         "params": {"channels": ["orderbook_delta"], "market_tickers": ["A", "B"]}},
         [], {}, [[]], [{}], "x", 1, 1.0, None, True]
cases += [rval() for _ in range(3000)]
L = []
for c in cases:
    src = json.dumps(c)
    for name, kw in STYLES.items():
        L.append("\t".join(["dumps", name, src, json.dumps(c, **kw).replace("\n", "\x01")]))
ap = argparse.ArgumentParser(); ap.add_argument("--out", default="tests/data/golden")
emit(pathlib.Path(ap.parse_args().out), "jsondumps", None, L)
