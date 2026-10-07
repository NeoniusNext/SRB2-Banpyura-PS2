#!/bin/bash
# Linux replacement of build_reference.ps1 + run_reference.py: PC reference engine (software renderer, -ps2ref hooks) runs the four
# attract demos headless (Xvfb, SDL dummy audio) and writes golden/phase0-v2/run1/DEMO_00n/{tics.csv,frames.csv,frame-*.idx,soc.tsv,sfx.csv}.
# usage: tools/ps2/make_golden_linux.sh   (needs: cmake -S . -B build/pc-ref -G Ninja -DSRB2_CONFIG_PS2REF=ON -DSRB2_CONFIG_STATIC_STDLIB=OFF; ninja)
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
ASSETS=${SRB2_ASSETS:-/opt/srb2-assets}
EXE=$(ls "$ROOT"/build/${SRB2_PC_BUILD:-pc-golden}/bin/*/* | head -1)
for n in 1 2 3 4; do
  D=DEMO_00$n
  OUT=$ROOT/golden/phase0-v2/run1/$D
  rm -rf "$OUT"; mkdir -p "$OUT/home/.srb2"
  unzip -oq "$ASSETS/srb2.pk3" $D -d "$OUT/home/.srb2" && mv "$OUT/home/.srb2/$D" "$OUT/home/.srb2/$D.lmp"
  cp "$OUT/home/.srb2/$D.lmp" "$ROOT/golden/phase0-v2/$D.lmp"
  printf 'fpscap "35"\nfullscreen "Off"\nshowfps "Off"\nshowping "Off"\n' > "$OUT/home/.srb2/reference.cfg"
  (cd "$OUT" && SRB2WADDIR=$ASSETS SDL_AUDIODRIVER=dummy xvfb-run -a -s "-screen 0 800x600x24" timeout 600 "$EXE" -ps2ref "$OUT" -home "$OUT/home" \
     -config reference.cfg -nolog -noendtxt -win -width 320 -height 200 -timedemo $D.lmp > stdout.log 2>&1)
  test -f "$OUT/complete.txt" && echo "$D ok: $(ls "$OUT"/frame-*.idx | wc -l) frames, $(wc -l < "$OUT/tics.csv") tics"
done
