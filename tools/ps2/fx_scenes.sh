#!/bin/bash
# OPT11-FX: run a list of scenes through fx_pair.py. usage: fx_scenes.sh TAG ELF scene...   scene = NAME:MAP:TICK:CMD
# CMD (the camera, a console command after the first shot: teleport~-nop~-ang~90~-aim~-10) is - for none.
TAG=$1
ELF=$2
shift 2
cd "$(dirname "$0")/../.."
for s in "$@"; do
	IFS=: read -r name map tick cmd <<<"$s"
	[ "$cmd" = "-" ] && cmd=""
	python3 tools/ps2/fx_pair.py "${TAG}_${name}" --ref "$name" --map "$map" --tick "$tick" --elf "$ELF" ${cmd:+--pre "$cmd"} 2>&1 | tail -2
done
