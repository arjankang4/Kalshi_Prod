#!/bin/bash
# Are the golden vectors on disk still describing the Python they were taken from?
#
# Every .vec header carries `source=<rel-path> sha=<16 hex>` written by tools/gen_golden.py at
# generation time, and until 2026-08-26 NOTHING compared it to anything. That gap was live: the
# edge curve was refitted and pricing/edge.py changed on 2026-08-25, and the only symptom was
# `varcal_edge` failing with 66,471 unexplained value mismatches. Worse is the silent direction --
# a suite whose Python moved in a way the C++ happens to still satisfy passes, and the port banks
# a parity claim it has not earned.
#
# This is one test rather than five lines in each of ten, because it must cover suites nobody
# thought to add it to. Run before the replays, as a ctest fixture, so a stale tree fails with one
# line saying so instead of a wall of diffs.
set -u
GOLDEN="${1:?usage: check_vectors.sh <golden-dir> <kalshi-root>}"
REF="${2:?usage: check_vectors.sh <golden-dir> <kalshi-root>}"

shopt -s nullglob
vecs=("$GOLDEN"/*.vec)
if [ ${#vecs[@]} -eq 0 ]; then
    echo "FATAL: no .vec files in $GOLDEN -- run 'make vectors'"
    exit 1
fi

rc=0
for f in "${vecs[@]}"; do
    hdr=$(head -1 "$f")
    name=$(sed -n 's/.*suite=\([^ ]*\).*/\1/p' <<<"$hdr")
    src=$(sed -n 's/.*source=\([^ ]*\).*/\1/p' <<<"$hdr")
    vsha=$(sed -n 's/.*sha=\([^ ]*\).*/\1/p' <<<"$hdr")
    # source=- is legitimate: `median`, `skew_boards` and `json_roundtrip` are not derived from a
    # single Python module, so there is nothing to hash. They are covered by regeneration, not by
    # this check.
    if [ "$src" = "-" ]; then
        printf "  %-16s (no single source)\n" "$name"
        continue
    fi
    if [ ! -f "$REF/$src" ]; then
        printf "  %-16s MISSING SOURCE %s\n" "$name" "$REF/$src"; rc=1; continue
    fi
    tsha=$(sha256sum "$REF/$src" | cut -c1-16)
    if [ "$vsha" = "$tsha" ]; then
        printf "  %-16s ok   %s\n" "$name" "$src"
    else
        printf "  %-16s STALE  %s\n" "$name" "$src"
        printf "  %-16s        vectors were taken from %s, the file is now %s\n" "" "$vsha" "$tsha"
        rc=1
    fi
done

if [ $rc -ne 0 ]; then
    echo
    echo "The golden vectors predate the Python they claim to describe. Run 'make vectors'."
    echo "If they are already fresh, the reference moved and this port has to move with it --"
    echo "diff the module before assuming a regeneration is all that is needed."
fi
exit $rc
