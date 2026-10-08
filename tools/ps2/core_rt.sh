#!/bin/bash
# OPT11-CORE: one REAL-TIME demo run (-playdemo: the tic clock runs, frames are interpolated when fpscap allows) on a --prof ELF, with the frame pacing
# statistics of tools/ps2/core_tick.py (tics asked/run, frame interval histogram in 1/60 s, longest frame).
# usage: core_rt.sh NAME ELF DEMO_00n FRAMES "extra reference.cfg line" [engine args...]
#   e.g. core_rt.sh rt-hw ELF DEMO_001 840 'vid_wait "On"' -renderer Hardware -zreserve 1536
cd "$(dirname "$0")/../.."
NAME=$1; ELF=$2; DEMO=$3; FRAMES=$4; CFG=$5; shift 5
ARGS=(--name "$NAME" --elf "$ELF" --pak /home/user/SRB2-Banpyura-PS2/build/pak --out build/runs --demo "$DEMO" --no-ref --playdemo --timeout 1500 --until "ZQUIT DONE")
[ -n "$CFG" ] && ARGS+=(--cfg "$CFG")
python3 tools/ps2/opt_run.py "${ARGS[@]}" -- -ps2prof -zquit "$FRAMES" "$@" 2>&1 | tail -1
python3 tools/ps2/core_tick.py build/runs/$NAME/boot.txt
grep -E "^HWPROF win=[0-9]+ " build/runs/$NAME/boot.txt | sed -n '2,4p' | cut -c1-220
rm -f build/runs/$NAME/pcsx2.log build/runs/$NAME/SRB2.ELF build/runs/$NAME/*.PAK
