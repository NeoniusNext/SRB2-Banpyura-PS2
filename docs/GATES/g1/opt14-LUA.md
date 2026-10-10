# OPT14-LUA: Lua errors on the PS2 (user complaint: "Lua errors on PS2, none on PC")

Worktree `/home/user/wt/lua`, branch `opt14-lua`, base `ece6e7c` (merged OPT13 IQ/IS/IZ/IR/IO). Reference: PC build `build/pc-golden` (x86-64 Linux), upstream 0e09462.
Tools: `tools/ps2/lua_equiv.py` (PC against PS2 ELF in PCSX2, `LQ` lines + WARNING/ERROR lines with tracebacks), `tools/ps2/lua_suite.sh`, scripts `tools/ps2/luatests/`.

## 1. Step 1: the OPT12-LOAD suite on the merged ELF (task g)

ELF: `SRB2_PS2_OUT=build/out SRB2_PS2_NO= SRB2_PS2_HW=1 python3 tools/ps2/build.py --jobs 2` (release, LTO, 203 files), run: `sh tools/ps2/lua_suite.sh build/out/SRB2.ELF build/pak2`.

All 15 scripts are SAME (0 differing lines) on the merged ELF: lt_vm 88, lt_math 59, lt_globals 50, lt_info 507, lt_libs 33, lt_slots 12 928, lt_soc 9, lt_fields 428, lt_api 202, lt_hooks 31,
lt_hooks3 17, lt_err 53, lt_hud 50, lt_hooks2 27, lt_local 6 lines. (Software renderer, single player. The suite found no divergence caused by the OPT13 merge: IQ-5 struct layout, IQ-6 hook mask, IS-703 interpolation state.)

Conclusion of step 1: the existing suite does not reproduce the complaint. Steps 2.. build bigger "mod like" scenarios and look at the code paths the suite cannot reach.
