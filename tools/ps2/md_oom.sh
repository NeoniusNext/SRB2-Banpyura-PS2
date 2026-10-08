#!/bin/bash
# OPT11-MODEL: fault injection into the memory of the model path (docs/GATES/g1/opt11-MODEL.md): the Nth allocation made while frames are drawn (any tag) fails
# (-zoomany -zoomnth N -zoomafter 2: the same mechanism as tools/ps2/oom_inject.py); the scene has 20 model objects and the player model, gr_models On.
# The run must reach ZQUIT DONE with no I_Error / HEAP CHECK FAILED: a model that does not fit is a sprite.
# usage: md_oom.sh ELF TAG "N N N ..."
ELF=$1
TAG=$2
POINTS=${3:-"1 2 3 4 5 6 8 10 13 17 21 28 34 45 55 70 89 120 144 200 233 300 400"}
cd "$(dirname "$0")/../.."
SET=build/mdall/set_oom.lua
mkdir -p build/mdall
echo 'rawset(_G, "MDL_SET", {"POSS", "SPOS", "BUZZ", "CRAB", "RING", "SPRB", "SPRY", "PTER", "BMCE", "BGAR", "FISH", "SKIM", "PNTY", "JETB", "EGGM", "FANG", "ROSY", "EGR1", "SEBH", "BRAK"})' > $SET
ok=0; bad=0
for n in $POINTS; do
	python3 tools/ps2/hf_run.py "${TAG}_$n" --elf "$ELF" --pak build/pak-m --cfg 'gr_models "On";chasecam "On"' --files "$SET,tools/ps2/mdlscene.lua" --timeout 700 \
		-- -skipintro -warp 1 -renderer Hardware -zreserve 1536 -zck -zquitall 500 -zoomany -zoomnth $n -zoomafter 2 > build/runs/${TAG}_$n.log 2>&1
	f=build/runs/${TAG}_$n/boot.txt
	done_=$(grep -c "ZQUIT DONE" $f)
	err=$(grep -c "I_Error\|HEAP CHECK FAILED" $f)
	ev=$(grep -c "OOM (recoverable)" $f)
	hwfb=$(grep -m1 "ps2_hwfb: fallbacks" $f | cut -c1-120)
	mod=$(grep "^models:" $f | tail -1 | cut -c1-150)
	echo "N=$n done=$done_ errors=$err recoverable=$ev | $mod | $hwfb"
	if [ "$done_" -ge 1 ] && [ "$err" -eq 0 ]; then ok=$((ok+1)); else bad=$((bad+1)); fi
done
echo "md_oom $TAG: $ok ok, $bad failed"
