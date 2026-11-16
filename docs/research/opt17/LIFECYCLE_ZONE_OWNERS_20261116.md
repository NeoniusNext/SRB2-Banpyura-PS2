# Zone owners across hardware restarts

The final switch-PAL sequence on commit22b5620c stopped at gametic141 with `Z_CheckHeap -1140: block2028 doesn't have a proper user`. The prepared ELF82bc5f26 is rejected. The failure-only diagnostic build preserved the allocator layout and original error while identifying the live block: PU_HWRPLANE,818176bytes, payload0x191db00, user0x82ba18 holding0x1400cc0. Its link map places gc at0x82ba08, making that user exactly&gc.blk. The [source and proof index](LIFECYCLE_ZONE_OWNERS_RESULTS_20261116.json) records complete hashes and historical logs.

Zone's contract is direct: while an allocation has a user pointer, that variable must still point to its payload. HWR_GCacheLevel assumed that level teardown had already freed its PU_HWRPLANE block and assigned gc.blk=NULL. A same-level software-to-hardware return also calls HWR_LoadLevel→HWR_PlaneCacheReset→HWR_GCacheLevel, although that block is still live. The next geometry-cache allocation overwrites&gc.blk with another payload, orphaning the old block. The periodic heap check detects it later; the final resolution change is not necessarily the operation that lost ownership.

The fix detaches the interned batch-state pointers, frees any still-live gc.blk, and then keeps the existing reset, counters and cache-size policy. After a real level teardown, Zone has already cleared gc.blk, so the same reset is safe. Normal cache reclaim and growth already detach state before freeing; their behavior is unchanged. Hardware release drains driver work before same-level reacquisition.

A second ownership bug was found during the audit. Every PS2_PROFILE keyed flat owns a separate CPU pixel allocation, but flat cleanup used the legacy alias rule: it deleted the driver handle, skipped freeing pixels, then freed the child metadata containing the Zone user. The fix frees the independent pixels first in paged/compact metadata and in the supported dense opt-out path. The non-PS2_PROFILE legacy alias path still skips the child pixel free, and wall cleanup is unchanged. Driver source stealing leaves child.data=NULL, which is safe through the corrected cleanup.

The actual geometry-cache reset and real PS2 Zone allocator reproduce the original proper-user error with the old source. Fixed normal and ASan/UBSan runs each pass1495checks over64resets, covering a live old cache, a block already released by Zone, default/user sizing and replacement ownership. The flat fixture executes actual engine creation, regeneration, source stealing and cleanup against real Zone. Each paged, compact and dense sanitized run passes504checks over24cycles and compares12632064source bytes. The old cleanup leaves a live Zone block after actual libc metadata free; a separate deterministic retired-owner reuse case reproduces the exact proper-user error without a host use-after-free. Fixed reuse passes. Driver/raster services are fixture boundaries; these host results are not native switch-sequence evidence.

```sh
python tools/ps2/opt17/gcache_lifetime_hosttest.py --out build/lifecycle/gcache --sanitize
python tools/ps2/opt17/flat_child_lifetime_hosttest.py --out build/lifecycle/flats --sanitize
python tools/ps2/opt17/flat_child_lifetime_hosttest.py --out build/lifecycle/compact --layout compact --sanitize
python tools/ps2/opt17/flat_child_lifetime_hosttest.py --out build/lifecycle/dense --layout dense --sanitize
python tools/ps2/opt17/flat_child_lifetime_hosttest.py --out build/lifecycle/reuse --owner-reuse --sanitize
```

To reproduce the historical negatives, point --source-root at commit22b5620c's source tree and add --expect-orphan; the flat exact-diagnostic case also uses --owner-reuse. The tools mutate no source and start no emulator. Full target hw_cache compilation passes with default and dense opt-out flags, and hw_main including the corrected geometry cache compiles with warnings fatal. No renderer ABI, pixel data, quality policy, tile layout or graphics tolerance changes. The failed native sequences and final broad matrix must pass again before release.
