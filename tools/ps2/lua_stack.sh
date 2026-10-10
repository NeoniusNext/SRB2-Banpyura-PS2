#!/bin/sh
# OPT14: the deepest the 384 KiB main stack of the EE goes when a script recurses 200 levels (C stack overflow) from different hooks: one run per hook (tools/ps2/luatests/lt_stack3.lua with LM_CTX
# replaced), -zstack -zquit N, "stackused=" of the ZSTAT line. usage: tools/ps2/lua_stack.sh ELF [PAKDIR] [extra engine args...]   results: build/opt14-stack/<ctx>/ps2/boot.txt, summary on stdout
ELF=${1:?ELF}
PAK=${2:-build/pak2}
shift 2 2>/dev/null
OUT=build/opt14-stack
mkdir -p $OUT
for ctx in thinker playerthink prethink hud hudscores maploadhook mapchange chat netvars addonloaded intermission; do
  f=$OUT/lt_stack3_$ctx.lua
  sed "s/^local LM_CTX = \"thinker\"/local LM_CTX = \"$ctx\"/" tools/ps2/luatests/lt_stack3.lua > $f
  python3 tools/ps2/ftest_run.py --name $ctx --elf $ELF --pak $PAK --files $f --until ZSTAT --timeout 900 --out $OUT -- -skipintro -warp 1 -file lt_stack3_$ctx.lua -zstack -zquit 700 -ps2ref-maptics "$@" > $OUT/$ctx.out 2>&1
  echo "$ctx: $(grep -a -o 'stackused=[0-9]*' $OUT/$ctx/boot.txt | head -1) $(grep -a 'LQ \(gsub\|sort\|format\)' $OUT/$ctx/boot.txt | cut -c1-80 | tr '\n' '|')"
done
