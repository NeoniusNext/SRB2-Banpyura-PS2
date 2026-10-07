#!/bin/bash
# Native (x86, PS2_PROFILE) run of the four attract demos for bit-exact A/B comparison of software-renderer/tick changes.
# The host profile build is configured as in docs/GATES/g1/opt10-SW.md (cmake -DSRB2_CONFIG_PS2PROFILE=ON -DSRB2_CONFIG_PS2REF=ON, CFLAGS
# "-DPS2_NOOPT_SLOPE -DPS2_NOOPT_SEGS -fwrapv": the float slope code is not bit-equal between x86 and EE, so both sides of the A/B switch it off).
# usage: host_demo.sh EXE OUTDIR [DEMO_001 ...]   -> OUTDIR/<demo>/{tics.csv,allhash.csv,frames.csv,frame-*.idx}
set -e
EXE=$(realpath "$1"); mkdir -p "$2"; OUT=$(realpath "$2"); shift 2
PAK=${SRB2_PAKDIR:-/home/user/SRB2-Banpyura-PS2/build/pak}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
DEMOS=${@:-DEMO_001 DEMO_002 DEMO_003 DEMO_004}
for D in $DEMOS; do
  W=$OUT/$D
  rm -rf "$W"; mkdir -p "$W/home/.srb2"
  cp "$ROOT/golden/phase0-v2/$D.lmp" "$W/home/.srb2/$D.lmp"
  printf 'fpscap "35"\nfullscreen "Off"\nshowfps "Off"\nshowping "Off"\nrollingdemos "Off"\n' > "$W/home/.srb2/reference.cfg"
  (cd "$W" && SRB2WADDIR=$PAK SDL_AUDIODRIVER=dummy xvfb-run -a -s "-screen 0 800x600x24" timeout 400 "$EXE" -ps2ref "$W" -home "$W/home" \
     -config reference.cfg -nolog -noendtxt -win -width 320 -height 200 -timedemo $D.lmp -ps2ref-hashall > stdout.log 2>&1) || true
  test -f "$W/complete.txt" && echo "$D ok: $(wc -l < $W/allhash.csv) hashed frames, $(wc -l < $W/tics.csv) tics" || { echo "$D FAILED"; tail -5 "$W/stdout.log"; }
done
