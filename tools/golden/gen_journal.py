#!/usr/bin/env python3
"""strategy/journal.py -- the JSONL writer, compared on the FILE BYTES it leaves on disk.

The real `open_journal` / `Journal.write` / `close` are run with JOURNAL_DIR pointed at a temp
dir (never the reference tree), over records that exercise everything json.dumps(...,
separators=(",", ":"), default=str) can do on the live path: nested dicts and lists (tuples),
key order, int vs float, 1e16 / 1e-05 / -0.0 spellings, NaN/Infinity, nanosecond ints, bools,
None, non-ASCII and control characters (ensure_ascii escapes), and datetimes, which reach
`default=str` and are written as str(datetime). 120 rows cross FSYNC_EVERY twice; the journal is
then closed and REOPENED under the same stamp, which must append, not truncate.

Each row carries the record in an encoding the C++ can rebuild (NaN/inf and datetimes as marker
objects) and the exact line Python wrote. The last line carries the whole file's length and
sha256, and the relative path the file landed at.

    row  <i>\t<encoded record>\t<expected line>
    reopen
    file <relpath> <bytes> <sha256>
    iso  <ns> <isoformat ET> <isoformat UTC> \t <str ET> \t <str UTC>
"""
import argparse, datetime as dt, hashlib, json, math, pathlib, random, sys, tempfile
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
from gen_golden import emit, config  # noqa: E402
from strategy import journal as jnl  # noqa: E402

UTC = dt.timezone.utc
EPOCH = dt.datetime(1970, 1, 1, tzinfo=UTC)


def ns_of(t):
    d = t - EPOCH
    return (d.days * 86400 + d.seconds) * 1_000_000_000 + d.microseconds * 1000


def enc(v):
    if isinstance(v, dt.datetime):
        return {"__dt__": ns_of(v), "tz": "utc" if v.utcoffset() == dt.timedelta(0) else "et"}
    if isinstance(v, float) and not math.isfinite(v):
        return {"__f__": "nan" if v != v else ("inf" if v > 0 else "-inf")}
    if isinstance(v, dict):
        return {k: enc(x) for k, x in v.items()}
    if isinstance(v, (list, tuple)):
        return [enc(x) for x in v]
    return v


def records(rnd):
    t_et = dt.datetime(2026, 8, 21, 10, 30, 0, 123456, tzinfo=config.ET)
    base = [
        {"v": 2, "ts_ns": 1787327428284123456, "live": False, "ticker": "KXINX-26AUG21H1600-B7687",
         "side": "bid", "rung": 0, "price": 0.43, "count": 20, "edge": 0.0125, "rung_min_edge": 0.01,
         "rule": "improve_1t", "theo": 0.4425, "best_bid": 0.42, "best_ask": 0.45, "R": None,
         "required_edge": 0.01, "risk_u": 0.5, "edge_source": "curve", "theo_vol": 0.44,
         "p_rv": None, "lean_cents": -0.5, "delta_adj": 0.0, "f_move": -0.0, "underlying": "SPX",
         "spot": 7695.123456789, "spot_source": "fast_futures", "sigma": 0.1234,
         "skew_smoothed": True},
        {"floats": [0.1, 1e16, 1e-05, 123456789012345.6, 5e-324, 1.7976931348623157e308, -0.0,
                    2.0, 1e22, 0.30000000000000004]},
        {"nonfinite": [float("nan"), float("inf"), float("-inf")]},
        {"ints": [0, -1, 2**53 + 1, -(2**63), 2**63 - 1]},
        {"s": "café ☃ \U0001F600 \"q\" \\ \n\t\r\b\f \x00\x1f\x7f /"},
        {"nested": {"a": {"b": [1, (2, 3), {"c": None}]}, "empty": {}, "el": []}},
        {"dt_utc": dt.datetime(2026, 8, 21, 20, 0, tzinfo=UTC), "dt_et": t_et,
         "in_list": [t_et, dt.datetime(2026, 3, 8, 3, 0, 1, tzinfo=config.ET)]},
        {},
    ]
    out = list(base)
    keys = ["ticker", "side", "price", "theo", "edge", "R", "spot", "sigma", "n", "ok", "ts_ns"]
    while len(out) < 120:
        r = {}
        for k in rnd.sample(keys, rnd.randrange(1, len(keys))):
            c = rnd.random()
            if c < 0.3:
                r[k] = rnd.uniform(-1e4, 1e4)
            elif c < 0.45:
                r[k] = rnd.randint(-10**12, 10**18)
            elif c < 0.55:
                r[k] = None
            elif c < 0.65:
                r[k] = rnd.random() < 0.5
            elif c < 0.8:
                r[k] = "".join(rnd.choice("abcXYZ-_:.é☃") for _ in range(rnd.randrange(0, 12)))
            elif c < 0.9:
                r[k] = [rnd.uniform(0, 1) for _ in range(rnd.randrange(0, 4))]
            else:
                r[k] = t_et + dt.timedelta(microseconds=rnd.randrange(0, 10**12))
        out.append(r)
    return out


def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--out", default="tests/data/golden")
    out = pathlib.Path(ap.parse_args().out)
    rnd = random.Random(1717)
    L = []
    recs = records(rnd)
    with tempfile.TemporaryDirectory() as td:
        real_dir = jnl.JOURNAL_DIR
        jnl.JOURNAL_DIR = pathlib.Path(td) / "var" / "journal"
        try:
            stamp = "20260821T143000Z"
            j = jnl.open_journal(stamp, name="quotes.jsonl")
            written = 0
            for i, r in enumerate(recs[:117]):
                before = j.path.stat().st_size if j.path.exists() else 0
                j.write(r)
                j._fh.flush()
                line = j.path.read_bytes()[before:].decode("ascii")
                assert line.endswith("\n")
                L.append(f"row {i}\t{json.dumps(enc(r))}\t{line[:-1]}")
                written += 1
            assert j.n == 117
            j.close()
            L.append("reopen")
            j = jnl.open_journal(stamp, name="quotes.jsonl")
            for i, r in enumerate(recs[117:], start=117):
                before = j.path.stat().st_size
                j.write(r)
                j._fh.flush()
                line = j.path.read_bytes()[before:].decode("ascii")
                L.append(f"row {i}\t{json.dumps(enc(r))}\t{line[:-1]}")
            j.close()
            data = j.path.read_bytes()
            rel = j.path.relative_to(td)
            L.append(f"file {rel} {len(data)} {hashlib.sha256(data).hexdigest()}")
        finally:
            jnl.JOURNAL_DIR = real_dir

    # isoformat / str of aware datetimes, both zones, with and without microseconds, across DST
    ts = [dt.datetime(2026, 8, 21, 14, 30, tzinfo=UTC), dt.datetime(2026, 3, 8, 6, 59, 59, tzinfo=UTC),
          dt.datetime(2026, 3, 8, 7, 0, 0, 1, tzinfo=UTC), dt.datetime(2026, 11, 1, 5, 30, tzinfo=UTC),
          dt.datetime(2026, 11, 1, 6, 30, tzinfo=UTC), dt.datetime(2026, 12, 31, 23, 59, 59, 999999, tzinfo=UTC),
          dt.datetime(2024, 2, 29, 4, 0, tzinfo=UTC)]
    for _ in range(3000):
        ts.append(EPOCH + dt.timedelta(microseconds=rnd.randrange(1_600_000_000 * 10**6,
                                                                   1_900_000_000 * 10**6)))
    for t in ts:
        e = t.astimezone(config.ET)
        L.append(f"iso {ns_of(t)} {e.isoformat()} {t.isoformat()}\t{str(e)}\t{str(t)}")
    emit(out, "journal", "strategy/journal.py", L)


if __name__ == "__main__":
    main()
