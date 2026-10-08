#!/bin/bash
# OPT11-MODEL: "do no harm" check: with gr_models Off (the default) the picture and the speed must be those of the tree without the model work.
# usage: md_noharm.sh ELF BASE_ELF TAG   (both ELFs of --prof builds; BASE_ELF = the main branch before the model work)
#   <TAG>n_base_d1 / _d4, <TAG>n_new_d1 / _d4   the time demos (HWPROF, without the console lines of the profiler): compare with tools/ps2/md_wall.py
#   <TAG>p_base_d1 / _d4, <TAG>p_new_d1 / _d4   pictures at tics 300..900: pixel comparison with tools/ps2/fx_ppmcmp.py (the top 40 rows hold the console lines)
cd "$(dirname "$0")/../.."
ELF=$1
BASE=$2
TAG=$3
export FX_NOCON=1
SH="-vidshot k300,k450,k600,k750,k900 -hwfbh 200"
tools/ps2/fx_demo.sh ${TAG}n_base "$BASE" "D1 D4"
tools/ps2/fx_demo.sh ${TAG}n_new "$ELF" "D1 D4"
tools/ps2/fx_demo.sh ${TAG}p_base "$BASE" "D1 D4" $SH
tools/ps2/fx_demo.sh ${TAG}p_new "$ELF" "D1 D4" $SH
for d in d1 d4; do
	echo "== pictures $d"
	python3 tools/ps2/fx_ppmcmp.py ${TAG}p_base_$d ${TAG}p_new_$d 40
done
echo "md_noharm $TAG done"
