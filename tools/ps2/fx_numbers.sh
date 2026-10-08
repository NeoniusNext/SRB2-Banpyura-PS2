#!/bin/bash
# OPT11-FX2: the final numbers of round 2: speed of the whole-tic demo frames without the console of the profiler, one ELF after the other (one emulator at a time).
# usage: fx_numbers.sh ELF BASE_ELF TAG
#   <TAG>n_base  BASE_ELF (the merged tree before round 2), all D1 / D4
#   <TAG>n_off   ELF with all FX2 paths off (-hwfx 1373)
#   <TAG>n_on    ELF with all paths on (-hwfx 0)
#   <TAG>p_base / <TAG>p_on   pictures of BASE_ELF and ELF at tics 300..900 (k shots): pixel comparison with fx_ppmcmp.py
# compare with:  fx_prof.py build/runs/<TAG>n_off_d1/boot.txt build/runs/<TAG>n_on_d1/boot.txt
cd "$(dirname "$0")/../.."
ELF=$1
BASE=$2
TAG=$3
export FX_NOCON=1
SH="-vidshot k300,k450,k600,k750,k900 -hwfbh 200"
tools/ps2/fx_demo.sh ${TAG}n_base "$BASE" "D1 D4"
tools/ps2/fx_demo.sh ${TAG}n_off "$ELF" "D1 D4" -hwfx 1373
tools/ps2/fx_demo.sh ${TAG}n_on "$ELF" "D1 D4" -hwfx 0
tools/ps2/fx_demo.sh ${TAG}p_base "$BASE" "D4 D1" $SH
tools/ps2/fx_demo.sh ${TAG}p_on "$ELF" "D4 D1" $SH -hwfx 0
echo "fx_numbers $TAG done"
