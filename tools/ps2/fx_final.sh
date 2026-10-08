#!/bin/bash
# OPT11-FX2: the final checks of one ELF, one after the other (one emulator at a time).  usage: fx_final.sh ELF TAG
#   <TAG>c_dN    check mode (-hwfx 2): no HWC ... MISMATCH line may appear
#   <TAG>pa/pb   pictures at tics 300..900 (k shots), all FX2 paths off / on, whole tics
#   <TAG>la/lb   the same between two tics (-fxfrac 50, frame interpolation on)
#   <TAG>qa/qb   speed between two tics (no shots): off / on
# all FX2 paths off = 1373 (1 + 4 + 8 + 16 + 64 + 256 + 1024)
cd "$(dirname "$0")/../.."
ELF=$1
TAG=$2
OFF=1373
export FX_NOCON=1
SH="-vidshot k300,k450,k600,k750,k900 -hwfbh 200"
tools/ps2/fx_demo.sh ${TAG}c "$ELF" "D4 D1" -hwfx 2
tools/ps2/fx_demo.sh ${TAG}pa "$ELF" "D4 D1" $SH -hwfx $OFF
tools/ps2/fx_demo.sh ${TAG}pb "$ELF" "D4 D1" $SH -hwfx 0
export FX_INTERP=1
tools/ps2/fx_demo.sh ${TAG}la "$ELF" "D4 D1" $SH -hwfx $OFF -fxfrac 50
tools/ps2/fx_demo.sh ${TAG}lb "$ELF" "D4 D1" $SH -hwfx 0 -fxfrac 50
tools/ps2/fx_demo.sh ${TAG}qa "$ELF" "D1 D4" -hwfx $OFF -fxfrac 50
tools/ps2/fx_demo.sh ${TAG}qb "$ELF" "D1 D4" -hwfx 0 -fxfrac 50
echo "fx_final $TAG done"
