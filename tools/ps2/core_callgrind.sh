#!/bin/bash
# OPT11-CORE: callgrind profile of the tic logic (TryRunTics only) of one demo on the native host profile build (x86; instruction counts, not EE cycles: use it for the
# call tree and the ranking, the EE sampler (build.py --sample, tools/ps2/sw_sample.sh) for cycles).
# usage: core_callgrind.sh HOSTBUILD_DIR DEMO_00n OUTDIR     e.g.  core_callgrind.sh build/host-polychk DEMO_003 build/cg
# then: callgrind_annotate --inclusive=yes OUTDIR/DEMO_003.cg | head -60
set -e
B=$(realpath "$1"); D=$2; OUT=$3
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
PAK=${SRB2_PAKDIR:-/home/user/SRB2-Banpyura-PS2/build/pak}
EXE=$(ls "$B"/b/bin/*)
mkdir -p "$OUT"; OUT=$(realpath "$OUT")
W=$OUT/$D.run
rm -rf "$W"; mkdir -p "$W/home/.srb2"
cp "$ROOT/golden/phase0-v2/$D.lmp" "$W/home/.srb2/$D.lmp"
printf 'fpscap "35"\nfullscreen "Off"\nshowfps "Off"\nshowping "Off"\nrollingdemos "Off"\n' > "$W/home/.srb2/reference.cfg"
cd "$W"
SRB2WADDIR=$PAK SDL_AUDIODRIVER=dummy xvfb-run -a -s "-screen 0 800x600x24" valgrind --tool=callgrind --toggle-collect=${CG_FUNC:-P_Ticker} --callgrind-out-file="$OUT/$D.%p.cg" \
  "$EXE" -ps2ref "$W" -home "$W/home" -config reference.cfg -nolog -noendtxt -win -width 320 -height 200 -timedemo $D.lmp > stdout.log 2>&1 || true
ls -la "$OUT/$D.cg"
