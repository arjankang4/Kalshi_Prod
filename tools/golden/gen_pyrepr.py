#!/usr/bin/env python3
"""Vectors for kp::pycompat::py_float_repr / py_json_float: `<hex> <repr> <json>` per line."""
import argparse, json, math, pathlib, random, struct, sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
from gen_golden import emit  # noqa: E402

ap = argparse.ArgumentParser(); ap.add_argument("--out", default="tests/data/golden")
out = pathlib.Path(ap.parse_args().out)
rnd = random.Random(20260923)
xs = [0.0, -0.0, 1.0, -1.0, 2.0, 0.1, 0.005, 0.015, 1e16, 1e15, 9999999999999998.0, 1e-4, 1e-5,
      0.0001, 0.00011, 123456789012345678.0, 5e-324, 1.7976931348623157e308, float("nan"),
      float("inf"), float("-inf"), 2.5, 1/3, 250.0, 0.3333333333333333, 1e22, 1e23, 1.5e-7]
xs += [rnd.uniform(-1, 1) for _ in range(20000)]
xs += [round(rnd.uniform(0, 1), 2) for _ in range(5000)]            # cent prices
xs += [rnd.uniform(-1e6, 1e6) for _ in range(5000)]
xs += [struct.unpack("<d", struct.pack("<Q", rnd.getrandbits(64)))[0] for _ in range(20000)]
xs += [10.0 ** e * m for e in range(-12, 24) for m in (1.0, 1.5, 9.99)]
L = [f"repr {x.hex() if math.isfinite(x) else repr(x)} {repr(x)} {json.dumps(x)}" for x in xs]
emit(out, "pyrepr", None, L)
