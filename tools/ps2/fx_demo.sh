#!/bin/bash
# OPT11-FX2: HWPROF time demo(s) of one ELF one after the other (one emulator at a time).
# usage: fx_demo.sh TAG ELF "D1 D4 ..." [extra engine args]      -> build/runs/<TAG>_d<N>/boot.txt, compare with fx_ab.py / fx_prof.py
TAG=$1
ELF=$2
DEMOS=$3
shift 3
cd "$(dirname "$0")/../.."
for d in $DEMOS; do
	n=${d#D}
	python3 -B tools/ps2/opt_run.py --name "${TAG}_d$n" --elf "$ELF" --pak /home/user/SRB2-Banpyura-PS2/build/pak --out build/runs --demo "DEMO_00$n" --no-ref --timeout 2400 --until "gametics in" -- -renderer Hardware -zreserve 1536 -ps2prof "$@" > "build/runs/${TAG}_d$n.log" 2>&1
	echo "$TAG d$n: $(grep -h 'timed .* gametics' build/runs/${TAG}_d$n/boot.txt | tail -1)"
done
