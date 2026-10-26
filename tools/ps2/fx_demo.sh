#!/bin/bash
# OPT11-FX2: HWPROF time demo(s) of one ELF one after the other (one emulator at a time).
# FX_EMU=/opt/pcsx2/slot3/AppRun pins the run to one emulator copy (the first free one is taken otherwise; a copy shared by several runs at once stops at start)
# usage: fx_demo.sh TAG ELF "D1 D4 ..." [extra engine args]      -> build/runs/<TAG>_d<N>/boot.txt, compare with fx_ab.py / fx_prof.py
TAG=$1
ELF=$2
DEMOS=$3
shift 3
cd "$(dirname "$0")/../.."
for d in $DEMOS; do
	n=${d#D}
	cfg=()
	if [ -n "$FX_NOCON" ]; then cfg+=(--cfg 'con_hudlines "0"'); fi # FX_NOCON=1: no console lines on the screen (the profile lines are printed on it: 150+ glyph polygons a frame)
	if [ -n "$FX_INTERP" ]; then cfg+=(--cfg 'fpscap "Match refresh rate"'); fi # FX_INTERP=1: frame interpolation on (with -fxfrac N every frame is N percent between two tics)
	if [ -n "$FX_CFG" ]; then # lines of the staged reference.cfg separated by '|', e.g. FX_CFG='fpscap "Match refresh rate"|con_hudlines "0"' (opt_run says fpscap 35; the profile lines print on the console, 4 lines of text, 150+ glyph polygons a frame)
		IFS='|' read -ra parts <<< "$FX_CFG"
		for c in "${parts[@]}"; do cfg+=(--cfg "$c"); done
	fi
	python3 -B tools/ps2/opt_run.py --name "${TAG}_d$n" --elf "$ELF" --pak "${FX_PAK:-/home/user/SRB2-Banpyura-PS2/build/pak}" --out build/runs --demo "DEMO_00$n" --no-ref --timeout 2400 --until "gametics in" ${FX_EMU:+--emu "$FX_EMU"} "${cfg[@]}" -- -renderer Hardware -zreserve 1536 -ps2prof "$@" > "build/runs/${TAG}_d$n.log" 2>&1
	rm -f "build/runs/${TAG}_d$n/SRB2.ELF" # (10 MB a run: the disk is shared; the ELF is the one given on the command line)
	echo "$TAG d$n: $(grep -h 'timed .* gametics' build/runs/${TAG}_d$n/boot.txt | tail -1)"
done
