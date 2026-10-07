#!/bin/bash
# Fast bit-exact A/B of a working tree against the baseline host profile (see host_demo.sh): rebuild the candidate, run the demos, compare
# the tic log and the FNV hash of EVERY rendered frame (-ps2ref-hashall) with build/host-base/out (made from the start-of-OPT10 tree).
# usage: host_ab.sh [DEMO_00n ...]
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
ninja -C build/host-cand/b -j${JOBS:-3} 2>&1 | tail -1
EXE=$(ls build/host-cand/b/bin/*)
tools/ps2/host_demo.sh "$EXE" build/host-cand/out "$@"
rc=0
for D in ${@:-DEMO_001 DEMO_002 DEMO_003 DEMO_004}; do
  if cmp -s build/host-base/out/$D/tics.csv build/host-cand/out/$D/tics.csv && cmp -s build/host-base/out/$D/allhash.csv build/host-cand/out/$D/allhash.csv; then
    echo "AB $D: tics and all $(($(wc -l < build/host-cand/out/$D/allhash.csv) - 1)) frame hashes IDENTICAL"
  else
    echo "AB $D: DIFFERENT"; rc=1
    diff build/host-base/out/$D/tics.csv build/host-cand/out/$D/tics.csv | head -3
    diff build/host-base/out/$D/allhash.csv build/host-cand/out/$D/allhash.csv | head -6
  fi
done
exit $rc
