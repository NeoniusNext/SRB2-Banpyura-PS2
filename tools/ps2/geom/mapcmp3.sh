#!/bin/bash
# usage: tools/ps2/geom/mapcmp.sh MODE ELFDIR TAG "engine args" MAP...
#   MODE ref: only the reference runs   build/runs/R3_<MAP>      (ELFDIR, args)
#   MODE new: only the new runs         build/runs/<TAG>_<MAP>     (ELFDIR, args), then the comparison with R3_<MAP> by gm_polycmp.py
# every run: a static view of the map (the player at the start), -zquit 250 frames, -hwpolyhash, HW, -zreserve 1536
W=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$W" || exit 1
export PS2DEV=/opt/ps2dev-x/ps2dev
export PATH=$PS2DEV/ee/bin:$PS2DEV/bin:$PS2DEV/dvp/bin:$PATH
MODE=$1; ELF=$2; TAG=$3; ARGS=$4; shift 4
OUT=build/logs/mapcmp_${TAG}.txt
: > $OUT
for MAP in "$@"; do
  if [ "$MODE" = ref ]; then NAME=R3_$MAP; else NAME=${TAG}_$MAP; fi
  rm -rf build/runs/$NAME
  python3 tools/ps2/hwrun.py --elf $W/build/$ELF/SRB2.ELF --timeout 600 "$NAME=map:$MAP:ZQUIT DONE" -- -hwdbg 0 -zreserve 1536 -zquit 250 -singletics -hwpolyhash $ARGS > build/logs/r_$NAME.txt 2>&1
  rm -f build/runs/$NAME/SRB2.ELF
  if [ "$MODE" = new ]; then
    echo "== $MAP: $(python3 tools/ps2/gm_polycmp.py $NAME R3_$MAP --world | head -3 | tr '\n' ' ') | mismatch lines: $(grep -c 'HWGC MISMATCH' build/runs/$NAME/boot.txt) | fallback: $(grep -c 'HARDWARE -> SOFTWARE' build/runs/$NAME/boot.txt) | $(grep -h '^HWPROF32' build/runs/$NAME/boot.txt | tail -1 | cut -c1-210)" >> $OUT
  else
    echo "== $MAP ref: frames $(grep -c HWPH build/runs/$NAME/boot.txt) fallback $(grep -c 'HARDWARE -> SOFTWARE' build/runs/$NAME/boot.txt)" >> $OUT
  fi
done
echo done >> $OUT
