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

### PS2-LUA-3: the order in which C evaluates the arguments of a call (FixedMul, FixedHypot): the error names another argument

* Cause: `lib_fixedmul` and `lib_fixedhypot` read both arguments inside one call (`FixedMul(luaL_checkfixed(L, 1), luaL_checkfixed(L, 2))`). C does not say which is evaluated first: x86 GCC and MSVC (the PC builds) take the
  last one first, the EE GCC the first one. With two bad arguments the PC says "bad argument #2 to '?'", the PS2 "#1". (The same class of problem was found for the RNG by OPT10-S, PS2-142, which scanned the engine but
  not the Lua libraries; a scan of `lua_*.c` / `deh_lua.c` for statements with two `luaL_check*/luaL_opt*` calls finds exactly these two (plus `FixedRem` and `GETSECSPECIAL`, whose operands/macro are evaluated left to right on both).)
* Test: `tools/ps2/luatests/lt_argorder.lua` (every function with two numeric arguments with every combination of bad ones): 2 of 20 lines differed.
* Fix (`src/lua_mathlib.c`, `#ifdef PS2`): the arguments are read into locals right to left. After the fix `lt_argorder`: SAME.
* Operators of the integer VM over a grid of 39 x 39 edge operands (shifts beyond 31 and negative, INT32 limits, pow, %, /, comparisons, concatenation, `string.format("%d")`): `lt_ops.lua` SAME. (The only differences are the
  two known platform properties of `lt_plat`: the literal `2147483648` and `%x` of a negative number.)

### The C stack of the EE under Lua (not a defect found; the margin measured)

`tools/ps2/lua_stack.sh` runs the worst recursion a script can start (200 nested `string.gsub` callbacks; `table.sort` comparators that sort; `string.format` inside gsub; Lua stops at LUAI_MAXCCALLS with "C stack overflow")
from eleven different hooks and prints the deepest the 384 KiB main stack went (`-zstack`, `stackused=` of ZSTAT):

| started from | stack used (of 393 216 B) |
|---|---:|
| MobjThinker / PlayerThink / PreThinkFrame | 339 056 / 339 088 / 339 024 |
| HUD hook (game) | 341 632 |
| MapLoad hook / MapChange hook | 340 944 / 339 184 |
| PlayerMsg (chat) / NetVars / AddonLoaded | 338 976 / 339 024 / 339 232 |
| engine <-> Lua recursion (P_KillMobj <-> MobjDeath, P_SpawnMobj <-> MobjSpawn, P_RemoveMobj <-> MobjRemoved, 197-198 levels) | 228 696 |

The worst case uses 86-87 % of the stack (about 1.5 KB of C stack per level: the 1 KiB `luaL_Buffer` of `gsub` plus frames); the engine alone peaks at 133 KB (PS2-77). The margin is 52 KB: no overflow, no change.
The messages and depths (198 / 197 / 199 levels) are the PC's (`lt_stack`, `lt_stack2`, `lt_stack3` SAME). A different context (a debug build, a deeper engine call chain) could reach the end of the stack, which on the EE corrupts memory silently:
if a user script recurses without bound through `gsub`/`sort` callbacks this is the place where the PS2 differs from the PC (8 MB stack).

### PS2-LUA-4: the Lua slab pool kept the memory of freed objects for ever (the same size only); a heavy mod ate 2.7 MB more than it needed

* Cause: PS2-LOAD-18 made the heap of the main Lua state a slab allocator (16 KB slabs carved in 16-byte size classes, a free list per class, no headers). A freed block can only serve the next
  allocation of its own class and a slab was never returned, so garbage of one size followed by garbage of another size left the first in the free lists for the rest of the run.
* Measured with `tools/ps2/luatests/lt_heap.lua` on BIG.pk3 (100 scripts, 3.6 MB of live objects; `memfree` now prints "Lua heap" and "Lua pool"): after rounds of 15 000 small strings, 6 000 medium
  strings, 1 500 large strings and 12 000 tables, each dropped and collected, Lua counts 3.64 MB live and the zone holds 6.65 MB for the heap (2.8 MB on free lists), against 3.97 MB after the fix.
* Fix (`src/lua_script.c`): a directory of the slabs (sorted by address, 12 KB, made with the state), the bytes of each slab that sit on the free lists are added up, slabs that are free in their whole
  used extent go back to the zone (`LUA_PoolTrim`; never the slab being carved). It runs after the full collection at the end of loading, after `collectgarbage()` of a script, every ~10 s from
  `LUA_Step` when 256 KB or more sit on the lists, and as a reclaim hook of the zone (`Z_AddReclaimHook`, "the hook only frees") when an allocation would fail. Nothing is allocated by the trim.
* Checks: `lt_poolstress.lua` (40 rounds of 1 500 objects of every kind, survivors verified every round, then 30 rounds with 4 000 objects and ~10 survivors; 481 slabs given back, 0 wrong checksums) SAME as the PC;
  `lt_heap` 601 lines SAME; the whole suite below.

### PS2-LUA-5: a call from Lua that would run past the end of the EE stack is refused (hardening, not triggered by any test)

`luaD_call` (ldo.c, PS2_PROFILE) compares `$sp` with the bottom of the main thread stack + 40 KiB and raises the same error as the 200th C level ("C stack overflow"). The worst recursion measured
(200 levels of gsub callbacks, section above) stops 52 KB from the end, so the check never fires in any test of the stand (the levels reached are the PC's: 198/197/198); with `-luastackmargin 300`
(a margin that is reached) gsub stops at level 50 with the same message and the stack stays at 132 KB. The EE has no stack guard; without the check an overflow corrupts the heap silently.

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
