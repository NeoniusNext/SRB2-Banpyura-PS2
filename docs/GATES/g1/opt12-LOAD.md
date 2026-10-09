# OPT12-LOAD: loading speed, SRP2 packs, UDMF, and Lua 1:1

Role LOAD of OPT12 (registry PS2-LOAD-1..49). Worktree `/home/user/wt/load`, branch `opt12-load`. Baseline for the A/B numbers is commit
`0d5395f` (the profiler of PS2-LOAD-1 on top of the OPT11 merge) built the same way as the candidate: `SRB2_PS2_LTO=0` instrumented ELFs, PCSX2 2.8.2 in the
Linux container, 32 MB retail RAM profile, COP0 Count (294 912 cycles per ms of EE time). **Release (LTO) numbers: see the end of section 2 when present;
until then every number below is from the non-LTO instrumented ELF** (the brief allows these for A/B, not for release claims).

The report is appended after every stage; sections: 1 done, 2 commands and numbers, 3 not checked, 4 registry of deviations, 5 edits in foreign files,
then «Lua 1:1» (classification of every difference to upstream GLideKS/SRB2-Banpyura, commit `0e09462`).

## 1. Done

| id | what | where |
|---|---|---|
| PS2-LOAD-1 | load-time profiler (`-loadprof`: laps of the start-up, intervals of `W_InitFile`, lump reads, `P_LoadLevel` stages, UDMF, Lua) and level-structure hash (`-loadhash`), lump read log (`-lpreads`), sampler windows (`-ps2sample -lpsamp N`) | `src/ps2/ps2_loadprof.[ch]`, hooks in `i_main.c d_main.c w_wad.c w_pack.c p_setup.c r_data.c` |
| PS2-LOAD-2 | hash index of lump names per file (`W_CheckNumForName*`, `W_CheckNumForPatchName*`) instead of a linear scan over every lump of every file | `w_wad.c/.h` |
| PS2-LOAD-3 | sprite name index (`R_GetSpriteNumByName`) | `r_things.c` |
| PS2-LOAD-4 | pad prewarm: the 2-3 s wait for the pad library no longer sits in the middle of the start-up | `ps2/i_joy.c` |
| PS2-LOAD-5..11 | SRP2 **version 2** pack: head table (the first 16 bytes of every lump live in the index, so the many `W_ReadLumpHeader(.., 16, 0)` of the start-up are not file reads), CRC32 table per lump, Fletcher-style checksums of header and index, dedup of identical lumps, optional storage order list (`lump_order.py`: lumps read at start-up are stored in the order they are read), `-verifypack` (integrity check off the hot path), named errors for a damaged or too new pack, backward compatible reader (version 1 and 2); `cook.py --from-pak/--order/--version/--no-dedup`; `verify_pack.py` and the host test updated | `w_pack.c/.h`, `w_wad.c`, `tools/ps2/cook.py verify_pack.py lump_order.py pack_hosttest.c` |
| PS2-LOAD-13 | the 18 palette cvars reload the palette 16 times with the soft-double colour cube while they register: one reload at the end of the block (identical final palette); the fade colormaps (16384 nearest-colour searches) and the light table of the default colormap are kept while their inputs (first row of COLORMAP, master palette, FADECMAP/FADEWMAP lump numbers) are the same, so a level load no longer recomputes them | `v_video.c`, `netcode/d_netcmd.c` (3 lines), `r_data.c` |
| PS2-LOAD-14 | `NearestPaletteColor` / `NearestColor` answer from a luma-sorted search instead of 256 distance computations; **identical answer** (lowest index among the nearest) proven for all 2^24 colours on 9 palettes (the game PLAYPAL, grey ramp, 3-3-2 cube, six random/degenerate ones) | `ps2/ps2_nearest.[ch]`, `r_data.c`, `tools/ps2/nearest_hosttest.c` |
| PS2-LOAD-15 | UDMF tokenizer: state in locals, class table, one function per token (was: 5 function calls and ~10 reloads of the struct per character), byte-level skip of the blocks in the count pass, no copy of the lump for `strlen`; **same tokens, same positions** (tokenizer_hosttest: a real 1 MB TEXTMAP, 400 000 random short inputs, 2000 word-salad inputs, block skip and the reads after it) | `m_tokenizer.[ch]`, `m_misc.c`, `p_setup.c` (TextmapCount), `doomdef.h` |
| PS2-LOAD-16 | a partial read of a deflated lump (every sublump of a map WAD inside a pk3) inflated the **whole** lump again: the last one is kept as an evictable cache block, a full read inflates straight into the caller's buffer | `w_wad.c` |
| PS2-LOAD-30 | Lua 1:1 with upstream: run-by-run comparison of PC reference build and PS2 ELF (`tools/ps2/lua_equiv.py`, `lua_suite.sh`, scripts `tools/ps2/luatests/`), the divergences found and fixed (see «Lua 1:1») | many, see the table below |

Not done as a code change in this report: map MD5 laziness, R_LoadTextures (300 M cycles) restructuring, async IOP reads, a precompiled-Lua cache: see section 3.

## 2. Commands and numbers

### Tools

```
export PS2DEV=/opt/ps2dev-x/ps2dev
SRB2_PS2_OUT=$PWD/build/out-lp SRB2_PS2_LTO=0 SRB2_PS2_NO= SRB2_PS2_HW=1 python3 tools/ps2/build.py --jobs 2          # instrumented ELF
SRB2_PS2_OUT=$PWD/build/out-smp SRB2_PS2_LTO=0 SRB2_PS2_NO= SRB2_PS2_HW=1 python3 tools/ps2/build.py --jobs 2 --sample  # sampler ELF
python3 tools/ps2/opt_run.py --name N --elf ELF --pak build/pak2 --out build/runs --map MAP01 --timeout 900 -- -zquit 10 -loadprof -loadhash
python3 tools/ps2/ftest_run.py --name N --out build/runs --elf ELF --pak build/pak2 --files build/addons/UM.pk3 --until "ZQUIT DONE" -- -config reference.cfg -nolog -noendtxt -skipintro -file UM.pk3 -warp 99 -zquit 10 -loadprof -loadhash
python3 tools/ps2/sample_report.py --elf build/out-smp/SRB2.ELF --log build/runs/N/boot.txt      # with -ps2sample -lpsamp 1..7 (windows: boot, level, R_Init, registration, precache, map file, level free)
python3 tools/ps2/make_udmf_map.py --out build/addons --map 99 --cols 40 --rows 30                   # 1200 rooms, 2470 lines, 4800 sides, 1.0 MB TEXTMAP, in a pk3
python3 tools/ps2/lua_equiv.py --elf build/out-lp/SRB2.ELF [--warp 1|--demo DEMO_001 --timedemo] --until "LQ DONE_X" tools/ps2/luatests/lt_x.lua      # one Lua script
tools/ps2/lua_suite.sh build/out-lp/SRB2.ELF            # all 14 Lua scripts
python3 tools/ps2/tokenizer_hosttest.py build/hosttest/TEXTMAP.txt ; build/hosttest/nearest_hosttest build/hosttest/PLAYPAL.lmp   # host proofs
```

### Start-up (title screen, `-skipintro`), cycles of EE time, non-LTO instrumented ELF

Baseline `b0` (commit 0d5395f) against the candidate `cur3`; the sum of the `B_` laps is the time from `main()` to the first frame of the title.
**Sum 2 323.6 M -> 665.5 M cycles (7 879 ms -> 2 256 ms of EE time, 3.49x)**; real wall time of the whole PCSX2 run to frame 11 of MAP01: 11.1 s -> 4.9 s.

| stage | baseline M | now M | share now |
|---|---:|---:|---:|
| PS2Boot_Init (IOP modules) | 9.7 | 19.0 | 2.9% |
| W_InitMultipleFiles (4 main packs + their SOC) | 32.5 | 39.9 | 6.0% |
| I_StartupGraphics .. SCR_Startup .. palette remap | (in GFX) | 15.9 | 2.4% |
| HU_Init + CON_Init | (in GFX) | 4.1 | 0.6% |
| command registration (cvars, palette cube) | (in GFX) 786.1 | 11.6 | 1.7% |
| HU_LoadGraphics | 63.4 | 17.0 | 2.6% |
| M_FirstLoadConfig .. G_LoadGameData .. M_Init | 20.6 | 20.1 | 3.0% |
| **R_Init** | **1 333.3** | **445.7** | **67.0%** |
| sound init | 25.1 | 25.0 | 3.8% |
| ST_Init, net, start of title | 46.1 | 41.1 | 6.2% |

R_Init split: `R_LoadTextures` 764.8 -> 300.2, `R_InitSprites` 390.8 -> 63.1, `R_InitColormaps` 146.0 -> 51.1, flats 10.5, other 3.4.
Lump reading in the whole start-up: `W_ReadLump*` 362.4 -> 27.3 M cycles (22 279 calls, 815 of them real file reads now).
What is left: R_LoadTextures 300 M (65 % of it is the zone allocator `ZA_Alloc` address-ordered fit and the TEXTURES-lump tokenizer, see section 3), the
single remaining fade/light colormap generation 51 M (soft double, done once), `W_InitMultipleFiles` 40 M (index and SOC).

### One level (MAP01, `-warp`), P_LoadLevel stages

| stage | baseline M | now M |
|---|---:|---:|
| **P_LoadLevel (all)** | **646.0** | **454.1** |
| level wipe (fade out, `P_RunLevelWipe`: waits for the tics of the wipe, idle EE time: cannot be shortened without changing what the player sees) | 170 (inside "pre") | 170.2 |
| rest of "pre" (loading screen, palette, settings, music start) | 8 | 4.3 |
| old level freed | 21.1 | 21.8 |
| level setup before the map file (`R_ReInitColormaps` was 152.6) | 153.6 | 1.0 |
| P_LoadMapFromFile (vertexes .. nodes .. blockmap .. link) | 136.1 | 94.7 |
| P_SpawnSlopes + things + specials | 23.9 | 23.7 |
| R_PrecacheLevel | 99.1 | 102.6 |

Work (without the wipe) 476 M -> 284 M cycles = **1.68x**. The target 2x is **not reached** for a binary map: what remains is `R_PrecacheLevel` 103 M (LZ4 decode of
sprite patches 37 M, memcpy, texture composition), `P_LoadMapData` 60 M, the BSP 21 M, slopes 17 M, the free of the old level 22 M.

### UDMF (synthetic 1200-room map in a pk3, `-warp 99`), P_LoadLevel stages in M cycles (non-LTO)

| stage | baseline | now |
|---|---:|---:|
| tokenizer open (copy of the 1.0 MB TEXTMAP) | 15.5 | 5.0 |
| count pass | 56.7 | 26.3 |
| vertexes / sectors / lines / sides / things | 4.1 / 12.5 / 16.8 / 27.1 / 14.0 | 3.8 / 11.5 / 15.1 / 25.0 / 12.5 |
| **UDMF parse (open .. things)** | **147.0** | **99.3** (1.48x) |
| vres_GetMap (directory of the map WAD in the pk3) | 23.6 | 11.9 |
| P_LoadMapBSP (ZNODES) | 19.4 | 7.6 |
| `W_ReadLump*` in the level | 71.1 | 24.1 |
| P_LoadMapFromFile | 222.2 | 140.1 (1.59x) |
| **P_LoadLevel (all)** | **605.8** | **379.0** (1.60x) |

The level structure hash (`-loadhash`: vertexes, sectors, lines, sides, BSP, blockmap, reject, things) is **equal** to the baseline for this map and for MAP01.
The UDMF parse target 3x is **not reached yet**; see the next stages of this report (in-place pair scanner, lazy MD5).

## 3. Not checked / not done

- Release (LTO) numbers: not yet measured; every table above is non-LTO instrumented. Golden demos (`tools/ps2/golden_full.sh`), physics = PC golden: not rerun on the final ELF yet.
- `R_LoadTextures` 300 M (the biggest piece of the start-up) is not restructured; `ZA_Alloc` (CORE's allocator) is a quarter of it.
- Add-on loading (pk3/wad with Lua + SOC), Lua state creation time, script parse time, hook call cost on D1..D4: not measured yet.
- The map MD5: for binary maps the PS2 profile hashes the virtual lumps after `VRES_DROP` freed them (the data pointers are NULL), so `mapmd5` is a hash of whatever lies at address 0, not of the map; for UDMF it is the MD5 of the TEXTMAP (16-28 M cycles on a 1 MB map). Left as is (changing a value used by demos and the server info packet is NET's/CORE's call).
- HUD pixels (only the arguments the library accepts and the sizes it returns are compared), KeyDown/KeyUp (need device events), NetVars / `LUA_Archive` (fire only in a netgame / join), BotAI / BotTiccmd / BotRespawn, TeamSwitch, PlayerQuit, GameQuit, SeenPlayer, ShieldSpecial, LinedefExecute, ShouldJingleContinue, FollowMobj, HurtMsg: not exercised by the scripts (see «Lua 1:1»).
- Real-hardware loading (DVD/HDD/USB read speed, IOP async reads): not measured, only the emulator.

## 4. Registry of deviations from upstream behaviour or the previous PS2 behaviour

| id | deviation | why it is safe |
|---|---|---|
| PS2-LOAD-13 | the palette is rebuilt once instead of 16 times while the palette cvars register | the final palette is the one the last registration produced; the intermediate ones were never shown (rendermode none at that point) |
| PS2-LOAD-13 | `fadecolormap` and the default light table survive `R_ReInitColormaps` when the inputs are bit-equal | inputs are compared byte by byte; a changed palette or colormap lump recomputes |
| PS2-LOAD-14 | `NearestPaletteColor` is a different algorithm | exhaustively equal to the original loop (host test); the validity of the cached context is checked against the 256 colours on every call |
| PS2-LOAD-15 | `Tokenizer_SRB2Read` (PS2 only, `#ifdef PS2_PROFILE`) rewritten; the old token buffer is freed when a token grows; `Tokenizer_SRB2SkipBlock` does not count `tokenizer->line` (never read) | host test equality of tokens, positions and comment state |
| PS2-LOAD-16 | one inflated lump stays in memory as `PU_CACHE` | evictable; the key is (wad, lump, size) |
| PS2-LOAD-5..11 | SRP2 version 2 (see docs/PACK_FORMAT.md) | the reader accepts version 1 and 2; version 3 or a damaged header give a named error |

## 5. Edits in files owned by other roles (pointwise)

- `src/netcode/d_netcmd.c`: three lines around the registration of the 18 palette cvars (`V_DeferPaletteReload`), PS2_PROFILE only.
- `src/hardware/hw_main.c` (HWFRONT): the sprite shadow angle uses the Lua value when `HAS_LUA` (the old condition hard-wired "no Lua"), line 3977.
- `src/p_tick.c`, `src/m_cheat.c`, `src/p_spec.c`, `src/p_enemy.c` (CORE): `LIMIT_*` bounds replaced by `PS2_OOR_*` (a bound that is the PC number, growing the live tables first when a script/SOC/hook needs a slot past the small size).
- `src/command.c`, `src/m_fixed.h`, `src/g_game.c`: float to int conversions and `G_TicsToCentiseconds` give the PC numbers (see «Lua 1:1»).
- `src/z_zone.c`, `src/ps2/ps2_mem.c`: untouched.

## Lua 1:1 with upstream GLideKS/SRB2-Banpyura

Reference: `git diff 0e09462308610005f640ed84b21ec8a4ef116a4b HEAD -- src/lua_*.c src/lua_*.h src/blua src/deh_lua.c src/deh_lua.h src/ps2/lua_stub.c src/p_enemy.c` plus the files I had to touch
(below). Classes: **(a)** needed for the PS2, kept, proven to give the script the same behaviour; **(b)** already upstream; **(c)** superfluous or a loss of function: removed, upstream restored.
The reference run is the PC build `build/pc-golden` on Linux x86-64 (Windows differs where noted: `long` is 32 bit there, like the PS2).

### Method

`tools/ps2/lua_equiv.py` runs the same scripts (`-file a.lua ...`) on the PC build and on the PS2 ELF in PCSX2 and compares, line by line, everything the scripts print with
`print("LQ ...")` and the engine's own `WARNING/ERROR` lines **including the stack tracebacks**. `tools/ps2/lua_suite.sh ELF` runs the 14 scripts:

| script | covers | lines |
|---|---|---:|
| lt_vm | integer VM: arithmetic wrap, `^^ << >> & \|`, comparison, string library, tables and traversal order, closures, metatables, coroutines, pcall/error messages, deep recursion | 88 |
| lt_math | `FixedMul/Div/Rem/Hypot/Sqrt/Int/Floor/Trunc/Ceil/Round`, `sin cos tan asin acos` full tables (FINEACON.DAT), `R_PointToAngle*`, `FixedAngle`, `AngleFixed`, `tofixed`, 3000 random operand pairs hashed | 59 |
| lt_globals | the whole global namespace (384 names with type and size of the tables) | 50 |
| lt_info | states, mobjinfo, sfxinfo, spriteinfo, skincolors: limits, freeslots, read/write, **references held across the growth of the PS2 tables** | 507 |
| lt_slots | freeslot exhaustion of every kind (1030 MT, 8200 S, 1030 SPR, 1030 SPR2, 1030 SKINCOLOR, 1610 sfx), text of every "allocated" / "Ran out of" message | 12 928 |
| lt_libs | CV_ / COM_ / input / color / http helpers / skin / Banpyura / user variables | 33 |
| lt_soc | SOC numbers through `get_number` -> `LUA_EvalMath` (expressions not in the base-game table), objects, frames, sounds, colours, level headers | 9 |
| lt_fields | every field of every userdata type (generated from the `*_opt[]` tables): sector, line, side, vertex, subsector, mobj, player, skin... read and written | 428 |
| lt_api | 200 calls into `P_*`, `R_*`, `G_*`, `M_*`, `S_*`, taglist, blockmap search, thinkers, randoms, in a played demo | 202 |
| lt_hooks | every hook of `lua_hook.h` registered (generic and by type); counts + digests of the arguments over 700 tics of a golden demo (~1.4 M MobjThinker calls) | 31 |
| lt_hooks2 | live game: shield, damage, boss spawn/death, chat, viewpoint, kill/respawn, music, `G_ExitLevel`, intermission, next map; random seed fixed with `-ps2ref-maptics` | 27 |
| lt_err | 30 kinds of runtime error: message, traceback of errors in 6 different hooks, a chunk failing at load | 53 |
| lt_hud | HUD library: patch sizes/offsets of 29 names, sprite and sprite2 patches, string widths (336 combinations), colormaps, 50 drawing calls with good and bad arguments (PC at `-width 320 -height 200`) | 50 |
| lt_local | `addfilelocal` of a Lua file from a hook, `AddonLoaded` | 6 |

All 14 are **SAME** on the final non-LTO ELF (`build/lua-equiv/suite.txt`). `lt_hibyte` (identifiers with bytes >= 0x80) runs on the PS2 only (the Linux C-locale PC build rejects them, the Windows one accepts).
`lt_plat` prints, without comparing, the results that the *unchanged upstream source* gives differently on LP64 Linux and on the PS2/Windows (`strtol` saturation of
`2147483648` and `tonumber("4294967295")`, `%x` of a negative number): these are properties of the platform, not of the port.

### Divergences found by the runs and fixed (all kept as (a))

| difference | symptom before | fix |
|---|---|---|
| `lua_Number` pow outside 32 bit (`2^31`, `10^10`, `0^-1`) | EE saturates to 0x7FFFFFFF, x86 gives 0x80000000 | `luai_numpow` -> `ps2_lua_f2i` (luaconf.h) |
| float / double -> int conversions of console variables and `FloatToFixed` out of range | `lq_float 99538` clamped on the EE, rejected on the PC | `PS2_FloatToI32/DoubleToI32` (m_fixed.h, command.c) |
| `G_TicsToCentiseconds/Milliseconds` | 14 tics -> 39 on the EE (truncating FPU), 40 on the PC | integer form, equal for all 35 values |
| Lua-visible limits (`#states`, `#mobjinfo`, `#sfxinfo`, `MT_FIRSTFREESLOT + n`, `S_`, `SPR_`, `SPR2_`, `SKINCOLOR_`, sfx) were the small live table sizes | `states[9000]` was "out of range", `#mobjinfo` was 660 | scripts and SOC see the PC numbers (`NUM*`); an index past the live table grows the tables once (`PS2_OOR_*`, `PS2_WIDEN`); `LUA_RemapUserdata` keeps references held by scripts valid |
| `R_GetSpriteNumByName` returned the live table size for "not found" | `#sprnames` comparisons wrong | returns `NUMSPRITES` |
| `searchBlockmap("lines")` read the compact UINT16 lists as INT32 | **crash** (TLB miss) in any script | `lib_searchBlockmap_LineCall` + compact branch |
| `Banpyura.SpriteShadow_SetAngle` ignored in the HW renderer | no effect | `hw_main.c` uses the Lua value when `HAS_LUA` |
| side UDMF fields (offsets, scales, light) | ok after `SIDER/SIDEW` | verified by lt_fields |

### Classification of every difference to upstream

| file | difference | class | proof / remark |
|---|---|:--:|---|
| `blua/llex.c` | bytes >= 0x80 are letters in identifiers (`LEX_ISALPHA`) | (a) | newlib's C locale makes `isalpha(0xD1)` false; the PC game runs under the code page of the system (Windows) where L_LithCore-style names compile. lt_hibyte |
| `blua/luaconf.h` | `LUA_INT32` = `int` | (a) | newlib `int32_t` is `long` on the EE, upstream's is `int` (printf/varargs/array types) |
| `blua/luaconf.h` | `luai_numpow` x86 conversion | (a) | lt_vm `pow` lines |
| `deh_lua.c` | `LIMIT_*` free slot tables, `PS2Limits_Grow` before the first slot past the small size, `PS2_FREESLOT_CHECK` | (a) | lt_slots, lt_info (same texts, same numbers, same exhaustion at 1024/8192/1024/9/1024/1600+) |
| `deh_lua.h` | `#ifndef HAS_LUA` inline stubs | (b)-like | compiled only with `SRB2_PS2_NO=lua`; the default build has `HAS_LUA` |
| `lua_baselib.c` | `PS2_OOR_*` range checks | (a) | same message texts and bounds (lt_err, lt_api) |
| `lua_baselib.c` | `gametypedesc[].notes` is a pointer (`PS2_DYNLIMITS`) | (a) | strings under 441 bytes identical; upstream's `strncpy(..., 441)` of a longer description leaves the array unterminated (undefined), the PS2 keeps the whole string |
| `lua_blockmaplib.c` | compact blockmap lists | (a) | lt_api `blockmap_lines`, crash fixed |
| `lua_colorlib.c` | 128 KB nearest-colour memo allocated on first use as `PU_CACHE` | (a) | lt_libs `color_grid` (216 colours) |
| `lua_hook.h` `lua_hooklib.c` | `#ifndef HAS_LUA` stubs | (b)-like | not in the default build |
| `lua_hook.h` `lua_hooklib.c` | `lua_mobjhooks_any` early exit in `LUA_HookMobj/Hook2Mobj/HookMobjLineCollide` (PS2_OPT_PTICK) | (a) | the three functions return `hook.status`, initialised to 0/false, when `mobj_hook_available` is false; the flag is set by `add_mobj_hook`, the only way to register one; lt_hooks (hooks registered) and lt_hooks2 |
| `lua_hooklib.c` | `mobjHookIds` allocated on first hook, `LUA_GrowMobjHooks` | (a) | lt_hooks (hooks by type incl. `MT_FIRSTFREESLOT`-range types) |
| `lua_httplib.c/.h` | `HAVE_CURL` guard | (a) | no libcurl on the PS2: `http.get/post...` cannot reach a network; the pure helpers (`encodeURL`, `decodeBase64`, `buildQuery`...) are equal (lt_libs). **Loss of function for network requests, not closable here** |
| `lua_hud.h` `lua_hudlib_drawlist.h` | stubs | (b)-like | not in the default build |
| `lua_hudlib.c` | `LuaKeepPatch` keeps a sprite patch a script holds from being evicted | (a) | a script keeping `v.cachePatch` userdata no longer gets "patch_t doesn't exist anymore" under memory pressure |
| `lua_infolib.c` | `actionsoverridden` allocated on the first Lua action override; `R_SpriteInfoPivot`; `PS2_WIDEN/OOR` | (a) | lt_info, lt_slots, lt_soc |
| `lua_libs.h` | stubs | (b)-like | not in the default build |
| `lua_maplib.c` | `SIDER/SIDEW` (UDMF side fields in a lazily made array) | (a) | lt_fields side fields |
| `lua_mobjlib.c` | `PS2_OOR_MOBJTYPE` | (a) | lt_fields |
| `lua_script.c/.h` | `PS2Lua_InCall` (zone does not jump out of a failed allocation while a script runs), `LUA_AllocActionsOverridden`, `LUA_RemapUserdata`, `TAGGROUPS_PARAM` | (a) | not visible to scripts except `LUA_RemapUserdata` (lt_info held references) |
| `lua_taglib.c` | `TAGGROUPS_TYPE`, `Taggroup_Lookup` | (a) | lt_api `taglist`/`sectags`/`tagged` |
| `ps2/lua_stub.c` | the stubs of a build without Lua | (b)-like | `tools/ps2/sources.txt`: `# unless:lua`; **not linked in the default build** (`SRB2_PS2_NO=` empty). A build with `SRB2_PS2_NO=lua` keeps the old cut-down profile on purpose |
| `p_enemy.c` | `HAS_LUA` guard of the `LUA_CallAction` prototype; `PS2_OOR_*` | (a) | |
| `p_enemy.c` | PS2-142: arguments of `P_SpawnMobj` / `P_InstaThrust` / `P_SetOrigin` evaluated right to left like the PC builds (random numbers go to the same coordinates) | (a) | DEMO_003 state hash against the PC golden (CORE's find); not Lua-specific |
| `p_enemy.c` | PS2_OPT_PTICK `P_LookForPlayers/Shield` jump over empty player slots | (a) | CORE's, golden demos; not Lua-specific |
| `m_fixed.c` | `FixedHypot` INT32_MIN wrap-abs (my first try) | **(c) removed** | upstream computes `R_PointToDist2` with a negative table index there (undefined, depends on the memory in front of `tantoangle`): not a behaviour to reproduce. Back to upstream |
| `blua/lobject.c` | `strtol` through `long long` (my first try) | **(c) removed** | `strtol` width is a platform property (Windows and the PS2: 32 bit). Back to upstream; the platform dependent results are listed by lt_plat |
| `deh_soc.c` | `get_number` answers from a table (`ps2/soc_numbers.c`) for the expressions of the base game, `LUA_EvalMath` for the rest | (a) | the table is the PC reference log of the expressions (`tools/ps2/soc_strings.tsv`), so the values are the PC's; the add-on expressions go through the real evaluator: lt_soc |
| `command.c m_fixed.h g_game.c info.h sounds.h dehacked.c p_spec.c p_tick.c m_cheat.c r_things.c r_picformats.c ps2_ftest.c hw_main.c` | see the table of divergences above | (a) | |

PS2_PROFILE stubs and `lua_stub.c` do not fire in the default build: `build.py` defines `HAS_LUA` unless `SRB2_PS2_NO` contains `lua`; `nm` of the default ELF has `lua_script.o`'s
`Got_Luacmd`, `LUA_Archive`, no `lua_stub.o` symbols.

### Not run (honest list)

KeyDown / KeyUp (need device events), `NetVars` and the Lua part of savegames and joins (only in a netgame), BotAI / BotTiccmd / BotRespawn, TeamSwitch, ViewpointSwitch (the command ran, no hook call in single player), PlayerQuit, GameQuit, SeenPlayer, ShieldSpecial, LinedefExecute, ShouldJingleContinue,
FollowMobj, HurtMsg: the hooks are registered on both sides (no `addHook` error) but the single-player scenarios never call them. `PlayerCmd` is called once per built ticcmd, which depends on the frame timing of the machine: its count is left out of the comparison.
