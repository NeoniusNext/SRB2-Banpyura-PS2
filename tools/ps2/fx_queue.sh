#!/bin/bash
# OPT11-FX: the PCSX2 runs of the final check one after the other (one emulator at a time). Every step writes build/fx/queue_<step>.log.
# usage: fx_queue.sh ELF step...     steps: screens fxscene chain chain2 gamma waterB waterA model1 model2 fog22 restore
ELF=$1
shift
cd "$(dirname "$0")/../.."
SCR=/tmp/claude-0/-home-user-SRB2-Banpyura-PS2/8e8c5b29-5620-5644-b155-3acefb642e04/scratchpad
for step in "$@"; do
	case $step in
	screens) python3 tools/ps2/fx_screens.py --tag fx --elf "$ELF" ;;
	fxscene) rm -rf build/ref/fx_fxscene3; python3 tools/ps2/fx_pair.py fxscene3 --ref fxscene3 --map 1 --tick 300 --addon tools/ps2/fxscene.lua --elf "$ELF" --cmd con_hudlines~0 ;;
	chain) python3 tools/ps2/fx_chain.py chain1 1,7,2,13,10,5,1 --elf "$ELF" ;;
	chain2) python3 tools/ps2/fx_chain.py chain2 1,7,2,13,10,5,1 --elf "$ELF" "--extra=-hwdbg 33554432" ;;
	fog22) python3 tools/ps2/fx_pair.py fog22b --ref fog22b --map 22 --tick 330 --pre 'teleport~-x~-700~-y~-11300~-z~1500~-ang~90~-aim~0' --elf "$ELF" ;;
	restore) python3 tools/ps2/pcshot.py cfgrestore --shots k30 --warp 1 --cfg 'chasecam "Off"' --exe "$PWD/build/pc-ref/bin/lsdlsrb2_claude/lucid-mayer-1izlqe" --size 320x200 ;;
	gamma) python3 tools/ps2/fx_pair.py gamma4 --ref gamma4 --map 1 --tick 300 --cmd 'gamma~4;con_hudlines~0' --elf "$ELF" ;;
	waterB) tools/ps2/fx_water.sh a3 "$ELF" ;;
	waterA) tools/ps2/fx_water.sh a3o "$ELF" "-hwwater 1" ;;
	model1) rm -rf build/ref/fx_model1; python3 tools/ps2/fx_pair.py model1 --ref model1 --map 1 --tick 300 --addon tools/ps2/fxscene.lua --tree $SCR/fxmodel --elf "$ELF" --cmd 'gr_models~On;con_hudlines~0' ;;
	model2) rm -rf build/ref/fx_model2; python3 tools/ps2/fx_pair.py model2 --ref model2 --map 1 --tick 300 --addon tools/ps2/fxscene.lua --tree $SCR/fxmodel --elf "$ELF" --cmd 'gr_models~On;gr_modellighting~On;con_hudlines~0' ;;
	esac > "build/fx/queue_$step.log" 2>&1
	echo "step $step done: $(tail -1 build/fx/queue_$step.log | cut -c1-200)"
done
