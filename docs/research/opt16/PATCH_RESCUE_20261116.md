# Try-only AP88 patch rescue — 2026-11-16

The deferred PS2 renderer can select a patch, lose its purgable AP88 upload
source under pressure, and then need to upload it later in the same frame.
The driver already calls `HWR_PS2_RegenerateMipmap` at that point, but kind-zero
patches previously had no regeneration path. The remaining full-size-mode
integration run showed two `no data (purged)` patch skips at frame 129 even
after the independent VU constant-slot fix restored all three sampled frames.

The new path rebuilds ordinary AP88 patch images from their existing converted
patch columns. It adds no per-texture metadata, source-cache copy, lump-ID
restriction or rendering limit. The final full `hw_cache.c` SHA256 is
`2ff513529707134b55d2fc6e048ecbe9177d77c96e9e4cec22da5bc69fe9e542`.
The 68-byte mipmap header remains unchanged at
`ae19d9a3ce26bda9a11acb9fe52906712edb5b711b759cf9d070e63077c190a7`.

## Lifetime and reconstruction

The source is the live `patch_t`, which already contains the converted pixels,
posts and columns. This works for decoded PNGs, generated/rotated patches and
mapped mipmaps without recovering a lump number, rotation recipe or filename.
`Patch_IsEvictable` rejects patches with hardware metadata, and
`Patch_AllocateHardwarePatch` converts an evictable `PU_CACHE` patch to the
existing pinned low-priority patch tag. Source columns/posts outside an embedded
patch are existing non-purgable `PU_PATCH_DATA` allocations.

A read-only callback finds the target in a source patch's primary/colormap
chain. It visits exactly the two ranges already used by `HWR_FreePatchCache`:
`PU_PATCH..PU_PATCH_ROTATED` and `PU_SPRITE..PU_HUDGFX`. These must remain
separate: the intervening tag 17 contains raw patch data rather than patch
structures. The callback never allocates, frees, retags, uploads or changes
patches. Lookup context is cleared before allocation begins. The iterator API
has no userdata parameter; the two private context pointers occupy eight
bytes on the actual EE, with no per-patch or per-mipmap field.

After lookup, a successful `MakeBlockEx(m, true)` creates the same transparent
AP88 background as normal patch loading. The existing `HWR_DrawPatchInCache`
then draws the same source columns with the target mipmap's copied colormap.
No loader/SetTexture recursion occurs. A fully transparent source may have NULL
pixel/post pointers, so eligibility requires real source columns without
requiring pixel/post allocations. Models, raw-flat placeholders and fade masks
have different recipes and remain on their existing paths.

The new block remains `PU_HWRCACHE` while the driver consumes it. The existing
driver release path then restores its normal cache tag. Existing patch and
hardware teardown continues to own source, colormap, mipmap and image storage.

## Failure behavior

Normal `HWR_MakePatch`, its allocation threshold and all existing map/flat
regeneration branches are unchanged. Rescue always uses the try allocator,
including images below the normal 64 KiB threshold. A failed attempt leaves
`m->data` NULL and returns to the driver's existing missing-texture handling;
there is no new fatal error or guard throw.

The try allocator still rejects requests above `0x7FFFFFF0` with a fatal
parameter error. Rescue checks the AP88 byte bound before lookup/allocation.
The same bound proves the existing allocator helper's signed width-times-height
product representable. Zero dimensions, absent sources and source extents larger
than the existing mipmap also return without an allocation. The routine changes
only the image owner pointer and bytes; handle, format, flags, dimensions,
colormap and cold metadata remain intact.

`PS2_NOOPT_PATCH_RESCUE` restores the original kind-zero path. The public
regeneration body preprocesses exactly to the pre-rescue page implementation
with that opt-out and on
the non-PS2/profile path. Lookup costs an arena walk on a missing AP88 source;
resident textures and already-present image data take their existing early
returns. No frame-rate improvement is claimed for this recovery path.

## Executed component proof

[`patch_rescue_hosttest.py`](../../../tools/ps2/patch_rescue_hosttest.py) and its
[fixture](../../../tools/ps2/patch_rescue_hosttest.c) retain complete actual
production types, normal patch creation, background fill, both patch drawers,
source lookup/rescue, patch conversion, rotation, hardware teardown and patch
free functions. The normal pre-purge image is copied as the reference; the
complete reconstructed image is compared directly after source purge and again
after a second purge. Normal allocation/drawing bodies are checked literally
against baseline `604cfd150` before compilation.

The final report is
`build/nov16-hwopt/patch-rescue-host-v9/report.json`. With ASan/UBSan, each active
layout passes 595 cases and compares 72854768 complete AP88 bytes. This includes:

- Every 15 vanilla PNGs, freshly decoded by the original engine/libpng oracle
  into canonical patch pixel matrices, then converted into patches by the actual
  `Picture_PatchConvert` code. The PNG decoder is unchanged by this work.
- Actual `RotatedPatch_DoRotation` over all 71 nonzero angle indices and both
  flips, plus generated sparse/tall/NPOT patch columns.
- All five canonical source tags, primary and second-child mapped mipmaps,
  complete 256-byte translation tables, index-255 cases, both existing draw
  paths, and a fully transparent source with NULL pixel/post pointers.
- Owner invalidation during purge, a live hardware source protected by the
  actual `Patch_IsEvictable` rule, repeated regeneration, and actual
  `HWR_FreeTexture`/`Patch_Free` teardown with real backing frees.
- Forced try failure with the fixture guard both armed and unarmed, no mandatory
  allocation route, no fatal/guard calls, oversized/zero/no-parent cases, and
  exclusion of model/fade/raw-flat placeholders.

The 68-byte and baseline-storage layout builds produce the same reference hash
and rescued bytes. The rescue opt-out preserves missing-data behavior. Six
source corruptions are detected: mandatory allocation, discarded colormap,
merged tag ranges, allocation inside iteration, omitted size guard and rejected
mapped-parent lookup. Generated source, executed tools, input PNGs/matrices,
logs and hashes are preserved under the output directory. Engine/zone/driver
services remain explicitly checked host boundaries; these tests do not measure
shipping allocator fragmentation or GS timing.

The actual EE default and rescue-opt-out cache sources pass normal build syntax
checks. An additional actual-source EE object probe asserts the eight-byte
context and unchanged 68-byte mipmap. Its standalone non-LTO `-O2` callback has
no stack frame; public regeneration has a 48-byte frame. These are diagnostic
object observations, distinct from the coordinator's final shipping LTO image.

The independent final-storage regression at
`build/nov16-hwopt/pages-after-rescue/report.json` passes 851 cases and five
controls, including actual target header sizes. Focused page/layout fixtures
explicitly disable rescue and reference this separate rescue proof.

No emulator was used for this component. The coordinator's complete game image,
skip, cache-history and memory gates determine final integration acceptance;
the pre-existing selected-while-collecting skip signals remain separately
reported and are not relabeled by this fix.

```sh
python3 tools/ps2/patch_rescue_hosttest.py \
  --assets /path/to/srb2-assets \
  --out build/patch-rescue-replay --negative-controls
```

The host PNG oracle uses the onboarding host libpng installation and the user's
normal asset archive. It does not download assets or run the game. Earlier
development outputs remain under `patch-rescue-host-v1` through `v8`; the final
report above retains the executed cleanup functions and complete source hashes.
