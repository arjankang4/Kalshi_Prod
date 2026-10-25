#!/usr/bin/env python3
"""rvol/model.py -- the fitted log-HAR artifact and its predictions.

Both LIVE artifacts (var/model/rvol/{SPX,NDX}.json) plus synthetic ones built to reach every
branch: no sigma_resid (the median fallback), sigma_resid 0 / NaN / negative, a single horizon (the
nearest-horizon path whatever `blend` says), an unordered horizon set, and a model missing a
horizon entirely.

Feature dicts are random but realistic (annualized sigmas 2%-60%, proxy fractions in [0, 1]) and
then damaged: NaN, None, zero, negative, missing keys, a missing `pf_3h` (defaults to 0.0), inf.
Every horizon is predicted directly, and predict_for_minutes is swept across the fitted range,
its ends, the blend boundaries and the degenerate minutes (None, 0, negative), both blended and
nearest.

Features are emitted as `k=<hex|None>` tokens; an absent key is simply not emitted.
"""
import argparse, json, math, pathlib, sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
from gen_golden import emit, h, config  # noqa: E402

import numpy as np  # noqa: E402
from rvol import fit as rfit  # noqa: E402
from rvol import model as rmodel  # noqa: E402

KEYS = ["f_3h", "f_1h", "f_30m", "f_15m", "f_5m", "pf_3h"]


def on(v):
    return "None" if v is None else h(v)


def feat_tokens(f):
    return " ".join(f"{k}={on(v)}" for k, v in f.items())


def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--out", default="tests/data/golden")
    out = pathlib.Path(ap.parse_args().out)
    L = []
    rng = np.random.default_rng(424242)

    arts = {}
    for u in ("SPX", "NDX"):
        arts[u] = rfit.load(u)
        L.append(f"load {u}")
    base = arts["SPX"]

    def variant(name, mutate):
        a = json.loads(json.dumps(base))
        mutate(a)
        a["underlying"] = name
        arts[name] = a
        L.append(f"art {name} {json.dumps(a, separators=(',', ':'))}")

    variant("NORESID", lambda a: [m.pop("sigma_resid") for m in a["models"].values()])
    variant("ZERORESID", lambda a: [m.__setitem__("sigma_resid", 0.0) for m in a["models"].values()])
    variant("NEGRESID", lambda a: [m.__setitem__("sigma_resid", -0.1) for m in a["models"].values()])
    variant("ONEH", lambda a: a.__setitem__("models", {"30m": a["models"]["30m"]}))
    variant("SHUFFLED", lambda a: a.__setitem__(
        "models", {k: a["models"][k] for k in ("7.5m", "60m", "30m")}))
    variant("TWOH", lambda a: a.__setitem__(
        "models", {k: a["models"][k] for k in ("60m", "7.5m")}))
    variant("NOEXTRA", lambda a: (a.__setitem__("extra", []),
                                  [m.__setitem__("beta", m["beta"][:5] + m["beta"][6:])
                                   for m in a["models"].values()]))

    models = {k: rmodel.RealizedVolModel(a) for k, a in arts.items()}
    for k, m in models.items():
        L.append(f"horizons {k} {' '.join(m.horizons())}")
        for hz in list(m.models) + ["15m"]:
            ap_, fac = m.retransform(hz)
            L.append(f"retransform {k} {hz} {1 if ap_ else 0} {h(fac)}")

    MINUTES = [None, 0.0, -5.0, 1.0, 5.0, 7.5, 7.4999999, 7.5000001, 8.0, 10.0, 12.345, 29.99,
               30.0, 30.01, 40.0, 44.9, 45.0, 45.1, 59.99, 60.0, 60.5, 120.0, 390.0]
    n_feats = 0
    for i in range(3000):
        f = {k: float(np.exp(rng.uniform(math.log(0.02), math.log(0.6)))) for k in KEYS[:5]}
        f["pf_3h"] = float(rng.choice([0.0, rng.uniform(0, 1), 1.0]))
        f["cov_3h"] = float(rng.uniform(0.4, 1.0))
        r = rng.random()
        if r < 0.25:
            k = str(rng.choice(KEYS))
            f[k] = rng.choice([math.nan, None, 0.0, -0.1, math.inf, -0.0])
            f[k] = None if f[k] is None else float(f[k])
        elif r < 0.30:
            f.pop(str(rng.choice(KEYS)))
        L.append(f"feats {i} {feat_tokens(f)}")
        n_feats += 1
        for k, m in models.items():
            if k not in ("SPX", "NDX") and i % 7:
                continue
            for hz in list(m.models) + ["15m"]:
                L.append(f"predict {k} {i} {hz} {on(m.predict(hz, f))}")
            mins = MINUTES if i % 5 == 0 else [None] + [float(v) for v in rng.uniform(0.1, 400, 6)]
            for mn in mins:
                for blend in (True, False):
                    s, lab = m.predict_for_minutes(mn, f, blend=blend)
                    L.append(f"pfm {k} {i} {on(mn)} {1 if blend else 0} {on(s)} {lab or '-'}")
    emit(out, "rvol_model", "rvol/model.py", L)


if __name__ == "__main__":
    main()
