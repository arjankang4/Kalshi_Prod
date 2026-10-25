#!/usr/bin/env python3
"""storage/feed_tee.py + storage/segments.py -- the tape the quote loop writes of its own socket.

THE BAR IS THE FILE BYTES. Both writers are driven through the same frames on the same injected
clock, and every file each one leaves on disk -- every `.jsonl.gz` segment and every `.meta.json`
sidecar -- must hash identically. That is stronger than "the same lines", and deliberately so:
`Segment.should_roll` compares the RAW FILE POSITION with the roll size, and that position moves
only when zlib emits output, which depends on the 32 KiB write buffer GzipFile puts in front of the
compressor. A port that gzips the same lines "equivalently" rolls at different frames, and the
tape's segment boundaries -- which the S3 flush, the promotion and every replay key on -- move.

Scenarios:
  tape      10,000 REAL frames (the 2026-08-26 quoter session's head, committed under
            tests/fixtures/kalshi_ws/), rolled by BYTES at 40 kB so the boundary lands wherever
            zlib's output does; Z_SYNC_FLUSH every 20 s of the frames' own clock.
  timeroll  rolled by SECONDS, including idle stretches where a header-only segment must NOT roll.
  drops     a queue of 50 overrun by a burst: counting, and the drop_marker's 5 s pacing.
  odd       frames the exchange does not send but the writer must survive identically: unicode,
            floats, big ints, a non-dict frame, string sids, a null seq, spaces in the raw text.

Line format:
  scenario <name> <session> <queue_max> <roll_bytes> <roll_seconds> <dt> <channel> <sub-json>
  clock <wall_ns> <monotonic-hex>      set both clocks
  offer <recv_ns> <raw>                raw frame text (or hex:<hex> when it has whitespace)
  open                                 FeedTee._open_segment() (start() minus the thread)
  drain                                one iteration of FeedTee._drain's loop body
  stop                                 FeedTee.stop()
  file <relpath> <size> <sha256>       every file on disk after the scenario, sorted
  detail <json>                        FeedTee.detail() after stop
"""
import argparse, gzip, hashlib, json, os, pathlib, queue, random, shutil, sys, tempfile
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
from gen_golden import emit, h  # noqa: E402
import config  # noqa: E402
from storage import feed_tee as ftee  # noqa: E402
from storage import segments  # noqa: E402

FIXTURE = (pathlib.Path(__file__).resolve().parent.parent.parent
           / "tests/fixtures/kalshi_ws/quoter-20260826-bdae7194-head10k.jsonl.gz")


class FakeTime:
    def __init__(self):
        self.ns, self.mono = 0, 0.0

    def time_ns(self):
        return self.ns

    def time(self):
        return self.ns / 1e9

    def monotonic(self):
        return self.mono


class FakeUUID:
    def __init__(self, hx):
        self.hex = hx


def raw_field(raw):
    return ("hex:" + raw.encode().hex()) if any(c.isspace() for c in raw) else raw


def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--out", default="tests/data/golden")
    out = pathlib.Path(ap.parse_args().out)
    clock = FakeTime()
    segments.time = clock
    ftee.time = clock
    real_uuid = ftee.uuid
    L = []

    def run(name, session, events, queue_max=50_000, roll_bytes=64 << 20, roll_seconds=900,
            sub=None, dt="2026-08-26", channel="quoter"):
        tmp = pathlib.Path(tempfile.mkdtemp(prefix="kp-tee-"))
        config.WS_DIR = tmp
        config.SEGMENT_ROLL_BYTES = roll_bytes
        config.SEGMENT_ROLL_SECONDS = roll_seconds
        ftee.uuid = type("U", (), {"uuid4": staticmethod(lambda: FakeUUID(session + "0" * 24))})
        sub = {} if sub is None else sub
        L.append(f"scenario {name} {session} {queue_max} {roll_bytes} {roll_seconds} {dt} {channel} "
                 f"{json.dumps(sub, separators=(',', ':'))}")
        if events and events[0][0] == "clock":       # the segment opens at the scenario's clock
            clock.ns, clock.mono = events[0][1], events[0][2]
        t = ftee.FeedTee(dt=dt, channel=channel, subscribe_state=sub, queue_max=queue_max)
        L.append(f"clock {clock.ns} {h(clock.mono)}")
        t._open_segment()                     # start() without the thread
        L.append("open")
        for ev in events:
            kind = ev[0]
            if kind == "clock":
                clock.ns, clock.mono = ev[1], ev[2]
                L.append(f"clock {clock.ns} {h(clock.mono)}")
            elif kind == "offer":
                t.offer(ev[1], json.loads(ev[2]))
                L.append(f"offer {ev[1]} {raw_field(ev[2])}")
            elif kind == "drain":
                # FeedTee._drain's loop body, verbatim in behaviour, one iteration.
                try:
                    recv_ns, frame = t._q.get_nowait()
                except queue.Empty:
                    t._maybe_roll()
                else:
                    try:
                        t._write(recv_ns, frame)
                    except Exception as e:           # noqa: BLE001
                        t.error = f"write: {type(e).__name__}: {str(e)[:100]}"
                    t._maybe_roll()
                L.append("drain")
        t.stop()
        L.append("stop")
        for p in sorted(tmp.rglob("*")):
            if p.is_file():
                b = p.read_bytes()
                L.append(f"file {p.relative_to(tmp)} {len(b)} {hashlib.sha256(b).hexdigest()}")
        L.append(f"detail {json.dumps(t.detail(), separators=(',', ':'))}")
        shutil.rmtree(tmp)

    # --- tape: real frames, rolled by bytes -----------------------------------------------------
    recs = [json.loads(l) for l in gzip.open(FIXTURE, "rt")]
    t0 = recs[0]["recv_ns"]
    ev = [("clock", t0 - 5_000_000, 1000.0)]
    for i, r in enumerate(recs):
        ns = r["recv_ns"]
        ev.append(("clock", ns, 1000.0 + (ns - t0) / 1e9))
        ev.append(("offer", ns, json.dumps(r["frame"], separators=(",", ":"))))
        ev.append(("drain",))
        if i % 997 == 0:
            ev.append(("drain",))                    # an idle pass: roll check with nothing queued
    run("tape", "bdae7194", ev, roll_bytes=40_000,
        sub={"channels": ["orderbook_delta", "fill", "market_positions"], "n_markets": 1440,
             "series": ["KXINX", "KXINXU", "KXNASDAQ100", "KXNASDAQ100U"], "live": False,
             "run": "20260826T173230Z"})

    # --- timeroll: rolled by seconds, with idle stretches ----------------------------------------
    rng = random.Random(7)
    ns0 = 1_790_000_000_123_456_789
    ev = [("clock", ns0, 50.0)]
    ns, mono = ns0, 50.0
    small = [json.dumps(r["frame"], separators=(",", ":")) for r in recs[1500:1800]]
    for i, raw in enumerate(small):
        step = rng.choice([0.05, 0.3, 1.7, 4.0])
        if i in (40, 41, 120):
            step = 45.0                              # idle: a header-only segment must not roll
            ev.append(("clock", ns + int(step * 1e9) // 2, mono + step / 2))
            ev.append(("drain",))
        ns += int(step * 1e9)
        mono += step
        ev.append(("clock", ns, mono))
        ev.append(("offer", ns, raw))
        ev.append(("drain",))
    for k in range(3):                                # a quiet tail: idle drains past the limit
        ns += 11_000_000_000
        mono += 11.0
        ev.append(("clock", ns, mono))
        ev.append(("drain",))
    run("timeroll", "0badc0de", ev, roll_seconds=10, channel="spot",
        sub={"underlyings": ["SPX", "NDX"], "feeds": ["futures", "index"]})

    # --- drops: a queue of 50 overrun ------------------------------------------------------------
    ev = [("clock", ns0, 3.0)]
    ns, mono = ns0, 3.0
    burst = small[:200]
    for raw in burst[:120]:                            # 120 offered, 50 fit, 70 dropped
        ns += 1_000_000
        ev.append(("offer", ns, raw))
    for _ in range(30):                                # the writer catches up at mono < 5: no marker
        ev.append(("drain",))
    ns += 10_000_000; mono = 9.0
    ev.append(("clock", ns, mono))
    for _ in range(25):                                # now past 5 s: exactly one marker
        ev.append(("drain",))
    for raw in burst[120:200]:                         # second burst while 0 queued: 50 fit again
        ns += 1_000_000
        ev.append(("offer", ns, raw))
    for step in range(60):
        mono += 0.5
        ns += 500_000_000
        ev.append(("clock", ns, mono))
        ev.append(("drain",))
    run("drops", "d0d0d0d0", ev, queue_max=50)

    # --- odd frames ------------------------------------------------------------------------------
    odd = [
        '{"type":"subscribed","id":1,"msg":{"channel":"fill","sid":1}}',
        '{"type": "orderbook_delta", "sid": 3, "seq": 7, "msg": {"market_ticker": "KXINX-X", "price_dollars": "0.5500", "delta_fp": "-3.00", "side": "yes"}}',
        '{"type":"fill","msg":{"market_ticker":"T","yes_price_dollars":"0.42","count_fp":"20.00","post_position_fp":"-20.00","ts":1790000000,"f":1.5,"g":1e-7,"h":-0.0,"i":123456789012345678,"j":true,"k":null}}',
        '{"type":"error","msg":{"code":27,"msg":"caf\\u00e9 \\ud83d\\ude00 é日"}}',
        '["not","a","dict"]',
        '{"type":"ok","sid":"3","seq":null}',
        '{"type":"ok","sid":3.0,"seq":9.5}',
        '{"type":"orderbook_snapshot","sid":3,"seq":5,"msg":{}}',
        '{"type":"orderbook_snapshot","sid":3,"seq":2,"msg":{}}',
        '{"type":"x","sid":false,"seq":1}',
        '{"type":"x","sid":null,"seq":1}',
        '{}',
        '{"type":"orderbook_delta","sid":4,"seq":1,"msg":{"s":"tab\\there\\nnl \\" q \\\\ bs \\u0001 \\u007f"}}',
        '"just a string"',
        '12345',
        '{"nested":{"a":[1,2,[3,{"b":[]}]],"c":{}},"e":1E+2,"f":0.1,"g":100000000000000000000.0}',
    ]
    ev = [("clock", ns0, 100.0)]
    ns = ns0
    for raw in odd:
        ns += 250_000_000
        ev.append(("clock", ns, 100.0 + (ns - ns0) / 1e9))
        ev.append(("offer", ns, raw))
        ev.append(("drain",))
    run("odd", "0dd0dd00", ev, sub={"k": "v", "n": [1, 2.5, None], "u": "é"})

    ftee.uuid = real_uuid
    emit(out, "feed_tee", "storage/feed_tee.py", L)


if __name__ == "__main__":
    main()
