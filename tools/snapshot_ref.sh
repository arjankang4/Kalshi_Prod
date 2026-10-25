#!/usr/bin/env bash
# Freeze the Python reference's CODE into ref/py-<UTC stamp>/ and point ref/current at it.
#
#     tools/snapshot_ref.sh [KALSHI_ROOT]      (default: ../Kalshi, a sibling checkout of the Kalshi repo)
#
# Code is COPIED (so the pin cannot move under the tests). Model artifacts, the capture fixtures and
# secrets are read-only SYMLINKS into the source tree. var/journal, var/risk, var/scratch and data/ws are
# REAL directories inside the snapshot, so running the reference from it never writes into the source tree.
# Prints which .py files differ from the previous pin. Then: make config && make check && make parity.
set -euo pipefail
cd "$(dirname "$0")/.."
K="$(cd "${1:-../Kalshi}" && pwd)"   # absolute, so the symlinks below resolve from ref/
PREV="$(readlink ref/current || true)"
STAMP="$(date -u +%Y%m%dT%H%MZ)"
NEW="ref/py-$STAMP"
mkdir -p "$NEW/storage" "$NEW/var"/{journal,log,scratch,latency,backtest,risk} "$NEW/data"
(cd "$K" && rsync -a --exclude __pycache__ --exclude '*.ipynb' config.py harness.py kalshi massive pricing rvol strategy edgecal "$OLDPWD/$NEW/")
for f in __init__ feed_tee book_replay units segments paths s3util; do cp "$K/storage/$f.py" "$NEW/storage/"; done
for d in model edgecal rvol; do ln -sfn "$K/var/$d" "$NEW/var/$d"; done
ln -sfn "$K/data/fixtures" "$NEW/data/fixtures"
for f in .env.kalshi .env.massive kalshi_key.txt; do ln -sfn "$K/$f" "$NEW/$f"; done
(cd "$NEW" && find . -name '*.py' | sort | xargs sha256sum) > "$NEW.sha256"
ln -sfn "py-$STAMP" ref/current
echo "pinned ref/current -> py-$STAMP"
if [ -n "$PREV" ] && [ -f "ref/$PREV.sha256" ]; then
    echo "changed since $PREV:"
    diff <(sort -k2 "ref/$PREV.sha256") <(sort -k2 "$NEW.sha256") | awk '/^[<>]/{print $3}' | sort -u | sed 's/^/  /' || true
fi
