#!/bin/bash
# OPT11-FX: the water scenes of the final check (PC OpenGL reference | PS2-HW): the grid texture FXFLAT.wad on the water of DSZ1 (seams, far ripple, angled views)
# and the plain water from four directions, GFZ1 / GFZ2 water pits (under water). The PC references are cached in build/ref/fx_<scene>.
# usage: fx_water.sh TAG ELF [hwargs]       e.g.  fx_water.sh a3 build/out/SRB2.ELF   and   fx_water.sh a3o build/out/SRB2.ELF "-hwwater 1" (the OPT10 sweep)
TAG=$1
ELF=$2
HWARGS=$3
cd "$(dirname "$0")/../.."
FXF=build/fx/FXFLAT.wad
run() { # name map tick pre addon
	python3 tools/ps2/fx_pair.py "${TAG}_$1" --ref "$1" --map "$2" --tick "$3" --elf "$ELF" ${4:+--pre "$4"} ${5:+--addon "$5"} ${HWARGS:+"--hwargs=$HWARGS"} 2>&1 | tail -1
}
run g2 7 300 'teleport~-nop~-ang~270~-aim~-40' $FXF
run g3 7 300 'teleport~-x~0~-y~-1984~-z~150~-ang~270~-aim~-30' $FXF
run grid_s5 7 300 'teleport~-nop~-ang~270~-aim~-5' $FXF
run dsz_e 7 300 'teleport~-nop~-ang~0~-aim~-8'
run dsz_w 7 300 'teleport~-nop~-ang~180~-aim~-8'
run dsz_n15 7 300 'teleport~-nop~-ang~90~-aim~-15'
run dsz_s5 7 300 'teleport~-nop~-ang~270~-aim~-5'
run under1 1 100 'teleport~-x~2020~-y~3468~-z~256~-ang~90~-aim~0'
run under2 2 100 'teleport~-x~3564~-y~6228~-z~-80~-ang~90~-aim~0'
