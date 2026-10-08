#!/bin/bash
# OPT11-FX3: A/B speed of one ELF on the four demos, without the profiler console, whole-tic frames and between two tics: fx_ab.sh TAG ELF "FLAGS_A" "FLAGS_B" [DEMOS]
#   runs <TAG>a_dN (-hwfx FLAGS_A) and <TAG>b_dN (-hwfx FLAGS_B), then the same with -fxfrac 50 and frame interpolation: <TAG>la_dN, <TAG>lb_dN; compare with fx_prof.py / fx_abtab.py
cd "$(dirname "$0")/../.."
TAG=$1
ELF=$2
FA=$3
FB=$4
DEMOS=${5:-"D1 D4"}
export FX_NOCON=1
tools/ps2/fx_demo.sh ${TAG}a "$ELF" "$DEMOS" -hwfx $FA
tools/ps2/fx_demo.sh ${TAG}b "$ELF" "$DEMOS" -hwfx $FB
export FX_INTERP=1
tools/ps2/fx_demo.sh ${TAG}la "$ELF" "$DEMOS" -hwfx $FA -fxfrac 50
tools/ps2/fx_demo.sh ${TAG}lb "$ELF" "$DEMOS" -hwfx $FB -fxfrac 50
echo "fx_ab $TAG done"
