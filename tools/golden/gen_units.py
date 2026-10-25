#!/usr/bin/env python3
"""storage/units.py -- fixed-point strings to int64 micro-dollars / centi-contracts.

Every price and size on the WebSocket book goes through these two functions, and a book is only as
right as they are. The sweep is every price level a Kalshi market can carry (the tick is a cent,
deci- and centi-cent markets exist, so every multiple of 1e-6 in [0, 1] that the API could spell),
every size shape, and the edges of Decimal's grammar -- including the one that is easy to get
wrong: `scaleb` rounds to the 28-digit context BEFORE the integrality test.

Values outside int64 are deliberately NOT swept: Python ints are unbounded and the port raises
OverflowError there (unreachable from the API: a price is <= 1 dollar, a size < 1e17 contracts).

Line format: `<fn> <json-of-input> <result>` where result is the int, `None`, or
`!<ExceptionType>!<message>` with spaces in the message escaped as \\x20 so the line splits.
"""
import argparse, json, pathlib, random, sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
from gen_golden import emit  # noqa: E402
from storage import units  # noqa: E402


def res(fn, x):
    try:
        r = fn(x)
        return "None" if r is None else str(r)
    except Exception as e:                                   # noqa: BLE001
        return f"!{type(e).__name__}!{str(e)}".replace("\\", "\\x5c").replace(" ", "\\x20")


def jd(v):
    return json.dumps(v).replace(" ", "\\u0020")


def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--out", default="tests/data/golden")
    out = pathlib.Path(ap.parse_args().out)
    rng = random.Random(4242)
    L = []
    inputs = []
    # every cent and a random sample of every micro-dollar spelling Kalshi uses
    inputs += [f"{c / 100:.4f}" for c in range(0, 101)]
    inputs += [f"{rng.randint(0, 1_000_000) / 1e6:.6f}" for _ in range(3000)]
    inputs += [f"{rng.randint(0, 10**9) / 100:.2f}" for _ in range(3000)]           # sizes
    inputs += [f"-{rng.randint(0, 10**7) / 100:.2f}" for _ in range(500)]           # delta_fp
    inputs += ["0", "1", "0.5", "0.50", "0.500000", "0.5000001", "0.1234567", "1e-6", "1E-7",
               "1e2", "-0", "-0.00", "+0.01", ".5", "5.", "1_000", "_1", "1_", "1._5",
               " 0.55 ", "\t1\n", "0.55 ", "", "  ", "abc", "0x10", "1,5", "1e", "e1",
               "Inf", "-Infinity", "nan", "NaN5", "sNaN", "0.1234560000000000000000000000001",
               "0.12345600000000000000000000005", "0.99999999999999999999999999999999",
               "9223372036854.775807", "-9223372036854.775808",
               "0E-1000", "1e-1000", "0.000000000000000000000000000000",
               "12.345", "12.34", "0.001", "0.0001", "100.00", "60261.00", "666.20", "1.5e1"]
    for s in inputs:
        for name, fn in (("micros", units.to_micros), ("cc", units.to_cc)):
            L.append(f"{name} {jd(s)} {res(fn, s)}")
    # non-string JSON values: None, ints, floats (Decimal(float) is the EXACT binary value), bools
    others = [None, 0, 1, 5, -3, 10**12, True, False, 0.5, 0.25, 0.1, 0.01, 1.5, 12.25, 1e-7, 2.0 ** -20,
              123456.789, float("inf"), float("-inf"), float("nan")]
    for v in others:
        for name, fn in (("micros", units.to_micros), ("cc", units.to_cc)):
            L.append(f"{name} {jd(v)} {res(fn, v)}")
    emit(out, "units", "storage/units.py", L)


if __name__ == "__main__":
    main()
