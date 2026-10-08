#!/bin/bash
# OPT11-MODEL: every campaign map with gr_models On and the chase camera (the player model on the screen, the objects of the map as models): no I_Error, no OOM,
# the memory the models take (HWPROF41) and their cost (HWPROF40). usage: md_maps.sh PROF_ELF TAG "01 02 ..."
ELF=$1
TAG=$2
MAPS=${3:-"01 02 04 05 07 08 10 11 13 14 16 22 25 30 32 33 40"}
cd "$(dirname "$0")/../.."
for m in $MAPS; do
	python3 tools/ps2/hf_run.py "${TAG}_$m" --elf "$ELF" --pak build/pak-m --cfg 'gr_models "On";chasecam "On"' --timeout 900 \
		-- -skipintro -warp $m -renderer Hardware -zreserve 1536 -zck -ps2prof -zquit 420 > build/runs/${TAG}_$m.log 2>&1
	f=build/runs/${TAG}_$m/boot.txt
	err=$(grep -c "I_Error\|HEAP CHECK FAILED\|Out of memory" $f)
	fb=$(grep -m1 "HARDWARE ->\|-> SOFTWARE\|falling back" $f | cut -c1-80)
	p40=$(grep "^HWPROF40 models" $f | tail -1 | sed 's/HWPROF40 models: //' | cut -c1-110)
	p41=$(grep "^HWPROF41 model memory" $f | tail -1 | cut -c1-150)
	echo "MAP$m done=$(grep -c 'ZQUIT DONE' $f) err=$err $fb | $p40 | $p41"
done
