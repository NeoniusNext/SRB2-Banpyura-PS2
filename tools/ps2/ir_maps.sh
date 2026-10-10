#!/bin/bash
# OPT13 IR: the polygon stream on static views of maps, stored records v3 on / off / check mode, on one ELF.
#   tools/ps2/ir_maps.sh ELF TAG MAP...   -> build/runs/TAG_<MAP>_{on,off,chk}; result lines in build/logs-ir/maps_<TAG>.txt
# on  = defaults (-hwpolyhash 3), off = -hwgc 0 (no cache: the reference stream), chk = -hwgc 2 -hwgv 4 (every hit replayed for real and calculated again, blocks and buckets compared)
# every run: the player at the start of the map, -singletics (a frame is a tic, fixed random seed), -zquit 250 frames, HW, -zreserve 1536
W=$(cd "$(dirname "$0")/../.." && pwd)
cd "$W" || exit 1
export PS2DEV=/opt/ps2dev-x/ps2dev PATH=/opt/ps2dev-x/ps2dev/ee/bin:$PATH
ELF=$1; TAG=$2; shift 2
PAK=${PAK:-$W/build/pak2}
OUT=build/logs-ir/maps_${TAG}.txt
: > $OUT
for MAP in "$@"; do
  for V in on off chk; do
    NAME=${TAG}_${MAP}_$V
    case $V in on) X="";; off) X="-hwgc 0";; chk) X="-hwgc 2 -hwgv 4";; esac
    python3 -B tools/ps2/opt_run.py --name $NAME --elf $ELF --pak $PAK --out build/runs --map $MAP --timeout 1500 --until "ZQUIT DONE" -- -renderer Hardware -zreserve 1536 -ps2prof -hwdbg 0 -zquit 250 -singletics -hwpolyhash 3 $X > build/runs/$NAME.log 2>&1
    rm -rf build/runs/$NAME/SRB2.ELF build/runs/$NAME/*.PAK build/runs/$NAME/FINEACON.DAT build/runs/$NAME/refout build/runs/$NAME/pcsx2.log
  done
  A=${TAG}_${MAP}_on; B=${TAG}_${MAP}_off; C=${TAG}_${MAP}_chk
  echo "== $MAP: frames $(grep -c '^HWPH' build/runs/$A/boot.txt)/$(grep -c '^HWPH' build/runs/$B/boot.txt) | all: $(python3 tools/ps2/gm_polycmp.py $A $B | head -2 | tr '\n' ' ') | world: $(python3 tools/ps2/gm_polycmp.py $A $B --world | head -2 | tr '\n' ' ') | fallback: $(grep -c 'HARDWARE -> SOFTWARE' build/runs/$A/boot.txt) | chk: $(grep -h '^HWPROF70' build/runs/$C/boot.txt | tail -1 | sed -E 's/.*(verify=[0-9]+ bad=[0-9]+ cullbad=[0-9]+).*/\1/') $(grep -h '^HWPROF32' build/runs/$C/boot.txt | tail -1 | sed -E 's/.*(check=[0-9]+ bad=[0-9]+).*/\1/') mism=$(grep -c 'HWGV MISMATCH\|HWGC MISMATCH' build/runs/$C/boot.txt) cullbad=$(grep -c 'CULL BAD' build/runs/$A/boot.txt) | hit-stats: $(grep -h '^HWPROF70' build/runs/$A/boot.txt | tail -1 | sed -E 's/.*(blocks=[0-9]+ culled=[0-9]+).*/\1/')" >> $OUT
done
echo done >> $OUT
