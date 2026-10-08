#!/bin/bash
# OPT11-MODEL: fault injection into the memory of the model path (docs/GATES/g1/opt11-MODEL.md): -hwmodelfail N[,K] makes the N-th (and the K - 1 after it) allocation of the model
# code fail as if the zone were full (PS2Models_TryAlloc: the model block, its texture, the blend map, the skin colour texture, the work areas of the drawing). The scene has 20 model
# objects and the player model, gr_models On, MAP01. The run must reach ZQUIT DONE with no I_Error / HEAP CHECK FAILED: a model that does not fit is a sprite.
# usage: md_oom.sh ELF TAG ["N N,K ..."]
ELF=$1
TAG=$2
POINTS=${3:-"1 2 3 4 5 6 8 11 16 24 40 1,100 5,1000"}
cd "$(dirname "$0")/../.."
SET=build/mdall/set_oom.lua
mkdir -p build/mdall
echo 'rawset(_G, "MDL_SET", {"POSS", "SPOS", "BUZZ", "CRAB", "RING", "SPRB", "SPRY", "PTER", "BMCE", "BGAR", "FISH", "SKIM", "PNTY", "JETB", "EGGM", "FANG", "ROSY", "EGR1", "SEBH", "BRAK"})' > $SET
ok=0; bad=0
for n in $POINTS; do
	t=${n//,/_}
	python3 tools/ps2/hf_run.py "${TAG}_$t" --elf "$ELF" --pak build/pak-m --cfg 'gr_models "On";chasecam "On"' --files "$SET,tools/ps2/mdlscene.lua" --timeout 700 \
		-- -skipintro -warp 1 -renderer Hardware -zreserve 1536 -zck -ps2prof -zquit 420 -hwmodelfail $n > build/runs/${TAG}_$t.log 2>&1
	rm -f build/runs/${TAG}_$t/SRB2.ELF
	f=build/runs/${TAG}_$t/boot.txt
	done_=$(grep -c "ZQUIT DONE" $f)
	err=$(grep -c "I_Error\|HEAP CHECK FAILED" $f)
	mod=$(grep "^HWPROF41 model memory" $f | tail -1 | cut -c26-190)
	echo "N=$n done=$done_ errors=$err | $mod"
	if [ "$done_" -ge 1 ] && [ "$err" -eq 0 ]; then ok=$((ok+1)); else bad=$((bad+1)); fi
done
echo "md_oom $TAG: $ok ok, $bad failed"
