#!/bin/sh
# PS2-LOAD-30: the whole Lua equivalence suite (PC reference build against the PS2 ELF in PCSX2), one result line per script.
# usage: [LUA_OUT=dir] tools/ps2/lua_suite.sh ELF [PAKDIR]   (run in the worktree; results in build/lua-equiv/<name>/ or $LUA_OUT/<name>/, summary on stdout)
ELF=${1:?ELF}
PAK=${2:-build/pak2}
T=tools/ps2/luatests
EQ="python3 tools/ps2/lua_equiv.py --elf $ELF --pak $PAK ${LUA_OUT:+--out $LUA_OUT}"
W320="--pc-args=-width 320 -height 200 -ps2ref-maptics"
run() { name=$1; shift; $EQ --name "$name" "$@" 2>&1 | grep -E "^(lt_|lm_|RESULT|[a-z0-9_]+: )" | tr '\n' ' '; echo; }
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
run lt_hooks3  --demo DEMO_001 --timedemo --until "LQ DONE_HOOKS3" $T/lt_hooks3.lua
run lt_err     --warp 1 --until "LQ DONE_ERR" $T/lt_err.lua $T/lt_err0.lua
run lt_hud     --warp 1 --until "LQ DONE_HUD" "$W320" $T/lt_hud.lua
run lt_hooks2  --warp 1 --until "LQ DONE_HOOKS2" "$W320" "--ps2-args=-ps2ref-maptics" $T/lt_hooks2.lua
run lt_local   --warp 1 --until "LQ DONE_LOCAL" "$W320" "--ps2-args=-ps2ref-maptics" --extra $T/lt_local2.lua $T/lt_local.lua
run lt_coro    --warp 1 --until "LQ DONE_CORO" "$W320" "--ps2-args=-ps2ref-maptics" $T/lt_coro.lua
run lt_udmany  --warp 11 --until "LQ DONE_UDMANY" "$W320" "--ps2-args=-ps2ref-maptics" $T/lt_udmany.lua
python3 tools/ps2/make_luapk3.py build/opt14-addons/LP.pk3 >/dev/null
python3 tools/ps2/make_bigaddon.py --out build/opt14-addons >/dev/null
run lt_pk3     --until "LQ DONE_PK3" build/opt14-addons/LP.pk3
run lm_data    --until "LQ DONE_LMDATA" $T/lm_data.lua
run lt_io      --until "LQ DONE_IO" $T/lt_io.lua
run lm_objects --demo DEMO_001 --timedemo --until "LQ DONE_LMOBJ" $T/lm_objects.lua
run lm_world   --demo DEMO_001 --timedemo --until "LQ DONE_LMWORLD" $T/lm_world.lua
run lm_hud     --warp 1 --until "LQ DONE_LMHUD" "$W320" "--ps2-args=-ps2ref-maptics" $T/lm_hud.lua
run lm_hud_hw  --warp 1 --until "LQ DONE_LMHUD" "$W320" "--ps2-args=-ps2ref-maptics -renderer Hardware" $T/lm_hud.lua
run lt_argorder --until "LQ DONE_ARGORDER" $T/lt_argorder.lua
run lt_ops     --until "LQ DONE_OPS" $T/lt_ops.lua
run lt_stack   --warp 1 --until "LQ DONE_STACK" "$W320" "--ps2-args=-ps2ref-maptics" $T/lt_stack.lua
run lt_stack2  --warp 1 --until "LQ DONE_STACK2" "$W320" "--ps2-args=-ps2ref-maptics" $T/lt_stack2.lua
run lt_poolstress --until "LQ DONE_POOLSTRESS" $T/lt_poolstress.lua
run lt_heap    --warp 1 --until "LQ DONE_HEAP" "$W320" "--ps2-args=-ps2ref-maptics" build/opt14-addons/BIG.pk3 $T/lt_heap.lua
echo "lt_heap zone check: $(python3 tools/ps2/lua_heapcheck.py ${LUA_OUT:-build/lua-equiv}/lt_heap/ps2/boot.txt | tail -1)"
run lt_actions --demo DEMO_001 --timedemo --until "LQ DONE_ACTIONS" $T/lt_actions.lua
