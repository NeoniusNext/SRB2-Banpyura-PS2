# OPT14-LUA: Lua errors on the PS2 (user complaint: "Lua errors on PS2, none on PC")

Worktree `/home/user/wt/lua`, branch `opt14-lua`, base `ece6e7c` (merged OPT13 IQ/IS/IZ/IR/IO). Reference: PC build `build/pc-golden` (x86-64 Linux), upstream 0e09462.
Tools: `tools/ps2/lua_equiv.py` (PC against PS2 ELF in PCSX2, `LQ` lines + WARNING/ERROR lines with tracebacks), `tools/ps2/lua_suite.sh`, scripts `tools/ps2/luatests/`.

## 1. Step 1: the OPT12-LOAD suite on the merged ELF (task g)

ELF: `SRB2_PS2_OUT=build/out SRB2_PS2_NO= SRB2_PS2_HW=1 python3 tools/ps2/build.py --jobs 2` (release, LTO, 203 files), run: `sh tools/ps2/lua_suite.sh build/out/SRB2.ELF build/pak2`.

All 15 scripts are SAME (0 differing lines) on the merged ELF: lt_vm 88, lt_math 59, lt_globals 50, lt_info 507, lt_libs 33, lt_slots 12 928, lt_soc 9, lt_fields 428, lt_api 202, lt_hooks 31,
lt_hooks3 17, lt_err 53, lt_hud 50, lt_hooks2 27, lt_local 6 lines. (Software renderer, single player. The suite found no divergence caused by the OPT13 merge: IQ-5 struct layout, IQ-6 hook mask, IS-703 interpolation state.)

Conclusion of step 1: the existing suite does not reproduce the complaint. Steps 2.. build bigger "mod like" scenarios and look at the code paths the suite cannot reach.

## 2. Found and fixed

### PS2-LUA-1: userdata made inside a coroutine were never invalidated (root: `if (L == gL)` in the PS2 bit set)

* Cause: `LUA_RawPushUserdata` marked a pointer in the bit set `valid_bloom` only for `L == gL` (the main thread). A coroutine (`coroutine.wrap/create`) runs on its own `lua_State`, the pointer was never marked,
  `LUA_InvalidateUserdata` (called by the zone for every freed block and by `P_RemoveThinker`) then returned early ("never had a userdata") and the userdata kept its stale pointer.
  Also the per-userdata `EXTVARS` entry (the fields a script stores in a mobj) survived.
* Symptom on the PS2 (script `tools/ps2/luatests/lt_coro.lua`, `lua_equiv.py --warp 1`): a mobj reference taken in a coroutine and the object removed; once the memory is reused by a new mobj (`mobjcache`
  recycles the slice at once) the old reference is `valid == true` and reads the **new, unrelated object**; the PC raises "accessed mobj_t doesn't exist anymore". 6 of 37 lines differed.
* Fix (`src/lua_script.c`): mark on every lua_State (the registry is shared). After the fix `lt_coro`: 37 LQ lines, SAME.

### PS2-LUA-2: more than 4096 live userdata made every new userdata rebuild the bit set: O(n^2), a stall of tens of seconds

* Cause: the bit set (16 384 bits) was rebuilt from the registry table whenever `valid_marked > 4096` (marks since the last rebuild). With more than 4096 live entries the rebuild left `valid_marked > 4096`
  and the next new userdata rebuilt again (a `lua_next` walk of the whole table each time). A script that touches thousands of objects (every `for mo in mobjs.iterate()` on a crowded level, `lines[i]` of a big map,
  a bot or scoreboard mod) is such a script: the registry keeps every userdata until its object dies.
* Measured (script `lt_udmany.lua`, MAP11 25 100 lines, PCSX2, release ELF): 9000 `lines[i]` userdata **28 691 ms** (before) against **107 ms** (after); 600 `P_SpawnMobj` after that 5027 ms against 21 ms.
* Fix: exact live count (`valid_live`), set of 65 536 bits (8 KiB .bss, was 2 KiB), rebuild only while the live entries fit (<= 1/8 of the bits, false positive rate <= 12 %), otherwise the set is off and the table is asked for
  every freed block (the upstream behaviour); it comes back (rebuilt by the next userdata push, not inside the zone) when the live entries fall under 1/16. `LUA_InvalidateUserdata` and the remap use the registry reference
  (no `lua_getfield` name lookup).
* After the fix `lt_udmany`: SAME (8 LQ lines).

### Memory note found on the way

Each live userdata costs ~100 bytes of Lua heap (userdata + registry node + the script's own reference): 12 000 line userdata + 3000 mobjs on MAP11 end with "Not enough memory to draw map MAP11 (PU_RENDERWORK)" and a return to the title.
Upstream has the same cost on the PC where it does not matter. Not changed (the script is an extreme; the PS2 handles 9000 + 600 with the renderer warnings "R_GenerateTexture: no room" that the PC does not have).

## 3. The stand (what was written and what it found)

All scripts are in `tools/ps2/luatests/` and are run by `tools/ps2/lua_equiv.py` (PC reference against the PS2 ELF); `lua_suite.sh` has them all. `lua_equiv.py` gained `--pc-only`, a cut of the lines after the end line,
path normalisation of add-on lumps (`NAME.pk3|lump`), and it ignores the PS2 memory notices ("Low memory", "R_GenerateTexture: no room") in the alert lines.

| script | what a mod does there | PC against PS2 (release ELF) |
|---|---|---|
| lm_data | 12 000-element arrays and sorts, hash tables of 8 000 keys, strings (gsub/gmatch/format/rep, base64, crc with `^^`), closures, metatables, weak tables, coroutines, pcall/error, recursion, a 4 000-record structure kept while garbage is made | 75 lines SAME |
| lm_objects | 3 freeslot types, 6 states, hooks by type and generic, bombs circle the player and spawn sparks, `mo.lm_*` Lua fields, P_KillMobj/P_RemoveMobj, `mobjs.iterate()` every tic, event hash over 630 tics (14 000 events) | 32 lines SAME |
| lm_world | sector waves (floor/ceiling/light/flat offsets, scales, flats), side textures/offsets, FOF edits, P_FloorzAtPos/P_CheckPosition/P_TeleportMove, error cases of read-only fields | 108 lines SAME |
| lm_hud | ~150 draw calls per frame in every font/flag, patches kept in tables across a level change, hooks of every HUD type, hud.enable/disable | 13 lines SAME in Software and in `-renderer Hardware` |
| lt_io | `io.openlocal` write/read/seek/lines/append/binary, denied names, `os.time/date/clock/difftime` | 30 lines SAME |
| lt_pk3 (`make_luapk3.py`) | a pk3 whose Lua folder has nested folders, mixed case, CRLF, UTF-8 text, a 12 000-record script, a syntax error, a runtime error, `return`, empty, non-lua files in Lua/ | 20 lines SAME (run order = zip order, as on the PC) |
| lt_stack, lt_stack2 | 200-level C recursion (pcall, metamethods, gsub, sort, coroutines, error handlers) and recursion through the engine (P_KillMobj <-> MobjDeath, P_SpawnMobj <-> MobjSpawn, P_RemoveMobj <-> MobjRemoved) | SAME incl. the stack tracebacks (79 lines) |
| lt_coro | PS2-LUA-1 | was DIFFERENT (6 of 37 lines), SAME after the fix |
| lt_udmany | PS2-LUA-2 | SAME; 28.7 s -> 0.1 s |
| lt_math without FINEACON.DAT | acos/asin tables when the file is missing (`tables.c` computes them) | the 59 values are SAME, one extra WARNING "FINEACON.DAT not found: acos is computed" |

The Lua suite of OPT12-LOAD (15 scripts) plus the new ones were also run on the `--debug` ELF (ZDEBUG red zones, RANGECHECK, PARANOIA; `SRB2_PS2_LTO=0 build.py --debug`): all SAME except lt_udmany,
which does not fit the memory of MAP11 with the larger debug zone headers ("Not enough memory to draw map MAP11"), nothing else. (The `--debug` build did not compile: `I_Assert(sfx_id < LIMIT_NUMSFX)` in s_sound.c is a signed/unsigned
comparison under -Werror; fixed with casts, two lines.)
