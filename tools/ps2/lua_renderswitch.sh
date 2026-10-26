#!/bin/sh
# OPT14: lt_renderswitch.lua on the PS2: the renderer is changed by console commands injected by frame (-netcmd file:cmd.txt) while a HUD script holds patches and colormaps in tables.
# usage: tools/ps2/lua_renderswitch.sh ELF [PAKDIR]   -> prints the LQ lines of the run (every "check" line must have n == total and no error text)
ELF=${1:?ELF}
PAK=${2:-build/pak2}
OUT=build/opt14-renderswitch
mkdir -p $OUT
printf '70:renderer Hardware|220:renderer Software|380:renderer Hardware|560:renderer Software' > $OUT/cmd.txt
python3 tools/ps2/ftest_run.py --name rs --elf $ELF --pak $PAK --files tools/ps2/luatests/lt_renderswitch.lua,$OUT/cmd.txt --until "LQ DONE_RENDERSWITCH" --timeout 1200 --out $OUT -- -skipintro -warp 1 -file lt_renderswitch.lua -netcmd file:cmd.txt -ps2ref-maptics > $OUT/rs.out 2>&1
grep -a "LQ \|WARNING\|ERROR\|renderer" $OUT/rs/boot.txt | cut -c1-200 | head -40
