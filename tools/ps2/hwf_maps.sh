#!/bin/bash
# OPT12 HWFRONT: the stream of the polygons on static views of maps, OPT12 culls on / off on one ELF (a --hwdetail build):
#   tools/ps2/hwf_maps.sh ELF TAG MAP...  -> build/runs/TAG_<MAP>_on  (-hwpolyhash 3) and TAG_<MAP>_off (-hwpolyhash 3 -hwfr 1), then gm_polycmp.py (all / --world)
# every run: the player at the start of the map, -singletics (a frame is a tic, fixed random seed), -zquit 250 frames, HW, -zreserve 1536
# result: build/logs/hwfmaps_<TAG>.txt
W=$(cd "$(dirname "$0")/../.." && pwd)
cd "$W" || exit 1
export PS2DEV=/opt/ps2dev-x/ps2dev
ELF=$1; TAG=$2; shift 2
OUT=build/logs/hwfmaps_${TAG}.txt
: > $OUT
for MAP in "$@"; do
  for V in on off; do
    NAME=${TAG}_${MAP}_$V
    EXTRA=""; [ $V = off ] && EXTRA="-hwfr 1"
    rm -rf build/runs/$NAME
    python3 tools/ps2/hwrun.py --elf $W/$ELF --timeout 900 "$NAME=map:$MAP:ZQUIT DONE" -- -hwdbg 0 -zreserve 1536 -zquit 250 -singletics -hwpolyhash 3 $EXTRA > build/logs/r_$NAME.txt 2>&1
    rm -f build/runs/$NAME/SRB2.ELF build/runs/$NAME/pcsx2.log
  done
  A=${TAG}_${MAP}_on; B=${TAG}_${MAP}_off
  echo "== $MAP: frames $(grep -c '^HWPH' build/runs/$A/boot.txt)/$(grep -c '^HWPH' build/runs/$B/boot.txt) | all: $(python3 tools/ps2/gm_polycmp.py $A $B | head -3 | tr '\n' ' ') | world: $(python3 tools/ps2/gm_polycmp.py $A $B --world | head -3 | tr '\n' ' ') | fallback: $(grep -c 'HARDWARE -> SOFTWARE' build/runs/$A/boot.txt) | $(grep -h '^HWPROF62' build/runs/$A/boot.txt | tail -1 | cut -c1-200)" >> $OUT
done
echo done >> $OUT
