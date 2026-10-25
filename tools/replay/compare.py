#!/usr/bin/env python3
"""Diff a Python trace and a C++ trace cycle by cycle, key by key. Exit 1 on any difference.

    python tools/replay/compare.py PY.jsonl CXX.jsonl [--max 20]

Values are compared EXACTLY -- floats included; both sides wrote them as repr() -- so "equal"
here means bit-identical. The first differences per top-level key are printed with their path.
"""
import argparse, json, sys


def walk(a, b, path, out, limit):
    if len(out) >= limit:
        return
    if type(a) is not type(b) and not (isinstance(a, (int, float)) and isinstance(b, (int, float))
                                        and type(a) is not bool and type(b) is not bool):
        out.append((path, a, b)); return
    if isinstance(a, dict):
        for k in list(a) + [k for k in b if k not in a]:
            if k not in a or k not in b:
                out.append((f"{path}.{k}", a.get(k, "<absent>"), b.get(k, "<absent>")))
            else:
                walk(a[k], b[k], f"{path}.{k}", out, limit)
    elif isinstance(a, list):
        if len(a) != len(b):
            out.append((f"{path}[len]", len(a), len(b)))
        for i, (x, y) in enumerate(zip(a, b)):
            walk(x, y, f"{path}[{i}]", out, limit)
    elif a != b or type(a) is not type(b):
        out.append((path, a, b))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("py"); ap.add_argument("cxx"); ap.add_argument("--max", type=int, default=20)
    a = ap.parse_args()
    P = [json.loads(l) for l in open(a.py)]
    C = [json.loads(l) for l in open(a.cxx)]
    if len(P) != len(C):
        print(f"cycle count differs: py {len(P)} cxx {len(C)}")
    by_key, shown, n_bad = {}, 0, 0
    for p, c in zip(P, C):
        diffs = []
        walk(p, c, "", diffs, 10**6)
        if diffs:
            n_bad += 1
        for path, x, y in diffs:
            key = path.split(".")[1].split("[")[0] if path.startswith(".") else path
            by_key[key] = by_key.get(key, 0) + 1
            if shown < a.max:
                print(f"cycle {p['i']:3d} {path}\n      py : {json.dumps(x)[:300]}\n      cxx: {json.dumps(y)[:300]}")
                shown += 1
    print(f"\n{len(P)} cycles, {n_bad} differ; differences by key: {by_key or 'none'}")
    sys.exit(1 if (n_bad or len(P) != len(C)) else 0)


if __name__ == "__main__":
    main()
