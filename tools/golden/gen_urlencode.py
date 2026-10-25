#!/usr/bin/env python3
"""urllib.parse.quote_plus / urlencode -- the query string of every Kalshi REST read.

The query string is not signed (kalshi/client._sign covers `{ts}{METHOD}{path}` WITHOUT the
query), so a divergence here does not show up as a 401. It shows up as a different REQUEST: a
cursor that does not round-trip, an event_ticker list that names a different event, a `tickers`
param Kalshi validates as one oversized ticker. So the bar is the same as everywhere else: byte for
byte against the reference's own interpreter.

Inputs are emitted as hex of their UTF-8 bytes, because quote() encodes to UTF-8 before escaping and
the C++ side receives exactly those bytes.
"""
import argparse, pathlib, random, sys
import urllib.parse
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
from gen_golden import emit  # noqa: E402


def hx(s):
    return s.encode("utf-8").hex() or "-"


def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--out", default="tests/data/golden")
    out = pathlib.Path(ap.parse_args().out)
    rng = random.Random(20260923)
    L = []
    # Every single code point in the ASCII range, and the reserved/unreserved boundary cases.
    fixed = [chr(i) for i in range(128)] + [
        "", " ", "  ", "+", "%", "%20", "a b+c", "~._-", "KXINX-26SEP23H1600", "KXINX-26SEP23H1600,KXINXU-26SEP23H1000",
        "2026-09-23T16:00:00Z", "abc/def?x=1&y=2#z", "é", "日本", "\U0001F600", " ", " ", "\x7f",
        "cursor:CgsI+/=", "None", "True", "1000", "0.5", "-1e-07",
    ]
    pool = ([chr(c) for c in range(32, 127)] + ["é", "ß", "€", "日", "\U0001F600", " ", "\t", "\n",
                                                 "\x00", "́", "￿", "\U0010FFFF"])
    rand = ["".join(rng.choice(pool) for _ in range(rng.randint(0, 24))) for _ in range(4000)]
    for s in fixed + rand:
        L.append(f"qp {hx(s)} {urllib.parse.quote_plus(s) or '<empty>'}")
    for _ in range(1500):
        n = rng.randint(0, 6)
        pairs = [(rng.choice(fixed + rand), rng.choice(fixed + rand)) for _ in range(n)]
        enc = urllib.parse.urlencode(pairs)
        L.append(f"ue {n} " + " ".join(f"{hx(k)} {hx(v)}" for k, v in pairs) + f" {enc or '<empty>'}")
    # The shapes kalshi/client._request actually builds: a list of pairs (orderbooks' repeated
    # `tickers`), None filtered out, non-str values through str().
    for pairs in ([("tickers", "KXINX-26SEP23H1600-B6600"), ("tickers", "KXINX-26SEP23H1600-T6700"),
                   ("depth", 10)],
                  [("status", "resting"), ("limit", 1000), ("event_ticker", "A,B,C"), ("cursor", None)],
                  [("limit", 200), ("count_filter", "position"), ("cursor", "abc/+=")],
                  [("min_close_ts", 1790000000), ("series_ticker", "KXINXU"), ("flag", True), ("x", 0.1)]):
        kept = [(k, str(v)) for k, v in pairs if v is not None]
        enc = urllib.parse.urlencode([(k, v) for k, v in pairs if v is not None])
        L.append(f"ue {len(kept)} " + " ".join(f"{hx(k)} {hx(v)}" for k, v in kept) + f" {enc}")
    emit(out, "urlencode", None, L)


if __name__ == "__main__":
    main()
