#!/bin/bash
# usage: tools/ps2/geom/dettest.sh ELFDIR TAG "extra args A" "extra args B" MAP...  -> build/runs/TAG_A_<MAP>, TAG_B_<MAP> (frame locked static view, -hwpolyhash), comparison A vs B by gm_polycmp --order in build/logs/det_TAG.txt
W=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$W" || exit 1
export PS2DEV=/opt/ps2dev-x/ps2dev
export PATH=$PS2DEV/ee/bin:$PS2DEV/bin:$PS2DEV/dvp/bin:$PATH
ELF=$1; TAG=$2; AA=$3; AB=$4; shift 4
OUT=build/logs/det_${TAG}.txt
: > $OUT
for MAP in "$@"; do
  for S in A B; do
    if [ $S = A ]; then ARGS=$AA; else ARGS=$AB; fi
    NAME=${TAG}${S}_$MAP
    rm -rf build/runs/$NAME
    python3 tools/ps2/hwrun.py --elf $W/build/$ELF/SRB2.ELF --timeout 600 "$NAME=map:$MAP:ZQUIT DONE" -- -hwdbg 0 -zreserve 1536 -zquit 250 -singletics -hwpolyhash $ARGS > build/logs/r_$NAME.txt 2>&1
    rm -f build/runs/$NAME/SRB2.ELF
  done
  echo "== $MAP: $(python3 tools/ps2/gm_polycmp.py ${TAG}A_$MAP ${TAG}B_$MAP --order --show 2 | head -3 | tr '\n' ' ')" >> $OUT
  echo "== $MAP world: $(python3 tools/ps2/gm_polycmp.py ${TAG}A_$MAP ${TAG}B_$MAP --world --show 2 | head -3 | tr '\n' ' ')" >> $OUT
done
echo done >> $OUT
