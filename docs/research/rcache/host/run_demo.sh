#!/bin/bash
# OPT13-RCACHE: run one golden demo of the host profile build, optionally under callgrind with the EE cache geometry.
# usage: run_demo.sh EXE DEMO_00n OUTDIR [cg "EXTRA CALLGRIND ARGS" | mc "EXTRA MEMCHECK ARGS"]     (cg mode: D1 8 KB 2-way 64 B, I1 16 KB 2-way 64 B, LL unused-but-valid)
set -e
EXE=$(realpath "$1"); D=$2; OUT=$3; MODE=$4; XARGS=$5
ROOT=$(cd "$(dirname "$0")/../../../.." && pwd)
PAK=${SRB2_PAKDIR:-/home/user/SRB2-Banpyura-PS2/build/pak}
mkdir -p "$OUT"; OUT=$(realpath "$OUT"); W=$OUT/$D.run
rm -rf "$W"; mkdir -p "$W/home/.srb2"
cp "$ROOT/golden/phase0-v2/$D.lmp" "$W/home/.srb2/$D.lmp"
printf 'fpscap "35"\nfullscreen "Off"\nshowfps "Off"\nshowping "Off"\nrollingdemos "Off"\n' > "$W/home/.srb2/reference.cfg"
cd "$W"
PRE=""
if [ "$MODE" = cg ]; then
  PRE="valgrind --tool=callgrind --cache-sim=yes --D1=8192,2,64 --I1=16384,2,64 --LL=262144,8,64 --callgrind-out-file=$OUT/$D.cg.%p $XARGS"
fi
if [ "$MODE" = mc ]; then
  PRE="valgrind --tool=memcheck --track-origins=yes --error-limit=no --num-callers=14 --log-file=$OUT/mc.%p.log $XARGS"
fi
SRB2WADDIR=$PAK SDL_AUDIODRIVER=dummy xvfb-run -a -s "-screen 0 800x600x24" $PRE "$EXE" -ps2ref "$W" -home "$W/home" -config reference.cfg -nolog -noendtxt -win -width 320 -height 200 -timedemo $D.lmp $DEMO_ARGS > stdout.log 2>&1 || true
ls "$W" | head -20
