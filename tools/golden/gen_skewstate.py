#!/usr/bin/env python3
"""strategy/skewstate.py -- SkewState: the refit cadence, the label-keyed queue, the smoothed curve.

Scripts of observe() calls against a state carried across "cycles", with the monotonic clock
driven from here. After every call the SERVED curve is dumped -- flat/ok/reason/n_buckets and
p_above(K) + ratio(K) on the listed grid and off it -- together with the whole `detail()` dict as
json.dumps text (queue depths per label, reasons, n_fits). p_above on the grid is the corrected
digital WITH the monotone projection, which is only armed when the state passes the listed grid
into the SkewFit -- the 2026-08-24 finding -- so a port that dropped `strikes=` goes red here.

Inputs:
  * the reference test's synthetic boards (flat vol, skewed vol, one flickering quote, a spot
    move of +40 points under the same labels, a board too sparse to fit);
  * the two real captured boards, and randomized perturbations of them (quotes pulled, widened,
    crossed, bumped) so the raw fit refuses, partially fits and fully fits across a session;
  * cadence on its exact boundary (t - last == recompute_s), `force`, a clock going backwards,
    the window prune on its boundary (ts == cutoff is KEPT), and `_rebuild` with no grid.

    board <id> <n> <R:K:bp:ap:bs:as>...
    new   <id> <u> <window|None> <recompute|None> <half_life|None>
    obs   <id> <board> <fwd|None> <w_atm|None> <now> <force> <ng> <grid>... <nk> <K>... \t <curve> \t <detail json>
    bare  <id> <fwd> <w_atm> <t> <nk> <K>... \t <curve>
  <curve> = flat ok reason nb then per K: p_above ratio
"""
import argparse, json, math, pathlib, random, sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
import gen_golden  # noqa: E402
from gen_golden import emit, h  # noqa: E402
from pricing import bs, skew as skewmod, theo  # noqa: E402
from strategy import skewstate  # noqa: E402


def on(v):
    return "None" if v is None else h(v)


def J(d):
    return json.dumps(d, separators=(",", ":"))


def synth(F, w_of, n=60, step=5.0, half_spread=0.10, size=10, bump=None):
    strikes = [F + step * i for i in range(-n, n + 1)]
    q = {}
    for k in strikes:
        w = w_of(k)
        for cp in ("C", "P"):
            mid = bs.bs_price(F, k, w, cp)
            if bump and cp == "C" and abs(k - bump[0]) < 1e-9:
                mid = max(mid + bump[1], 0.01)
            bid = mid - half_spread
            q[(cp, k)] = {"bp": bid if bid > 0 else 0.0, "ap": mid + half_spread,
                          "bs": size if bid > 0 else 0, "as": size}
    return q, strikes


def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--out", default="tests/data/golden")
    out = pathlib.Path(ap.parse_args().out)
    L = []
    boards = {}      # id -> (quotes, strikes)
    states = {}

    def board(bid, q, strikes):
        toks = []
        for (r, k), fr in q.items():
            toks.append(f"{r}:{h(k)}:{on(fr.get('bp'))}:{on(fr.get('ap'))}:"
                        f"{on(fr.get('bs'))}:{on(fr.get('as'))}")
        boards[bid] = (q, list(strikes))
        L.append(f"board {bid} {len(toks)} " + " ".join(toks))

    def curve_str(c, ks):
        parts = [("flat" if c.flat else "fit"), ("1" if c.ok else "0"), (c.reason or "-"),
                 str(c.n_buckets)]
        for k in ks:
            parts.append(on(c.p_above(k)))
            parts.append(h(c.ratio(k)))
        return " ".join(parts)

    def probe_ks(strikes, F):
        ks = sorted(strikes)[::4]
        if F:
            ks += [F, F + 2.5, F - 7.3, F * 1.01, F * 0.99]
        return ks

    def new(i, u="SPX", win=None, rec=None, hl=None):
        states[i] = skewstate.SkewState(u, window_s=win, recompute_s=rec, half_life_s=hl)
        L.append(f"new {i} {u} {on(win)} {on(rec)} {on(hl)}")

    def obs(i, bid, F, wa, now, force=False, strikes=None):
        q, ks0 = boards[bid]
        st = states[i]
        grid = ks0 if strikes is None else strikes
        c = st.observe(q, grid, F, wa, now=now, force=force)
        ks = probe_ks(ks0, F)
        L.append(f"obs {i} {bid} {on(F)} {on(wa)} {h(now)} {1 if force else 0} "
                 f"{len(grid)} " + " ".join(h(k) for k in grid) + f" {len(ks)} "
                 + " ".join(h(k) for k in ks) + f"\t{curve_str(c, ks)}\t{J(st.detail())}")

    def bare(i, F, wa, t):
        c = states[i]._rebuild(F, wa, t)
        ks = probe_ks(boards[0][1], F)
        L.append(f"bare {i} {h(F)} {h(wa)} {h(t)} {len(ks)} " + " ".join(h(k) for k in ks)
                 + f"\t{curve_str(c, ks)}")

    F0, W0 = 7700.0, 0.0055
    flat = lambda k: W0                                          # noqa: E731
    q, s = synth(F0, flat); board(0, q, s)

    # 1. cadence, incl. the exact boundary and force
    new(1, rec=5.0)
    for t, f in ((100.0, False), (102.0, False), (105.0, False), (106.0, False), (106.5, True),
                 (111.5, False), (111.4999, False)):
        obs(1, 0, F0, W0, t, f)
    # 2. one sample is not an average
    new(2)
    obs(2, 0, F0, W0, 0.0); obs(2, 0, F0, W0, 5.0)
    # 3. the window prunes; ts == cutoff is kept, ts < cutoff is not
    new(3, win=30.0)
    for i in range(8):
        obs(3, 0, F0, W0, float(i * 5))
    obs(3, 0, F0, W0, 65.0); obs(3, 0, F0, W0, 200.0); obs(3, 0, F0, W0, 204.0)
    # 4. one flickering quote
    new(4, win=30.0, rec=5.0, hl=10.0)
    for i in range(6):
        obs(4, 0, F0, W0, float(i * 5))
    base = skewmod.fit(q, s, F0, W0)
    k_bad = sorted(base.buckets, key=lambda b: abs(b["x"]))[3]["strike"]
    qb, sb = synth(F0, flat, bump=(k_bad, 3.0)); board(1, qb, sb)
    obs(4, 1, F0, W0, 30.0)
    obs(4, 0, F0, W0, 35.0)
    # 5. a spot move under the same labels
    new(5, win=60.0, rec=1.0)
    for i in range(4):
        obs(5, 0, F0, W0, float(i))
    q1, s1 = synth(F0 + 40.0, flat); board(2, q1, s1)
    obs(5, 2, F0 + 40.0, W0, 5.0)
    # 6. failing soft
    new(6)
    board(3, {("C", F0): {"bp": 1.0, "ap": 2.0, "bs": 5, "as": 5}}, [F0])
    obs(6, 3, F0, W0, 0.0)
    # (forward None is not probed: SkewFit.p_above raises on it in the reference, and
    # validate_theo never observes without a truthy spot.)
    obs(6, 0, F0, None, 2.0, True); obs(6, 0, F0, 0.0, 3.0, True)
    # 7. a real skew through the smoother
    def w_skewed(k):
        x = math.log(k / F0) / W0
        return W0 * max(0.35, 1.0 - 0.08 * x)
    qs, ss = synth(F0, w_skewed); board(4, qs, ss)
    new(7, rec=1.0)
    for i in range(6):
        obs(7, 4, F0, W0, float(i))
    # 8. the served curve carries the grid; and _rebuild with no grid
    w_sk = lambda k: W0 * (1.0 + 0.30 * max(0.0, -math.log(k / F0) / W0) / 6.0)  # noqa: E731
    q8, s8 = synth(F0, w_sk); board(5, q8, s8)
    new(8)
    for i in range(6):
        obs(8, 5, F0, W0, float(i))
    bare(8, F0, W0, 5.0)
    bare(8, F0, W0, 5.0 + 25.0)
    # clock going backwards: due is False (t - last < recompute), weights > 1
    new(9, rec=5.0)
    obs(9, 4, F0, W0, 100.0); obs(9, 4, F0, W0, 106.0); obs(9, 4, F0, W0, 90.0)
    # queue passed a DIFFERENT grid than the board carries (grid ignored by the quotes)
    new(10)
    obs(10, 4, F0, W0, 0.0, strikes=ss[::2]); obs(10, 4, F0, W0, 5.0, strikes=ss[::2])

    # --- real captured boards, and randomized perturbations of them, as a session ---
    real = gen_golden._real_boards()
    rnd = random.Random(424242)
    nb = 10
    for ri, (rb, rs) in enumerate(real):
        pr = theo.Pricer(rb, rs)
        F = pr.spot
        wa = skewmod.atm_vol(rb, rs, F)
        board(nb, rb, rs); base_id = nb; nb += 1
        for sess in range(12):
            sid = 100 + ri * 100 + sess
            new(sid, rnd.choice(("SPX", "NDX")), rnd.choice((None, 30.0, 12.0)),
                rnd.choice((None, 5.0, 2.0)), rnd.choice((None, 10.0, 3.0)))
            t = rnd.uniform(0, 1e5)
            for cyc in range(rnd.randrange(8, 25)):
                t += rnd.choice((2.0, 2.0, 2.0, 1.0, 5.0, 0.3, 40.0))
                r = rnd.random()
                if r < 0.35:
                    obs(sid, base_id, F, wa, t, rnd.random() < 0.1)
                    continue
                # perturb: each leg independently pulled / widened / bumped
                pq = {}
                pull = rnd.choice((0.0, 0.02, 0.3, 0.9))
                for key, fr in rb.items():
                    u = rnd.random()
                    if u < pull:
                        continue
                    fr = dict(fr)
                    if u < pull + 0.05 and fr.get("bp") is not None and fr.get("ap") is not None:
                        fr["ap"] = fr["ap"] + rnd.choice((0.05, 0.5, 3.0))
                    elif u < pull + 0.07 and fr.get("ap") is not None:
                        fr["bp"] = fr["ap"] + 0.05                       # crossed
                    elif u < pull + 0.10:
                        fr.pop("bp", None); fr.pop("bs", None)
                    pq[key] = fr
                board(nb, pq, rs)
                Fp = F + rnd.gauss(0, 3.0)
                wap = wa * rnd.uniform(0.95, 1.05) if rnd.random() > 0.03 else None
                obs(sid, nb, Fp, wap, t, rnd.random() < 0.1)
                nb += 1
    emit(out, "skewstate", "strategy/skewstate.py", L)


if __name__ == "__main__":
    main()
