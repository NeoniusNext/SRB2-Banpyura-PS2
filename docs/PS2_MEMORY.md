# PS2 memory budgeting and large-map limits

## Physical RAM and the usable heap

The port supports **32 MiB retail EE RAM** and **128 MiB kernel-reported development/TOOL/emulator EE RAM**. Detection uses `GetMemorySize()` only. It never reads or writes candidate addresses above retail RAM to discover extra memory. An unrecognized kernel size falls back to the retail budget.

Physical RAM is not the zone arena size. The ELF, BSS, startup allocations, main-thread stack, libc reserve, and allocator metadata all consume that budget. The main-thread stack base reported by `ReferThreadStatus()` is the heap ceiling. If that query is unusable, a conservative ceiling below the live stack pointer is used, with the linker stack size or a 512 KiB fallback. A 128 MiB loader that places its main stack near the retail boundary cannot safely use a 128 MiB contiguous heap through that stack.

At zone startup:

```
growth = max(0, safe_main_stack_base - current_sbrk)
arena  = page_round_down(growth - libc_reserve - 64 KiB)
arena  = min(arena, optional_arena_cap)
```

Existing libc allocations are subtracted. Fragmented free libc chunks are deliberately excluded from this **single contiguous arena** calculation: malloc might grow the break for the entire request if none fits. The additional 64 KiB is allowance for alignment, metadata, and libc heap-growth rounding. Failed arena allocations back off geometrically with bounded retries, allowing recovery from a loader/libc limit substantially below the nominal budget.

| Policy | Libc reserve | Render-start contiguous headroom target | LRU eviction slack |
| --- | ---: | ---: | ---: |
| Retail | 2 MiB | 4 MiB | 256 KiB |
| Development | 8 MiB | 16 MiB | 2 MiB |

These are budgets/targets, not guarantees that a map or frame fits. The reserve covers memory outside the zone, including I/O, audio, video, and driver scratch. `ps2_mem` reports both libc free chunks and contiguous growth, the safe heap ceiling, arena fragmentation, and usage by tag. `I_GetFreeMem()` includes unused zone bytes without double-counting the arena as free libc storage. Neither total free count guarantees a contiguous allocation.

`-zram 32` selects the smaller policy for development tests. `-zram 128` cannot promote a retail machine to extra physical RAM. `-zreserve`, `-zarena`, and `-zheadroom` accept nonnegative KiB values within the EE address range; negative, overflowing, or malformed values are refused. A cap only decreases the arena. A zero headroom disables render-start pre-eviction; it does not disable exhaustion checks.

## Allocation peaks and recovery

The segregated arena uses 16-byte release headers (32-byte `ZDEBUG` headers), boundary-tag coalescing, and 16-byte minimum payload alignment. Requests of at least 2 KiB have at least 64-byte payload alignment.

`Z_ReallocAlign()` now tries an in-place resize when the current address satisfies the new alignment:

* Shrinking releases the tail immediately and coalesces it with following free space.
* Growing consumes following free space without allocating a duplicate payload.
* Payload bytes, owner retargeting, tags/frame stamps, zero-filled extensions, and debug guards retain their semantics.
* Moving remains necessary when alignment changes or the following space is occupied. The source is pinned during purge/retry and copied before release. Thus the old-plus-new peak is reduced, **not universally eliminated**.

The normal allocator purges unlocked tags when outside the render lock, then evicts eligible owner-backed caches by exact frame LRU and retries. Retry byte targets saturate rather than wrapping. Required level data and batching geometry are not evicted.

`Z_TryMallocAlign()` is the PS2 recoverable allocation entry point. It performs the same cache retries, returns NULL on exhaustion, and leaves the supplied owner unchanged on failure. Other eligible caches may already have been evicted. Invalid sizes/tags/alignment or ownerless purgeable requests remain programming errors. Callers must implement their own fallback/cleanup before choosing this entry point; it does not make partially initialized global map state transactional.

Required allocations still use the diagnostic OOM path when no legal victim/contiguous block remains. The engine currently has no general mid-load rollback that safely returns arbitrary failed map loads to the menu.

## Large-map loading

For cooked packs, an embedded map WAD is read as a header, directory, then individual sublumps through the existing partial-read path. This removes the previous peak of the **whole embedded WAD plus all copied sublumps**. Each destination remains an independently aligned zone allocation, even when its WAD offset is unaligned. Directory/count/range checks precede the corresponding reads and allocations. Empty sublumps remain supported. Indexed LZ4 reads use the pack reader's bounded block buffers; they do not require decoding the entire embedded WAD into RAM.

The integration test loads a 900,000-byte embedded resource containing 450,000 bytes of actual sublump payload in a 600,000-byte zone. Its zone peak is below 451,000 bytes. This is a synthetic peak regression check, not a promise about an actual map's expanded engine structures.

Blockmap construction now allocates its grid through the PS2 zone, so a large grid is not confined to the small libc reserve. Required temporary block lists carry `PU_LEVEL`, rather than being treated as reconstructible render caches. Grid/list/output size arithmetic is checked before allocation. The existing blockmap geometry and list ordering are preserved. Its temporary lists and final blockmap still overlap during construction; very large or dense grids remain expensive.

For binary maps with at most 65,535 linedefs, the final profile blockmap keeps
32-bit cell offsets and compacts only its linedef lists to unsigned 16-bit
values (`65535` represents the original `-1`). The header zero and linedef zero
remain distinct and the iterator still skips exactly the first header entry.
Compaction runs in the existing root and shrinks it in place after construction,
so it introduces no second blockmap allocation. Larger maps, invalid indices,
and unusual legacy offsets that point into the offset table retain the original
32-bit representation. This reduces resident storage; it does not remove the
original builder's construction peak. `PS2_NOOPT_BLOCKMAP16` selects the old
representation for comparison.

## Hardware batching RAM and CPU work

On the EE, batch buffers are accounted under `PU_HWRBATCH`, allocated from the zone with alignment and purge/retry support. This is a persistent, non-purgeable CPU-geometry tag, separate from texture cache tags. Collection arrays grow by 50% on the PS2 profile instead of doubling, using resize rather than explicit allocate/copy/free loops.

The EE submits triangle indices directly into the collected vertex array. It no longer allocates a second final vertex array or copies vertices into that array for each batch. The initial eliminated array is **8192 × 20 = 163,840 bytes**, with larger savings on frames that formerly grew it. This also removes the corresponding CPU memcpy work. Sorting still uses the existing qsort for equal-key cases; it is bypassed only for strictly ordered keys. FNV hashing uses defined unsigned wraparound and the comparator avoids signed subtraction overflow.

Output index scratch is bounded to the initial 8192-vertex chunk except when a **single polygon** requires more. A full chunk is submitted before the next polygon with the same state and triangle order. No polygons, texture resolution, colors, lighting, or geometry are dropped. Collection storage still scales with **all polygons/vertices queued before sorting**; batching is not an unlimited-memory streaming renderer.

The synchronous host recorder compares the reference and EE paths across empty frames, 6000-polygon frames, homogeneous/mixed states, shaders on/off, untextured/horizon polygons, chunk boundaries, and a 9000-vertex fan. Both produce the same **218,886 triangles and exact vertex/state stream digest**. This verifies CPU submission semantics, not physical GS output or an EE FPS gain.

## Cross-component requirements

* Renderer/cache consumers must fetch/touch the **allocation root** before holding root or interior pointers. Current-frame roots are protected; untouched earlier-frame caches may be evicted even inside a nested `Z_PurgeLock`. The outermost lock's initial headroom preparation can evict current-frame caches because it assumes no view pointers are held yet.
* `Z_ReleaseCache(root)` is an explicit lifetime boundary for a synchronous
  consumer that has finished using **every** alias. It makes a current-frame,
  owner-backed `PU_CACHE` root eligible for pressure eviction without freeing
  it immediately. Fetching/touching it again restores current-frame protection;
  eviction clears the owner and requires rebuilding all aliases. Older cache
  stamps, required data, sprites and a temporarily pinned realloc source are
  unaffected. `R_ReleaseTextureCache()` provides this boundary after completed
  opaque wall drawing; deferred masked walls keep texture IDs and fetch again.
  Calling it while any column/post/pixel pointer is still in use is invalid.
* A cached root needs an owner slot that remains valid until it is freed or retargeted. After that slot becomes NULL, every column/post/pixel alias must be discarded and rebuilt. `PU_SPRITE` eviction remains restricted by the existing self-contained `Patch_IsEvictable()` contract.
* Do not make hardware texture/model caches newly evictable without a driver reload/owner contract. No additional hardware cache tags were added to LRU eligibility by this work.
* The EE indexed-triangle backend must consume/copy the referenced vertices and indices before returning. It must accept indices into the collected vertex array, not assume vertex indices are bounded by the draw's index count. The current EE backend consumes indexed triangles synchronously. An asynchronous backend would need explicit buffer-lifetime fences.
* Video/audio/pack/driver raw libc allocations must honor their own physical limits and the shared reserve. The zone cannot recover that separate reserve by evicting a zone cache, because its arena is already a libc allocation. Use zone allocations for suitable CPU buffers, tune the reserve to measured subsystem peaks, or supply recoverable subsystem-specific allocation paths.
* Extended RAM must be reported and mapped by the kernel/loader, with a stack layout and libc heap behavior that permit it. A command-line flag cannot provide that mapping.

## Actual limits

There is no lossless allocator optimization that makes arbitrary maps fit 32 MiB. The mandatory expanded vertices, sectors, linedefs, BSP, blockmap, thinkers, Lua/game data, renderer working set, and audio/video buffers must fit the physical budget **at the same time**, including transient peaks and fragmentation. A large allocation can fail even when total free bytes exceed its size. Non-cache live data is never silently discarded to claim support for a larger map.

The 128 MiB policy provides a larger usable arena only when the real heap/stack layout permits it. It does not remove EE integer/format limits, blockmap growth, collected-batch growth, GS VRAM limits, or CPU cost. These changes do not certify every stock/custom map on hardware or guarantee an FPS target. Actual worst-case map acceptance requires per-map EE high-water measurements with the integrated renderer/video/audio configuration.

## Reproducible checks

Use a separate output tree and verify its parent exists before running commands that generate files:

```powershell
Test-Path -LiteralPath "build"
python tools/ps2/zone_hosttest.py --out build/ps2-memory-check/host --x64 --identical --negative-controls
python tools/ps2/memory_hosttest.py --out build/ps2-memory-check/integration

$env:SRB2_PS2_OUT = 'build/ps2-memory-check/ee-release'
$env:SRB2_PS2_RELEASE = '1'
$env:SRB2_PS2_HW = '1'
python tools/ps2/build.py --syntax src/ps2/ps2_mem.c src/ps2/i_system.c src/z_zone.c src/hardware/hw_batching.c src/w_wad.c src/p_setup.c
# Repeat with SRB2_PS2_RELEASE=0 and a separate ee-debug output directory.
```

The zone suite covers x86 release/debug headers, randomized independent-model torture, owner/alias lifetimes, long-age LRU, nested render locks, fragmentation traces, budget overflow, actual-stack ceilings, geometric arena backoff, in-place resize peaks/guards, and recoverable exhaustion. Its native host allocator remains preprocessor-identical to the pinned vanilla code in all four checked configurations. Negative controls must fail an assertion/diagnostic, not crash.

## Out of memory is recoverable (OPT11-STAB, PS2-170..173)

`Z_Malloc` that finds no room (after every cache eviction and reclaim hook) no longer has to end the program. Code that can be abandoned half way arms a guard
(`zguard_t`, `Z_GUARD_TRY`, `src/z_zone.h`): the allocator then `longjmp`s back to it instead of `I_Error`. The guards (`src/ps2/ps2_hwfb.c`):

* **the frame** (`PS2HWFB_Display` around `D_Display`): in the hardware renderer the half drawn frame is dropped (`PS2HWD_Abort`: DMA channels stopped, `GIF_CTRL.RST`, VIF1
  reset, ring/frame state cleared), the ordinary renderer switch of Options -> Video runs (`SCR_SetMode`) and the game goes on in software for the rest of the level;
  the map is remembered as "does not fit in hardware"; at the next level the hardware renderer is tried again (unless the map is on the list, or it failed 6 times).
  In software the caches are emptied (`Z_EmergencyFree`, the free space joins into big blocks again) and the frame is drawn again; the third failure within 12 frames is the
  usual out-of-memory error. The resource failures of the GS driver (`hw_failure`) take the same way instead of `I_Error`.
* **the hardware part of a level load** (`PS2HWFB_BuildLevel`, replaces the direct `HWR_LoadLevel`), plus two gates that avoid the failure: a map with >= 14 000 subsectors
  (MAP11: 15 942) goes to software before anything is loaded, and a level that leaves < 2.5 MB of arena after its plane polygons were built does too.
* **the level load** (`PS2HWFB_LoadLevel` around `P_LoadLevel`, local games only): a map that does not fit goes back to the title screen with a message.
* Not taken: while a Lua hook runs (`PS2Lua_InCall`), and in a network game for any allocation that is not a hardware-renderer tag (`NetUpdate` runs inside the frame).
* Rule for code that allocates: `Z_Free(x); x = Z_Malloc(...)` leaves `x` dangling when the allocation jumps out; allocate first, or clear `x` before. Found with
  `tools/ps2/oom_inject.py` (`-zoomnth N`: the N-th allocation under a guard fails): `LoadPalette`, `R_ReInitColormaps`.
* Test switches: `-zoomtest F1,F2,..` (a hardware-tag allocation fails at frame F), `-zoomevery N`, `-zoomany` (any tag), `-zoomnth N -zoomafter F`, `-hwfbtest F` (a throw out of
  frame F), `-hwnomem` (every other start of the GS driver finds no memory), `ps2_finale N` (console: ending, credits, evaluation, continue, game end, intro).

The hardware driver's big work arrays (`ovq`, `cutbuf`, `plan_info`, `blk_owner`, 230 KB), the link-draw list (172 KB) are zone blocks that exist while the hardware renderer
runs (`.bss` -500 KB, arena +491 KB for a software game); `R_ReleaseDrawSegScales` gives the scale arrays of the FOF drawsegs (up to 874 KB) back when a level starts.
