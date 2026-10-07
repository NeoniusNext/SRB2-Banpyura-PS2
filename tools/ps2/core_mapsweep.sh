#!/bin/bash
# OPT11-CORE: equivalence of the tic logic over every map. The native host profile build (tools/ps2/host_variant.sh) enters each map with -warp, nobody presses a key,
# and writes the per-tic state hash (every mobj: position, momentum, state, flags; the player; the RNG) of the first TICS tics (-ps2ref-maptics) to <out>/<map>/tics.csv.
# Run it for the build with the old code (host_variant.sh core0 "-DPS2_NOOPT_CORE" ...) and for the new one and compare with tools/ps2/core_mapcmp.py.
# usage: core_mapsweep.sh HOSTBUILD_DIR OUTDIR [TICS=300] [maps ...]     (maps: the inventory names 01 .. 99, M3, F5, MB ...; default: all binary-format campaign/match/CTF maps: the host profile has no UDMF)
set -e
B=$(realpath "$1"); OUT=$2; TICS=${3:-300}; shift 3 || shift $#
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
PAK=${SRB2_PAKDIR:-/home/user/SRB2-Banpyura-PS2/build/pak}
EXE=$(ls "$B"/b/bin/*)
mkdir -p "$OUT"; OUT=$(realpath "$OUT")
MAPS=${@:-$(python3 - <<EOF
import json
d=json.load(open("$ROOT/build/ps2-sw-continuation-map-inventory.json"))
print(" ".join(m["map"] for m in d if m["kind"] in ("SP","MP-special","Match","CTF") and not m.get("udmf")))
EOF
)}
for M in $MAPS; do
  W=$OUT/$M
  rm -rf "$W"; mkdir -p "$W/home/.srb2"
  printf 'fpscap "35"\nfullscreen "Off"\nshowfps "Off"\nshowping "Off"\nrollingdemos "Off"\n' > "$W/home/.srb2/reference.cfg"
  (cd "$W" && SRB2WADDIR=$PAK SDL_AUDIODRIVER=dummy xvfb-run -a -s "-screen 0 800x600x24" timeout 90 "$EXE" -ps2ref "$W" -home "$W/home" \
     -config reference.cfg -nolog -noendtxt -win -width 320 -height 200 -skipintro -warp MAP$M -ps2ref-maptics $TICS > stdout.log 2>&1) || true
  if [ -f "$W/complete.txt" ]; then echo "MAP$M ok: $(($(wc -l < $W/tics.csv) - 1)) tics"; else echo "MAP$M FAILED"; tail -3 "$W/stdout.log"; fi
done
