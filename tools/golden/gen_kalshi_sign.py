#!/usr/bin/env python3
"""kalshi/client._sign -- RSA-PSS(SHA-256, MGF1-SHA-256, salt = 32) over `{ts}{METHOD}{path}`.

PSS is randomized, so the port cannot be compared byte for byte. What CAN be pinned is that the two
sides agree on every parameter: signatures made HERE, by the reference's own `_sign`, must verify
in C++ under salt length 32 exactly (and must NOT verify under 20 or under PKCS#1 v1.5), and the
C++ signer's output must verify under the same pinned parameters. The live check -- an authenticated
GET answered 200 by Kalshi -- is the end-to-end proof; this is the offline one.

A throwaway 2048-bit key is minted per run, in the same "BEGIN RSA PRIVATE KEY" (traditional
OpenSSL) PEM form as the production key file, and written beside the vectors. It is never the real
key: the real key must not be copied into a build tree.
"""
import argparse, pathlib, random, sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))
from gen_golden import emit  # noqa: E402
from cryptography.hazmat.primitives import serialization  # noqa: E402
from cryptography.hazmat.primitives.asymmetric import rsa  # noqa: E402
from kalshi import client as k  # noqa: E402


def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--out", default="tests/data/golden")
    out = pathlib.Path(ap.parse_args().out)
    out.mkdir(parents=True, exist_ok=True)
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    pem = key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.TraditionalOpenSSL,
                            serialization.NoEncryption())
    (out / "kalshi_sign_key.pem").write_bytes(pem)
    k._private_key = lambda: key                      # _sign() signs with whatever this returns
    rng = random.Random(99)
    L = [f"key {out / 'kalshi_sign_key.pem'}"]
    paths = ["/trade-api/v2/portfolio/orders", "/trade-api/v2/portfolio/positions",
             "/trade-api/v2/account/limits", "/trade-api/v2/portfolio/events/orders/batched",
             "/trade-api/v2/portfolio/events/orders/0e1f-ab", "/trade-api/ws/v2", "/"]
    for i in range(60):
        ts = str(1790000000000 + rng.randint(0, 10**9))
        method = rng.choice(["GET", "get", "POST", "DELETE", "Delete"])
        path = rng.choice(paths)
        L.append(f"sig {ts} {method} {path} {k._sign(ts, method, path)}")
    emit(out, "kalshi_sign", "kalshi/client.py", L)


if __name__ == "__main__":
    main()
