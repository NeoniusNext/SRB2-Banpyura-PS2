#!/bin/bash
# OPT13-IO host proofs (no console, no emulator): the pack reader on the packs v1 and v2 (all medium policies, injected device errors, the prefetch pass), the safe save module.
# usage: tools/ps2/io_hosttests.sh [PAKDIR-v2 (build/pak2)] [PAKDIR-v1 (build/pak)] [SRC (/opt/srb2-assets)]
set -u
cd "$(dirname "$0")/../.."
P2=${1:-build/pak2}; P1=${2:-build/pak}; SRC=${3:-/opt/srb2-assets}
mkdir -p build/hosttest
rc=0
python3 -B tools/ps2/test_pack_reader.py --pak "$P2" --src "$SRC" --out build/hosttest/packtest2 > build/hosttest/io_v2.log 2>&1 && echo "OK    pack reader v2 ($P2): $(grep -c . build/hosttest/io_v2.log) lines, $(grep 'TOTAL' build/hosttest/io_v2.log)" || { echo "FAIL  pack reader v2"; rc=1; }
python3 -B tools/ps2/test_pack_reader.py --pak "$P1" --src "$SRC" --out build/hosttest/packtest1 > build/hosttest/io_v1.log 2>&1 && echo "OK    pack reader v1 ($P1): $(grep 'TOTAL' build/hosttest/io_v1.log)" || { echo "FAIL  pack reader v1"; rc=1; }
mkdir -p build/hosttest/sf/d && rm -rf build/hosttest/sf/d/* && cc -O1 -Wall -Wextra -o build/hosttest/sf/safefile_hosttest tools/ps2/safefile_hosttest.c src/ps2/ps2_safefile.c -I src \
  && build/hosttest/sf/safefile_hosttest build/hosttest/sf/d | tail -1 | sed 's/^/OK    /' || { echo "FAIL  safefile"; rc=1; }
python3 tools/ps2/verify_pack.py --src "$SRC" --pak "$P2" > build/hosttest/verify_v2.log 2>&1 && echo "OK    verify_pack.py ($P2): $(grep TOTAL build/hosttest/verify_v2.log)" || { echo "FAIL  verify_pack.py"; rc=1; }
exit $rc
