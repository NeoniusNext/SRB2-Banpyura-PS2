#!/bin/sh
# PS2-LOAD-30: the whole Lua equivalence suite (PC reference build against the PS2 ELF in PCSX2), one result line per script.
# usage: tools/ps2/lua_suite.sh ELF [PAKDIR]   (run in the worktree; results in build/lua-equiv/<name>/, summary on stdout)
ELF=${1:?ELF}
PAK=${2:-build/pak2}
T=tools/ps2/luatests
EQ="python3 tools/ps2/lua_equiv.py --elf $ELF --pak $PAK"
W320="--pc-args=-width 320 -height 200 -ps2ref-maptics"
run() { name=$1; shift; $EQ --name "$name" "$@" 2>&1 | grep -E "^(lt_|RESULT|[a-z0-9]+: )" | tr '\n' ' '; echo; }
run lt_vm      --until "LQ DONE_VM" $T/lt_vm.lua
run lt_math    --until "LQ DONE_MATH" $T/lt_math.lua
run lt_globals --until "LQ DONE_GLOBALS" $T/lt_globals.lua
run lt_info    --until "LQ DONE_INFO" $T/lt_info.lua
run lt_libs    --until "LQ DONE_LIBS" $T/lt_libs.lua
run lt_slots   --until "LQ DONE_SLOTS" $T/lt_slots.lua
run lt_soc     --until "LQ DONE_SOCREAD" $T/lt_soc.soc $T/lt_socread.lua
run lt_fields  --demo DEMO_001 --timedemo --until "LQ DONE_FIELDS" $T/lt_fields.lua
run lt_api     --demo DEMO_001 --timedemo --until "LQ DONE_API" $T/lt_api.lua
run lt_hooks   --demo DEMO_001 --timedemo --until "LQ DONE_HOOKS" $T/lt_hooks.lua
run lt_err     --warp 1 --until "LQ DONE_ERR" $T/lt_err.lua $T/lt_err0.lua
run lt_hud     --warp 1 --until "LQ DONE_HUD" "$W320" $T/lt_hud.lua
run lt_hooks2  --warp 1 --until "LQ DONE_HOOKS2" "$W320" "--ps2-args=-ps2ref-maptics" $T/lt_hooks2.lua
run lt_local   --warp 1 --until "LQ DONE_LOCAL" "$W320" "--ps2-args=-ps2ref-maptics" --extra $T/lt_local2.lua $T/lt_local.lua
