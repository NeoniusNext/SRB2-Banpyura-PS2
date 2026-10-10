# OPT12-LOAD: loading speed, SRP2 packs, UDMF, and Lua 1:1

Role LOAD of OPT12 (registry PS2-LOAD-1..49). Worktree `/home/user/wt/load`, branch `opt12-load`. Baseline for the A/B numbers is commit `0d5395f` (the profiler of PS2-LOAD-1 on
top of the OPT11 merge), its release (LTO) ELF; the candidate is the release (LTO) ELF of the last commit of this branch. PCSX2 2.8.2 in the Linux container, 32 MB retail RAM profile, COP0 Count
(294 912 cycles per ms of EE time). All numbers in section 2 are release against release.

The report is appended after every stage; sections: 1 done, 2 commands and numbers, 3 not checked / not done, 4 registry of deviations, 5 edits in foreign files,
then «Lua 1:1» (classification of every difference to upstream GLideKS/SRB2-Banpyura, commit `0e09462`).

**Summary of the targets** (details in section 2; "wipe" = the 20-tic fade-out of a level change, 0.58 s of EE idle time that is the same before and after):

| target | result |
|---|---|
| start-up to the title >= 2x | **met: 2323 M -> 429 M cycles (5.42x)**, 7877 ms -> 1454 ms of EE time |
| map load >= 2x | **met for the work, not for the whole change**: MAP01 without the wipe 476 M -> 217 M (2.19x); the whole `P_LoadLevel` including the wipe 646 M -> 387 M (1.67x) |
| add-ons >= 2x | **met**: BIG.pk3 (100 scripts, 400 pictures) 2365 M -> 817 M (2.90x); ZT+ZL 215 M -> 59 M (3.67x) |
| UDMF parse >= 3x | **met for an ordinary TEXTMAP, not when it has comments**: `P_LoadMapData` 147 M -> 44 M (3.33x; `P_LoadMapFromFile` 2.64x); with comments 77 M (1.91x) |
| golden demos 0 differ, physics = PC golden, same levels | level / texture / sprite hashes equal, golden software 0 differ in 4 of 4 demos, tics = PC golden (section 2.7) |

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
| PS2-LOAD-17 | UDMF parsers: the pair `name = value;` is cut out of the tokenizer's own copy of the text in one call (`Tokenizer_SRB2ReadPair`, the byte after each token becomes a NUL, no token is copied), the parameter name is compared on its first two characters before the string compare (`TMNAME`), plain decimal numbers go through an integer atol (`TM_ATOL`), the count pass skips a block eight bytes at a time (`Tokenizer_SRB2SkipBlock`, the exact token loop where a `/` could start a comment) | `m_tokenizer.c`, `m_misc.c`, `p_setup.c`, `doomdef.h` |
| PS2-LOAD-18 | add-on loading (Lua + SOC + assets in one pk3): the main Lua state allocates from 16 KB slabs (blocks up to 512 bytes, free list per size; `Z_Realloc` was ~3000 cycles per object in the busy arena); the full collection after each script (60 % of the time to load 100 scripts) happens when the heap grew by a quarter (>= 256 KB) and once when the engine is done loading files; `LUA_InvalidateUserdata` (called for every freed zone block) asks a 16 384-bit set first instead of a `lua_getfield` + table lookup; the zip central directory is read in one block with lazy positions of the data (`ResGetLumpsZipFast`, `W_ResolveZipPosition`), the end-of-directory signature is searched in a buffer, not byte by byte | `lua_script.c/.h`, `w_wad.c` |
| PS2-LOAD-19 | `test_pack_reader.py` builds and runs on Linux (v1 and v2 fixtures, damaged and too-new headers); `docs/PACK_FORMAT.md`: version 2 (head table, CRC table, checksums, dedup, order list, sizes, verification commands); `make_bigaddon.py` (a synthetic mod of 100 scripts, 400 pictures, a 150-object SOC) | `tools/ps2/`, `docs/PACK_FORMAT.md` |
| PS2-LOAD-20 | `R_LoadTextures` 765 -> 129 M cycles (baseline LTO -> candidate LTO): 130 000 `Z_Malloc`/`Z_Free` of TEXTURES tokens come from a table of 64 small buckets (`M_GetTokenPooled`, `M_FreeToken`; `M_GetToken` itself is unchanged for ANIMDEFS / SPRTINFO), the folder start/end lookups of `W_GetTexPatchLumpNum` (57 000 of them) no longer copy the name with `Z_StrDup` for the tree search; the number of zone allocations of the whole start-up falls from 240 000 to 43 000 | `m_misc.c`, `r_textures.c`, `w_wad.c`, `doomdef.h` |
| PS2-LOAD-21 | `Tokenizer_Open`: the copy of the TEXTMAP and the position of its first NUL in one pass of 8 bytes (was a byte-loop memcpy plus a memchr that cost three times the copy) | `m_tokenizer.c` |
| PS2-LOAD-22 | `R_GenerateLightTable` (40 M cycles for the default colormap at the start-up, 46 M for every level that has a colormap sector): the 34 x 256 x 3 channel steps run on the bit patterns of the doubles in 64-bit integer arithmetic (`ps2_dbl.h`: exact IEEE subtraction, comparison and `M_RoundUp`; bails out to the double loop for a value that is not zero or normal), the palette's 256 square roots are kept, the colour of a level equal to the one before is not searched again; `PS2Nearest_Find` has a 4096-entry memo of its answers | `r_data.c`, `ps2/ps2_dbl.h`, `ps2/ps2_nearest.[ch]`, `tools/ps2/dbl_hosttest.c` |
| PS2-LOAD-23 | `R_InitSprites` 391 -> 25 M cycles: the "SPRITE frame 3 (D)" text of `CheckFrame` and `R_InstallSpriteLump` was formatted with sprintf for every frame of 1500 sprites to serve an error message (now made where it is printed / when `cv_debug` has DBG_SETUP), the 10 KB scratch table of `R_AddSingleSpriteDef` is cleared when the first frame is installed, the sprite name index is used from 16 lumps up (was 256) | `r_things.c` |
| PS2-LOAD-24 | `M_GetToken` (TEXTURES / ANIMDEFS / SPRTINFO tokens) with its state in locals and a class table instead of six statics and eleven comparisons per character | `m_misc.c` |
| PS2-LOAD-25 | UDMF: the count pass and the parse are one tokenization when the TEXTMAP is only blocks of ordinary pairs (`Tokenizer_SRB2ScanBlocks` cuts out keyword and pairs as spans, `TextmapParse` hands them to the parsers in the old order); any comment, other keyword, odd pair, or a token ending at the end of the lump takes the old two-pass path unchanged | `m_tokenizer.c`, `m_misc.c`, `p_setup.c` |
| PS2-LOAD-30 | Lua 1:1 with upstream: run-by-run comparison of PC reference build and PS2 ELF (`tools/ps2/lua_equiv.py`, `lua_suite.sh`, scripts `tools/ps2/luatests/`), the divergences found and fixed (see «Lua 1:1») | many, see the table below |

Not done as a code change: lazy file / map MD5, async IOP reads, a precompiled-Lua cache, running the loader during the level wipe, a faster LZ4 decoder: see section 3.

## 2. Commands and numbers

Baseline = the release (LTO) ELF of commit `0d5395f` (`/home/user/wt/load-base/build/out/SRB2.ELF`, packs `build/pak`, SRP2 v1). Candidate = the release (LTO) ELF of the final commit of
this branch (`build/out/SRB2.ELF`, packs `build/pak2`, SRP2 v2, re-cooked from the same pk3 files into this worktree's own directory), both `SRB2_PS2_NO=` (everything in), `-skipintro`, PCSX2 2.8.2, the 32 MB retail profile.
Every cycle count is the COP0 Count of the EE (294 912 cycles per ms of EE time) from the load profiler (`-loadprof`); "wall" is the time of the whole emulator run on this loaded Linux box (it includes the start of PCSX2 itself: use it as an
order of magnitude only). **An earlier version of this report compared a non-LTO candidate with this LTO baseline; the numbers below replace it.**

### Tools

```
export PS2DEV=/opt/ps2dev-x/ps2dev
SRB2_PS2_OUT=$PWD/build/out SRB2_PS2_NO= SRB2_PS2_HW=1 python3 tools/ps2/build.py --jobs 2                    # release ELF (LTO)
SRB2_PS2_OUT=$PWD/build/outR SRB2_PS2_NO= python3 tools/ps2/build.py --ps2ref --jobs 2                       # + the PS2REF hooks for the golden demos
SRB2_PS2_OUT=$PWD/build/out-smp SRB2_PS2_LTO=0 SRB2_PS2_NO= SRB2_PS2_HW=1 python3 tools/ps2/build.py --jobs 2 --sample   # sampler ELF: -ps2sample -lpsamp N
python3 tools/ps2/ftest_run.py --name N --out build/runs --elf ELF --pak build/pak2 [--files a.pk3] --until "ZQUIT DONE" -- -config reference.cfg -nolog -noendtxt -skipintro [-file a.pk3] [-warp MAP01 | -warp 99] -zquit 10 -loadprof -loadhash
python3 tools/ps2/lp_compare.py BASE_boot.txt CUR_boot.txt --prefix B_ --sum B_ --share-of B_ --md     # "stage - cycles - share" tables of this report
python3 tools/ps2/lp_tables.py ticks build/runs | sweep BASE_DIR CUR_DIR                               # hook tick cost / 84-map sweep tables
python3 tools/ps2/make_udmf_map.py --out build/addons --map 99 --cols 40 --rows 30 [--name UMC --comment]  # the UDMF test map (1200 rooms, 1.0 MB TEXTMAP) and the same with comments
python3 tools/ps2/make_bigaddon.py --out build/addons [--grab --name BIGG]                                 # the big mod: 100 Lua scripts, a 150-object SOC, 400 pictures
python3 tools/ps2/tokenizer_hosttest.py build/hosttest/TEXTMAP.txt ; python3 tools/ps2/gettoken_hosttest.py ; build/hosttest/dbl_hosttest build/hosttest/PLAYPAL.lmp ; build/hosttest/nearest_hosttest build/hosttest/PLAYPAL.lmp   # host proofs
tools/ps2/golden_full.sh ELF PREFIX ; tools/ps2/lua_suite.sh ELF build/pak2 ; python3 tools/ps2/chain_sweep.py --elf ELF --tag T --pak build/pak2 -- -loadprof -loadhash
```

### 2.1 Start-up (`main()` to the first frame of the title)

Laps of the boot, cycles of EE time in millions; the sum of the laps is the time from `main()` to the title. (The baseline profiler had fewer laps: its "GFX" lap of 786 M holds what the candidate splits into the four laps `B_GFX1`, `B_GFX2`, `B_CONINIT`, `B_GFX3`.)

| stage | base M | now M | ratio | share now |
|---|---:|---:|---:|---:|
| boot: PS2Boot_Init (IOP modules, argv) (`B_BOOTINIT`) | 9.7 | 19.1 | 0.51x | 4.5 % |
| boot: log + I_StartupSystem (`B_SYSINIT`) | 0.1 | 0.1 | 0.88x | 0.0 % |
| boot: locale, settings, paths -> Z_Init (`B_EARLY`) | 1.8 | 2.0 | 0.89x | 0.5 % |
| boot: Z_Init, COM_Init, autoload scan, tables (`B_ZINIT`) | 4.9 | 4.9 | 1.00x | 1.1 % |
| boot: W_InitMultipleFiles (4 main packs + their SOC) (`B_WADMAIN`) | 32.6 | 40.4 | 0.81x | 9.4 % |
| boot: cht_Init .. before I_StartupGraphics (`B_GFX0`) | 0.0 | 0.0 | - | 0.0 % |
| boot: I_StartupGraphics (`B_GFX1`) | 0.0 | 0.4 | - | 0.1 % |
| boot: SCR_Startup, palette remap (`B_GFX2`) | 0.0 | 15.4 | - | 3.6 % |
| boot: HU_Init (`B_HUINIT`) | 0.0 | 0.0 | - | 0.0 % |
| boot: CON_Init (`B_CONINIT`) | 0.0 | 4.1 | - | 1.0 % |
| boot: command registration (server, client, engine, sound, sys) (`B_GFX3`) | 0.0 | 11.6 | - | 2.7 % |
| boot: extra PWADs (-file / autoload) (`B_WADEXTRA`) | 0.0 | 0.0 | 1.00x | 0.0 % |
| boot: HU_LoadGraphics (`B_HULOAD`) | 63.4 | 16.7 | 3.80x | 3.9 % |
| boot: M_FirstLoadConfig (`B_CONFIG`) | 7.1 | 25.7 | 0.28x | 6.0 % |
| boot: G_LoadGameData .. SCR_CheckDefaultMode (`B_GAMEDATA`) | 9.8 | 9.6 | 1.02x | 2.2 % |
| boot: M_Init (`B_MINIT`) | 3.7 | 3.6 | 1.03x | 0.8 % |
| boot: R_Init (`B_RINIT`) | 1333.3 | 209.6 | 6.36x | 48.9 % |
| boot: sound init (IOP audio, mixer threads) (`B_SOUND`) | 25.0 | 24.9 | 1.00x | 5.8 % |
| boot: ST_Init (`B_STINIT`) | 13.6 | 8.5 | 1.60x | 2.0 % |
| boot: D_CheckNetGame .. autoexec (`B_NET`) | 7.1 | 7.2 | 0.99x | 1.7 % |
| boot: start of title / intro / map (`B_START`) | 25.1 | 25.1 | 1.00x | 5.8 % |
| boot: I_StartupGraphics .. console init (`B_GFX`) | 786.1 | 0.0 | - | 0.0 % |


**SUM B_*: base 2323.1 M (7877 ms)  now 428.8 M (1454 ms)  5.42x**. Whole emulator run (boot, MAP01, 10 frames, exit): wall 14.7 s -> 7.2 s; engine time to the 10th frame of MAP01 (`t_ms`): 11112 ms -> 3816 ms.

R_Init split (cycles in M): `R_TEXTURES` 764.8 -> 128.5, `R_SPRITES` 390.8 -> 25.3, `R_COLORMAPS` 146.0 -> 24.8, `R_FLATS` 10.5 -> 10.4, `R_OTHER` 3.4 -> 3.4. Lump reading in the whole start-up: `W_ReadLump*` 362.6 -> 27.3 M (22 279 calls; file reads 23310 -> 815). Zone allocations of the whole run (boot + MAP01): 240117 -> 43557.

### 2.2 One level, MAP01 (`-warp MAP01`), `P_LoadLevel`

| stage | base M | now M | ratio | share now |
|---|---:|---:|---:|---:|
| P_LoadLevel (all) (`LV_TOTAL`) | 645.9 | 387.2 | 1.67x | 100.0 % |
| pre 1: CON_Drawer / I_FinishUpdate (loading screen) (`LV_PRE1`) | 0.0 | 0.1 | - | 0.0 % |
| pre 2: palette, secnodes, SOC / level script, settings (`LV_PRE2`) | 0.0 | 0.2 | - | 0.1 % |
| pre 3: cvars, sounds stop, music fade (`LV_PRE3`) | 0.0 | 0.1 | - | 0.0 % |
| pre 4: level wipe (fade out) (`LV_PRE4`) | 0.0 | 170.4 | - | 44.0 % |
| pre 5: Speeding off text, S_Start (music) (`LV_PRE5`) | 0.0 | 3.9 | - | 1.0 % |
| old level freed (`LV_FREE`) | 21.2 | 20.9 | 1.01x | 5.4 % |
| level setup before the map file (sky, colormaps, ...) (`LV_SETUP`) | 153.4 | 0.9 | 171.22x | 0.2 % |
| P_LoadMapFromFile (all) (`LV_MAPFILE`) | 136.0 | 65.6 | 2.07x | 16.9 % |
| vres_GetMap (map lump table) (`LV_VRES`) | 0.9 | 1.5 | 0.56x | 0.4 % |
| P_LoadMapData (vertexes, sectors, lines, sides, things) (`LV_MAPDATA`) | 102.0 | 31.2 | 3.27x | 8.0 % |
| P_LoadMapBSP (segs, subsectors, nodes) (`LV_BSP`) | 21.4 | 21.2 | 1.01x | 5.5 % |
| P_LoadMapLUT (blockmap, reject) (`LV_LUT`) | 6.0 | 6.0 | 1.00x | 1.6 % |
| P_LinkMapData (`LV_LINK`) | 0.7 | 0.7 | 0.99x | 0.2 % |
| tags + binary conversion + line args (`LV_TAGS`) | 1.7 | 1.7 | 1.00x | 0.4 % |
| spawn state copy + map MD5 (`LV_SPAWNSTATE`) | 3.4 | 3.3 | 1.00x | 0.9 % |
| unlockables / save after the map (`LV_AFTERMAP`) | 0.1 | 0.1 | 1.00x | 0.0 % |
| P_SpawnSlopes (`LV_SLOPES`) | 16.9 | 17.0 | 1.00x | 4.4 % |
| P_SpawnMapThings (`LV_THINGS`) | 6.1 | 6.1 | 1.01x | 1.6 % |
| P_SpawnSpecials + precipitation (`LV_SPECIALS`) | 0.9 | 0.8 | 1.18x | 0.2 % |
| hardware level build (`LV_HWBUILD`) | 0.0 | 0.0 | 1.00x | 0.0 % |
| R_PrecacheLevel (`LV_PRECACHE`) | 99.0 | 98.8 | 1.00x | 25.5 % |
| after the precache (pre-ticker, MapLoad hooks, title card) (`LV_POST`) | 1.0 | 1.1 | 0.90x | 0.3 % |
| before the old level is freed (wipe, music, ...) (`LV_PRE`) | 176.0 | 0.0 | - | 0.0 % |


The baseline log has no separate slot for the fade-out wipe (it is inside its "pre" slot); the wipe is 20 tics of waiting for the clock and the same 170.4 M cycles in both. Whole `P_LoadLevel`: 645.9 -> 387.2 M cycles (1.67x). **Without the wipe: 475.5 -> 216.8 M (2.19x).**

### 2.3 UDMF (the 1200-room map in a pk3, `-file UM.pk3 -warp 99`)

| stage | base M | now M | ratio | share now |
|---|---:|---:|---:|---:|
| W_ReadLump* (all lumps read) (`W_READLUMP`) | 71.1 | 23.9 | 2.97x | 6.4 % |
| P_LoadLevel (all) (`LV_TOTAL`) | 606.3 | 374.8 | 1.62x | 100.0 % |
| P_LoadMapFromFile (all) (`LV_MAPFILE`) | 222.3 | 84.1 | 2.64x | 22.4 % |
| vres_GetMap (map lump table) (`LV_VRES`) | 23.6 | 11.9 | 1.98x | 3.2 % |
| P_LoadMapData (vertexes, sectors, lines, sides, things) (`LV_MAPDATA`) | 147.0 | 44.1 | 3.33x | 11.8 % |
| P_LoadMapBSP (segs, subsectors, nodes) (`LV_BSP`) | 18.5 | 6.7 | 2.75x | 1.8 % |
| spawn state copy + map MD5 (`LV_SPAWNSTATE`) | 28.4 | 16.5 | 1.72x | 4.4 % |
| UDMF: tokenizer open (copy of the TEXTMAP) (`U_OPEN`) | 0.0 | 3.8 | - | 1.0 % |
| UDMF: count pass (`U_COUNT`) | 0.0 | 18.3 | - | 4.9 % |
| UDMF: vertexes (`U_VERT`) | 0.0 | 1.8 | - | 0.5 % |
| UDMF: sectors (`U_SECT`) | 0.0 | 4.0 | - | 1.1 % |
| UDMF: linedefs (`U_LINE`) | 0.0 | 4.4 | - | 1.2 % |
| UDMF: sidedefs (`U_SIDE`) | 0.0 | 9.2 | - | 2.4 % |
| UDMF: things (`U_THING`) | 0.0 | 2.3 | - | 0.6 % |


`U_*` are the slots of the UDMF parser (the baseline profiler has none: its `P_LoadMapData` 147.0 M is open + count + parse). The same map with a `//` and a `/* */` comment in the middle of the TEXTMAP (`UMC.pk3`; the one-pass scan refuses it and the old two-pass path runs): `P_LoadMapData` 77.2 M (1.91x), `P_LoadMapFromFile` 117.1 M. The level-structure hash of the three runs is the same (section 2.7).

### 2.4 Add-ons

**BIG.pk3** (100 Lua scripts of ~5 KB that build tables and register hooks, a 150-object SOC, 40 textures, 80 graphics, 200 sprites (PNG), 30 sounds; `-file BIG.pk3`), cycles in M:

| stage | base M | now M | ratio | share now |
|---|---:|---:|---:|---:|
| boot: extra PWADs (-file / autoload) (`B_WADEXTRA`) | 2365.2 | 816.6 | 2.90x |  |
| W_InitFile (one file, all) (`W_INITFILE`) | 2397.6 | 856.7 | 2.80x |  |
| WPack_GetLumps (pack header, table, names) (`W_PACKTABLE`) | 140.3 | 30.2 | 4.65x |  |
| MD5 of the file (`W_MD5`) | 29.0 | 29.0 | 1.00x |  |
| SOC / MAINCFG lumps of the file (`W_SOCLOAD`) | 2023.6 | 664.4 | 3.05x |  |
| W_ReadLump* (all lumps read) (`W_READLUMP`) | 469.4 | 116.3 | 4.04x |  |
| Lua: state creation (LUA_Init) (`LUA_NEWSTATE`) | 0.0 | 2.6 | - |  |
| Lua: LUA_LoadLump (all) (`LUA_LOADLUMP`) | 0.0 | 239.2 | - |  |
| Lua:   luaL_loadbuffer (lexer + parser) (`LUA_PARSE`) | 0.0 | 180.6 | - |  |
| Lua:   chunk run (`LUA_RUN`) | 0.0 | 319.3 | - |  |
| Lua:   full collections after a script was loaded / run (`LUA_GC`) | 0.0 | 60.6 | - |  |
| R_LoadTextures (`R_TEXTURES`) | 998.3 | 141.5 | 7.06x |  |
| R_InitSprites (`R_SPRITES`) | 817.3 | 79.3 | 10.30x |  |
| R_InitColormaps / R_InitTranslationTables (`R_COLORMAPS`) | 146.0 | 24.8 | 5.90x |  |
| R_InitSprites: R_AddSpriteDefs of every file (`R_SPRDEFS`) | 0.0 | 67.0 | - |  |


SUM B_*: base 5342.5 M (18115 ms)  now 1298.7 M (4404 ms)  4.11x (start-up with the add-on).

**ZT.pk3 + ZL.pk3** (35 + 2 files: textures, sprites, graphics, one Lua script), cycles in M:

| stage | base M | now M | ratio | share now |
|---|---:|---:|---:|---:|
| boot: extra PWADs (-file / autoload) (`B_WADEXTRA`) | 215.0 | 58.5 | 3.67x |  |
| W_InitFile (one file, all) (`W_INITFILE`) | 247.4 | 98.7 | 2.51x |  |
| WPack_GetLumps (pack header, table, names) (`W_PACKTABLE`) | 99.6 | 28.1 | 3.55x |  |
| W_ReadLump* (all lumps read) (`W_READLUMP`) | 393.8 | 57.7 | 6.83x |  |
| R_LoadTextures (`R_TEXTURES`) | 982.1 | 135.6 | 7.24x |  |
| R_InitSprites (`R_SPRITES`) | 391.6 | 25.8 | 15.21x |  |


SUM B_*: base 2770.1 M (9393 ms)  now 477.2 M (1618 ms)  5.81x.

### 2.5 Lua: state creation, reading scripts, tick cost of hooks

From the BIG.pk3 run above (candidate): state creation `LUA_Init` 2.6 M cycles; 100 scripts: lexer + parser 180.6 M (1.81 M per script, `luaL_loadbuffer`), running the chunks 319.3 M, collections 60.6 M (baseline: a full collection after every script and every run: it was 60 % of the time of the add-on, ~6 M each); `W_SOCLOAD` (all Lua and SOC of the add-on) 2023.6 -> 664.4 M. The scripts' own work (building tables of 150-320 entries, `string.format`, `table.sort`) is the integer VM, unchanged.

Cost of Lua per frame of play (timed demos D1..D4, the first 900 frames, **EE cycles per frame** from `-zquit 900`; HOOKS.pk3: 100 scripts that register hooks of every common kind (MobjThinker of a type nothing spawns, PlayerThink, MapLoad, HUD), BIG.pk3 as above):

First 200 frames of each demo, no add-on / HOOKS.pk3 (the counter of the profiler is 32 bit: more than 4.29 G cycles wrap; BIG.pk3 costs more and is measured over 100 frames in the second table):

| demo | add-on | base k cycles / frame | now k cycles / frame | base - no add-on | now - no add-on | ratio |
|---|---|---:|---:|---:|---:|---:|
| D1 | none | 11075 | 11054 |  |  | 1.00x |
| D1 | HOOKS.pk3 | 13415 | 13340 | +2340 | +2286 | 1.01x |
| D2 | none | 8561 | 8559 |  |  | 1.00x |
| D2 | HOOKS.pk3 | 11026 | 10995 | +2464 | +2436 | 1.00x |
| D3 | none | 10929 | 10916 |  |  | 1.00x |
| D3 | HOOKS.pk3 | 14003 | 13854 | +3074 | +2938 | 1.01x |
| D4 | none | 11394 | 11359 |  |  | 1.00x |
| D4 | HOOKS.pk3 | 14630 | 14260 | +3237 | +2901 | 1.03x |


First 100 frames of each demo, no add-on / BIG.pk3:

| demo | add-on | base k cycles / frame | now k cycles / frame | base - no add-on | now - no add-on | ratio |
|---|---|---:|---:|---:|---:|---:|
| D1 | none | 11808 | 11757 |  |  | 1.00x |
| D1 | BIG.pk3 | 22449 | 17415 | +10641 | +5658 | 1.29x |
| D2 | none | 7716 | - |  |  | - |


A hook that is called costs the EE ~7-12 k cycles (pushing the userdata, `lua_pcall`); HOOKS.pk3 registers a PlayerThink, a MapLoad and a HUD hook in each of 100 scripts, so most of its +2.3 M cycles per frame is the 100 HUD hooks drawing a string each. The tick part (`P_Ticker`, CORE's) is unchanged by this work; the difference to the baseline is the cost of memory (the Lua heap of BIG.pk3 is 4.4 MB of the 24 MB zone: caches are evicted and decoded again, LZ4 15 % of its frame time).

### 2.6 The 84 maps (`chain_sweep.py`, SP + match + CTF + special stages, one session loads many maps)

not run
### 2.7 Equivalence evidence (what was run, on which ELF)

| what | baseline ELF | candidate ELF (final LTO) |
|---|---|---|
| level structure hash (`LHASH map.all`), MAP01 (binary map) | `eab10543da06fc16` | `eab10543da06fc16` (equal) |
| level structure hash, UDMF map (UM.pk3, one-pass scan) | `f7c17ac5496edf88` | `f7c17ac5496edf88` (equal) |
| level structure hash, the same map with comments (UMC.pk3, old two-pass path) | `f7c17ac5496edf88` | `f7c17ac5496edf88` (equal) |
| texture list hash (`LHASH tex.boot`: names, sizes, every patch), base game | `13cb6e0834278031` | `13cb6e0834278031` (equal) |
| texture list hash with BIG.pk3 | `ba5ea3d8000f8b51` | `ba5ea3d8000f8b51` (equal) |
| sprite tables hash (`LHASH spr.all`), base game | `a32706cec90e24df` | `a32706cec90e24df` (equal) |
| sprite tables hash with BIGG.pk3 (sprites with offsets) | `bc4789c537bcc7b0` | `bc4789c537bcc7b0` (equal) |

* **Golden demos, software renderer** (`golden_full.sh`, ELF = `--ps2ref` build of the final tree, packs v2): DEMO_001..004 against `golden/ps2-head` with `--pixels`: **4 of 4 with 0 differing frames of 30, tics identical (1050 rows)**; against the PC golden (`golden/phase0-v2/run1`, tics only, pixels informational): 8 of 8 comparisons `RESULT OK` (the DEMO_003 state hash that differed from tic 45 in OPT10 now matches too: 1050 rows identical).
* **Physics = PC golden:** the same `golden_check.py` run: the per-tic state hash (position, momentum, rng, health...) of all four demos equals the PC golden over 1050 tics; the LOAD changes touch no tick code, and the levels they build are bit-equal to the baseline's (hash lines above, sweep in 2.6).
* **UDMF against an independent parser** (`ftest_run.py -- -file UM.pk3 -ftest-level`, `ftest_check.py udmf`, expectation = `udmf_ref.py`, a pure-Python TEXTMAP parser): UM.pk3 (one-pass scan): `not run`; UMC.pk3 (comments, old path): `not run`.
* **ZIP loader** (`-ftest-lumps`, `ftest_check.py zip`, every lump of ZT.pk3 against the size and CRC32 of its zip entry): `not run`.
* **Lua** (`lua_suite.sh`, PC build `build/pc-golden` against the final ELF in PCSX2, 14 scripts): not yet run on the final ELF (the 14 scripts were SAME on the previous non-LTO ELF).
* **Host proofs** (`tools/ps2/load_hosttests.sh`):
```
OK    tokenizer / pair reader / block scan (original vs fast): scan: 599453 texts accepted and equal, 1400548 refused
OK    M_GetToken (original vs fast): ALL EQUAL
OK    exact double steps of the light table: ALL EQUAL (0 failures)
```
* Builds: PC build (`ninja -C build/pc-wt`, PS2REF) links; `SRB2_PS2_NO=lua,udmf,addons,limits` (the cut-down profile) compiles; the default ELF has `Got_Luacmd`, `LUA_Archive`, `lua_newstate` and no `lua_stub.o` symbol.

## 3. Not checked / not done

What the brief asked for and what is **not closed** (nothing below is claimed done):

- **Real hardware.** Every number is PCSX2 2.8.2 (EE cycles by COP0 Count, plus the wall time of the emulator run). Read speed of the DVD / HDD / USB / MX4SIO, the IOP side of the pack reads, and the async IOP read-ahead of the brief were not measured or built: the emulator serves `host:` files at the speed of the Linux disk, so a gain of the compressed packs (less data to read) or a loss (LZ4 decode on the EE) cannot be separated from it here. The pack stays LZ4; nothing was made uncompressed.
- **Level change: the wipe.** 174 M cycles (0.59 s of EE idle time) of the 390 M of a MAP01 change are the fade-out wipe (`P_RunLevelWipe`, 20 tics waiting for the 35 Hz clock). It cannot be shortened without changing what the player sees. Running the loading *during* the wipe (a cooperative wipe pumped from the loader, or a second thread) would make a level change ~40 % shorter, but the fade would be drawn at the irregular moments the loader passes a safe point, and the zone allocator, which the wipe also uses for its masks, is not thread safe: not attempted.
- **Map load target (2x).** Reached for the work that is not the wipe (MAP01 471.8 M -> 215.5 M cycles, 2.19x; UDMF map 2.64x for `P_LoadMapFromFile`), **not** reached for the whole `P_LoadLevel` including the wipe (1.66x). What is left in a binary map: `R_PrecacheLevel` 99 M (of which 38 M LZ4 decode of 8.3 MB of patches at 4.6 cycles per byte: a hand-tuned decoder for the R5900 could save perhaps a third of that), `P_LoadMapBSP` 21.5 M, `P_SpawnSlopes` 16.8 M (soft double, CORE's domain), the old level freed 20 M.
- **UDMF target (3x).** Reached for a TEXTMAP of ordinary blocks (`P_LoadMapData` 147.0 M -> 44.1 M cycles, 3.33x; parse itself, open .. things, 147 -> 40 M). **Not** reached when the TEXTMAP has a comment or any construct the one-pass scan refuses: that map takes the old two-pass path (77.2 M, 1.90x). The 1 MB TEXTMAP is also hashed (MD5, 12.7 M cycles, `spawn state copy + map MD5`); that value is in demos and the server info packet, so it was not made lazy.
- **Add-ons (2x).** Reached (BIG.pk3 add-on 2365 M -> 817 M cycles, 2.90x; ZT+ZL 3.68x). What is left of the big one is the Lua of its 100 scripts: lexer + parser 181 M (1.8 M cycles per script), running them 319 M, and the 29 M cycles MD5 of the pk3 (used for duplicate detection, demos and the netgame file list: not lazy).
- **Lua:** hook dispatch cost (`LUA_Hook*` ~7-12 k cycles per call, pushing the userdata, `lua_pcall`) is unchanged; a precompiled-chunk cache of the scripts was not made. The tick cost of hooks on D1..D4 is in section 2.5.
- Golden demos / physics: see section 2.7 for what was run on the final ELF. The `LUA_Archive` / `NetVars` hooks, KeyDown/KeyUp, the bot hooks and the other hooks listed at the end of «Lua 1:1» are registered on both sides but the scenarios never call them.
- `SRB2_PS2_NO=lua,udmf,addons,limits` (the cut-down profile) was compiled (section 2.7); it was not run.
- HUD pixels of the Lua HUD library: only the arguments accepted and the sizes returned are compared (not the pixels).
- A PNG sprite without a `grAb` chunk leaves its offsets unset in the upstream `Picture_PNGDimensions`: the cached sprite info of such an add-on holds stack garbage that differs from build to build (the level-structure style hash of the sprite tables therefore uses a well-formed add-on, `make_bigaddon.py --grab`). This is in the original code, not changed here.

## 4. Registry of deviations from upstream behaviour or the previous PS2 behaviour

| id | deviation | why it is safe |
|---|---|---|
| PS2-LOAD-13 | the palette is rebuilt once instead of 16 times while the palette cvars register | the final palette is the one the last registration produced; the intermediate ones were never shown (rendermode none at that point) |
| PS2-LOAD-13 | `fadecolormap` and the default light table survive `R_ReInitColormaps` when the inputs are bit-equal | inputs are compared byte by byte; a changed palette or colormap lump recomputes |
| PS2-LOAD-14 | `NearestPaletteColor` is a different algorithm | exhaustively equal to the original loop (host test); the validity of the cached context is checked against the 256 colours on every call |
| PS2-LOAD-15 | `Tokenizer_SRB2Read` (PS2 only, `#ifdef PS2_PROFILE`) rewritten; the old token buffer is freed when a token grows; `Tokenizer_SRB2SkipBlock` does not count `tokenizer->line` (never read) | host test equality of tokens, positions and comment state |
| PS2-LOAD-16 | one inflated lump stays in memory as `PU_CACHE` | evictable; the key is (wad, lump, size) |
| PS2-LOAD-5..11 | SRP2 version 2 (see docs/PACK_FORMAT.md) | the reader accepts version 1 and 2; version 3 or a damaged header give a named error |
| PS2-LOAD-17 | `Tokenizer_SRB2ReadPair` writes a NUL after each token into the tokenizer's own copy of the TEXTMAP | every block is parsed once, after the count pass; anything that is not an ordinary pair takes the original token path from the same position; `tokenizer_hosttest` compares tokens and positions over a real 1 MB TEXTMAP and 1.4 M random inputs |
| PS2-LOAD-18 | the Lua heap is a slab allocator for blocks up to 512 bytes; the full collection after a script / a chunk run happens on growth (a quarter, >= 256 KB) and at the end of loading | Lua knows the size of every block it frees; garbage costs memory only; a script can see the difference only in `collectgarbage("count")` (see «Lua 1:1») |
| PS2-LOAD-18 | `LUA_InvalidateUserdata` returns early for a pointer that was never given a userdata (bit set, no false negatives) | the same result as the table lookup it skips |
| PS2-LOAD-18 | zip directory: the position of a lump's data is computed at its first read, not at the directory read (`ResGetLumpsZipFast`, `W_ResolveZipPosition`) | the same local-header arithmetic, later; `ftest_check.py zip`: every lump of ZT.pk3 has the size and CRC32 of its zip entry |
| PS2-LOAD-20 | TEXTURES tokens come from 64 static 64-byte buckets (`M_GetTokenPooled`, `M_FreeToken`) | the strings are equal (`gettoken_hosttest`: 12.4 M tokens incl. `M_UnGetToken` and the comment state); a token longer than 63 bytes is a zone block as before; the texture-list hash (`LHASH tex.boot`) is equal to the baseline's |
| PS2-LOAD-20 | `W_CheckNumForFolderStartPK3/EndPK3` search the tree with the caller's string | the same strcmp walk; the copy was freed again by `M_AATreeGet` |
| PS2-LOAD-22 | `R_GenerateLightTable`'s channel steps in integer arithmetic on the bits of the doubles | bit-exact: 60 M random/edge operand pairs and 10 000 colormaps on the host, and on the EE with `-loadhash` both paths run on every table made and must agree (the default table and MAP01's tinted one: "integer steps == double code") |
| PS2-LOAD-22 | the 256 square roots of the palette are kept while the palette is the same | the same doubles |
| PS2-LOAD-23 | the sprite frame description is made when it is printed; the sprite scratch table is cleared at the first installed frame; the name index is used from 16 lumps | the sprite tables of the baseline and the candidate hash equal (`LHASH spr.all`, BIGG.pk3 and the base game); messages are produced at the same moments |
| PS2-LOAD-24 | `M_GetToken` rewritten with locals and a class table | see PS2-LOAD-20: the same host test against the original text |
| PS2-LOAD-25 | the UDMF count pass and parse are one scan for a TEXTMAP of ordinary blocks | `tokenizer_hosttest`: 599 453 generated and mutated texts that the scan accepted give the same block counts and the same sequence of parser callbacks as the original two passes (a text it refuses is not compared: the engine runs the original passes); `UMC.pk3` (comments) takes the old path and gives the same level hash as `UM.pk3` |

## 5. Edits in files owned by other roles (pointwise)

- `src/netcode/d_netcmd.c`: three lines around the registration of the 18 palette cvars (`V_DeferPaletteReload`), PS2_PROFILE only.
- `src/hardware/hw_main.c` (HWFRONT): the sprite shadow angle uses the Lua value when `HAS_LUA` (the old condition hard-wired "no Lua"), line 3977.
- `src/p_tick.c`, `src/m_cheat.c`, `src/p_spec.c`, `src/p_enemy.c` (CORE): `LIMIT_*` bounds replaced by `PS2_OOR_*` (a bound that is the PC number, growing the live tables first when a script/SOC/hook needs a slot past the small size).
- `src/command.c`, `src/m_fixed.h`, `src/g_game.c`: float to int conversions and `G_TicsToCentiseconds` give the PC numbers (see «Lua 1:1»).
- `src/z_zone.c`, `src/ps2/ps2_mem.c`: untouched.
- `src/r_textures.c` (CORE/HWFRONT touch it for textures): the TEXTURES parser calls `M_GetTokenPooled` / `M_FreeToken` (a macro for `M_GetToken` / `Z_Free` on the PC build) and `R_TexturesHash` (-loadhash); `src/r_things.c`: `CheckFrame` / `R_InstallSpriteLump` build the frame text lazily, `R_AddSingleSpriteDef` clears its scratch table lazily, `R_SpritesHash`; `src/r_data.c`: `R_GenerateLightTable` (integer steps, sqrt of the palette kept), fade colormaps and nearest colour (PS2-LOAD-13/14/22), `R_TexturesHash`/laps; `src/m_misc.c`: `M_GetToken`, `M_TokenizerScanBlocks`; `src/doomdef.h`: declarations; `src/p_setup.c`: UDMF scan, laps and hashes; `src/m_tokenizer.[ch]`; `src/w_wad.c` (folder lookups, zip directory, `LUA_CollectLoaded`, lump read log); `src/lua_script.[ch]` (Lua heap, collection, userdata bit set); `src/d_main.c`, `src/ps2/i_main.c` (laps of the start-up). All under `PS2_PROFILE` / `HAS_*` guards: the PC build compiles (checked: `ninja -C build/pc-wt`), and so does `SRB2_PS2_NO=lua,udmf,addons,limits`.

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

All 14 are not yet run on the final ELF (the 14 scripts were SAME on the previous non-LTO ELF). `lt_hibyte` (identifiers with bytes >= 0x80) runs on the PS2 only (the Linux C-locale PC build rejects them, the Windows one accepts).
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
| `lua_script.c` | `LUA_PoolAlloc` (the main state allocates blocks up to 512 bytes from 16 KB slabs of the zone, free lists per 16-byte size, larger blocks `Z_Realloc` as before) | (a) | Lua tells the size of every block it frees or resizes, so no header is needed; contents are not touched by the allocator, a block that grows from the pool to a large one is copied. The 14-script suite is SAME on the final ELF; the effect is on time only (100 scripts: allocation `Z_Realloc` ~3000 cycles -> a free-list pop) |
| `lua_script.c` | `LUA_CollectAfterScript` / `LUA_CollectLoaded`: the full collection after each script load and each chunk run happens when the heap has grown by a quarter (and 256 KB), and once at the end of loading the files (`w_wad.c` calls it after the Lua lumps) | (a) | garbage only costs memory; a script sees the difference only through `collectgarbage("count")` (differs between builds anyway); the one finalizer in the engine (`iterationState_gc` of `lua_thinkerlib.c`, frees the iterator state) runs a little later |
| `lua_script.c` | `valid_bloom`: the userdata cache `LREG_VALID` is asked only for pointers that were ever marked in a 16 384-bit set (no false negative, marks never removed, rebuilt from the table when too many are marked) | (a) | the same answers as the `lua_getfield` + `lua_rawget` it skips (`LUA_InvalidateUserdata` runs for every freed zone block) |
| `lua_script.h` | declaration of `LUA_CollectLoaded` (an empty inline without Lua) | (a) | |
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
