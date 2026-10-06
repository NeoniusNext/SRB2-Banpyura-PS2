# Retail memory continuation — 2026-10-03

The existing dirty tree was preserved. This work changes `r_textures.c/.h`,
`p_setup.c/.h`, `p_maputl.c`, `z_zone.c/.h`, `ps2/ps2_mem.c`, and the individual
memory/zone fixtures. The shared deviation registry is left to the coordinator.

## Changes and measurements

- The final blockmap uses 32-bit cell offsets and 16-bit linedef lists when
  `numlines <= 65535`. It compacts and shrinks its existing root, preserving all
  list entries, duplicate entries, order, empty lists, header zero and linedef
  zero. Linedef 65535, larger maps, unusual legacy offsets and invalid indices
  retain the complete 32-bit form. `PS2_NOOPT_BLOCKMAP16` disables compaction.
  The 54 actual generated-map fixtures compare every normalized list entry and
  the actual iterator's ordered callbacks, including early termination. Dense
  fixture resident allocation falls **101120 → 52368 bytes**, horizontal
  fixture **230032 → 121616 bytes**, sparse fixture **208864 → 204352 bytes**.
- Composite texture descriptors are counted first and written directly into
  the final root when retaining the opacity mask is cheaper than descriptor
  scratch. Otherwise one exact-size scratch allocation is used. The fragmented
  65×129 fixture (4096 posts) reduces peak **116208 → 68176 bytes**; with a
  forced pixel-root move **117248 → 69216 bytes**, a **48032-byte** reduction
  in either case. Descriptor realloc calls fall **4096 → 0**. Opaque fixture
  peaks are unchanged (**19152 bytes**, **20512 bytes** with forced move),
  while its descriptor realloc calls fall **65 → 0**. `PS2_NOOPT_TEXPOSTS`
  selects the previous path. The fragmented fixture now fits an **80000-byte**
  arena; the preceding implementation produces a diagnosed OOM in that arena.
- `Z_ReleaseCache(root)` exposes the end of a synchronous cache consumer's
  lifetime. It never frees eagerly; only current, owner-backed `PU_CACHE` roots
  are demoted to age one. Refetch protects them again. Required data, sprite
  roots, previously old roots, and a realloc-pinned root retain their semantics.
  `R_ReleaseTextureCache()` provides the wall-texture boundary; renderer call
  sites are implemented and verified by the renderer agent.
- Opt-in memory diagnostics now use `I_OutputMsg`, preserving log text without
  adding memory reports to the game's console HUD. `-zcaller` traces requests
  of at least 65536 bytes, including the previously unexplained 69896-byte OOM.

## Reproduction

```
python tools/ps2/memory_blockmap_hosttest.py --out build/ps2-next-memory/blockmap --compact --negative-controls
python tools/ps2/memory_texture_hosttest.py --out build/ps2-next-memory/texture --negative-controls
python tools/ps2/texture_hosttest.py --out build/ps2-next-memory/exact-texture --negative-controls
python tools/ps2/zone_hosttest.py --out build/ps2-next-memory/zone-final --only x86,zdebug,native --identical --negative-controls
```

EE release and debug syntax checks of the modified production sources pass with
zero warnings. Independent x86/x64 texture snapshots compare 16 fixtures × 10
copy/flip/clip/blend cases per architecture with **zero differing bytes**.
Negative controls corrupt a post topdelta and linedef zero: the respective
exact comparisons reject both mutations. The source-level host allocator
equivalence check and integrated cache-release tests are recorded in the
`zone-final` and `texture` output directories.

## Scope and remaining limits

The earlier MAP11 failures were real first-frame OOMs, not successful emulator
exits. Level structures had loaded, then the simultaneous current-frame texture
and renderer working sets exhausted the retail arena. Compaction reduces
resident mandatory data, but does not remove its original construction peak.
Counted posts reduce temporary peaks, but cannot make arbitrary textures fit.
Release is safe only after all aliases have expired; it is never a blanket
permission to evict current-frame caches. Reloading evicted textures may increase
CPU work under pressure, so integrated COP0/FPS and golden comparisons are
required. The coordinator performs the final integrated retail MAP11 run and
records its exact ELF and memory/performance result; these host figures alone
do not certify a hardware map or an indefinite soak.
