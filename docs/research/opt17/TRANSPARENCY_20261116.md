# Cached mip transparency defect

The frozen v7 renderer can turn index-255 holes into opaque cyan after a texture changes
stored level. The defect is in `tex_upload`'s P8 metadata classification, not in
the palette bytes or the decoded texture bytes. An isolated correction is in
`transparency-canonical-h255.patch`. Production integration is tracked in the
main OPT17 report. Baseline results below describe retained v7 input or an
isolated exact reversal of a reviewed correction; they do not assert that the
current working tree still contains that defect.

The reproduced sequence uses a 256×256 raw flat whose thin holes disappear at
level 1 under the existing decimator. Its first upload streams level 1 from the
raw lump. That path deliberately leaves `ps2_h255` unknown because a mip does not
describe the full source. A later request for level 2 uses the cached level 1 as
its source (`dsrc`). This path incorrectly saves the mip's lack of index 255 as
the full source's permanent `ps2_h255 = 1` classification. A subsequent full
upload trusts it. The keyed twin then selects the opaque CLUT despite the full
image containing holes.

`tex_drop`, `tex_free_all` and `dc_flush` clear driver records, handles and source
cache blocks. They intentionally retain engine `GLMipmap_t` fields. The wrong
classification therefore also survives a real `PS2HWD_SetScreenSize` GS restart.
This explains why the fault can depend on earlier drawing and mode changes.
It does not establish that every observed rendering defect has this cause.

The correction scans a derived cached source for the current upload only.
Only the original full source or its known canonical twin populates
`ps2_h255`. The status domain remains 0 unknown, 1 absent, 2 present, fitting the
existing two-bit field. It changes no texture indices, mip sampling policy,
filter settings, blend equations, physics, or feature switches.

## Executed host proof

`tools/ps2/opt17/transparency_hosttest.py` compiles the complete production texture
manager and the actual `tex_regs` routine. Raw-flat cases use the real
`flat_make_level`, `dc_find_finer`, `dec_level`, `tex_upload`, `tex_view`,
`tex_free_all` and `dc_flush` functions. Map cases additionally exercise the same
cache contract from a prepared packed mip entry; they are not a claim that map
texture generation normally enters through raw-flat streaming.

The fixture runs both variant orders and both direct and restart sequences. It
compares complete uploaded index bytes with the level oracle, reads the CSM1
CLUT packet image, checks TEX0's CLUT address, and compares logical RGBA CRCs.
The current source and frozen v7 source have the same texture-source SHA256.

| Proof | Baseline | Isolated correction |
|---|---:|---:|
| Observations | 41 | 41 |
| Wrong logical RGBA states | 12 | 0 |
| Poisoned canonical-absence observations | 24 | 0 |
| Exact index CRC pairs | 41 | 41 |

Normal, ASan/UBSan, packed PS2 mipmap layout, and packed ASan/UBSan executions
produce identical per-variant results. Each run completes 759 checks.

Current-source report: `build/nov17-transparency-host-v4/report.json`.
Frozen-v7 report: `build/nov17-transparency-v7-host-v1/report.json`.
The final current-source proof freezes the compiled C fixture alongside the
copied driver sources. Earlier exploratory output is superseded.

The primary, map-twin, stride, deletion and source-lock host tools accept both
unfixed and integrated source trees. Each detects its exact reviewed correction
and reverses only that correction for the isolated negative control, leaving
other integrated changes present. Requested headers and private packed-patch
helpers come from the selected source root. Their default source is the current
repository; `--source-root` can instead select an exported revision or frozen
candidate. Use a unique output directory for each run, for example:

```sh
python3 tools/ps2/opt17/transparency_hosttest.py --out build/alpha-regression
python3 tools/ps2/opt17/transparency_map_twin_hosttest.py --out build/alpha-map-regression
python3 tools/ps2/opt17/transparency_stride_hosttest.py --out build/alpha-stride-regression
python3 tools/ps2/opt17/transparency_delete_hosttest.py --out build/alpha-delete-regression
python3 tools/ps2/opt17/transparency_lock_hosttest.py --out build/alpha-lock-regression
```

If a later renderer change deliberately restructures one of those exact source
anchors, the original regression source can be exported portably from its
verified commit; no retained local build directory is required:

```sh
mkdir -p build/alpha-baseline-source
git archive 45f8de1cbf33e8e7b2581afc08cb02499dd8c186 src | tar -x -C build/alpha-baseline-source
python3 tools/ps2/opt17/transparency_hosttest.py --source-root build/alpha-baseline-source --out build/alpha-pinned-proof
```

The other four host tools accept the same `--source-root`. This revision's
texture SHA256 is `b7ebecb38b30ea72a23e37366b0c5e566ed3c99851aa9c11a8215b49673888ab`;
driver SHA256 is `2a54dc7851b62ea371cfe2ecb3731c4d68c56134d55abeadf1660dc57a697a72`.
The native builder also accepts this exported source root and remains build-only.

All five tools were subsequently executed against the integrated source whose
texture SHA256 is `0417241f650fd29a522a7fb985b3b26fbb0aeb7ab8d436d12d821891f8f89959`.
The full current private packed-patch helper branches compile in that proof.
Primary and map-twin tests retain exact index CRCs with zero corrected RGBA
errors. Stride, deletion and real-zone lock negatives still expose their
individual defects when only that correction is reversed. Complete input
hashes, results and scope are in
`TRANSPARENCY_INTEGRATED_RESULTS_20261116.json`. This is a component gate;
whole-game frame/stress acceptance is recorded separately by the coordinator.

An additional map-twin witness creates the cached level through a real keyed
full-source upload instead of preparing a cache entry. The keyed variant knows
the original contains index 255, while the original/plain variant is still
unknown. After the original derives level 2 from the shared level 1, the
baseline poisons only the original's canonical classification. Both full keyed
views, including the view after driver teardown/reinitialization, then have
wrong RGBA. The correction repairs both, with all six uploaded-index CRCs
unchanged. Normal, sanitizer, packed, and packed-sanitizer runs each pass 100
checks. Report: `build/nov17-transparency-map-twin-host-v1/report.json`.

This is a demonstrated source/state sequence with controlled thin-hole input.
The stock census's twelve raw flats of at least 256×256 containing index 255
retain holes at their recursive levels 1 through 5. No individual stock asset
or user's complete failure session is identified by the synthetic witness.

## Actual EE/GS proof

`transparency_native.py` builds paired isolated EE witnesses from frozen v7.
They run the actual renderer, DMA transport, GS CLUT transfer/readback, TEX0
selection, masked polygon draw, and frame readback. Fixture boundary services
provide raw-flat reads and allocations. The C units are materialized through
`-O3 -G8` LTO before the SDK startup link, avoiding the SDK crt0 LTO inline-asm
problem without changing renderer code.

Both ELFs reproduce the streamed-level sequence, draw a constant sample from a
real index-255 texel, and draw an opaque control. The second sequence performs a
real logical 320×200 → 640×400 transition, restarting from internal 320×224 to
640×448. Comparison tolerance is zero.

| GS observation | Baseline | Isolated correction |
|---|---:|---:|
| Canonical classification after derived L2 | 1 absent | 0 unknown |
| Full image keyed view CLUT kind | 1 opaque | 2 keyed |
| Actual GS CLUT entry 255 alpha | 128 | 0 |
| Wrong hole pixels before restart | 7,120 / 7,120 | 0 / 7,120 |
| Wrong hole pixels after restart | 28,640 / 28,640 | 0 / 28,640 |
| Opaque control | exact | exact |

The baseline's hole is cyan `0x00aaff`; the expected background and corrected
hole are `0x332211`. Both opaque controls are exactly `0xd67c29`. The baseline
is an intentional expected-failure observation witness; the strict report
requires its exact wrong pixels and requires all corrected pixels to match.
Both programs otherwise complete 26 boundary/control checks with no failures.

Strict report and source/ELF/log hashes:
`TRANSPARENCY_NATIVE_RESULTS_20261116.json`.
Builds and logs: `build/nov17-transparency-native-v4`.
This is PCSX2 execution, not a physical PS2 measurement or a full-engine scene
acceptance claim.

## Separate cached-row stride defect

The same investigation found that `HWDBG_NOZC`'s band-copy path uses the original
full width as the row stride when `data` holds a packed cached mip. The default
zero-copy path bypasses that row reader. Baseline execution corrupts uploaded
indices, and ASan reports a heap-buffer-overflow in `fill_idx` immediately after
the cached level's allocation.

The separate `transparency-cached-stride.patch` sets source width and height to
the selected stored level when `dce` becomes the upload source. It retains the
debug feature. With canonical classification repaired identically in both
variants to isolate this second defect, the stride correction passes all 41
observations with exact indices/RGBA and no ASan/UBSan error. Normal and sanitized
runs complete 696 checks. Report:
`build/nov17-transparency-stride-host-v2/report.json`.

## Other alpha-state audit

The keyed-view path derives its CLUT choice from the owning image's `has255`,
which propagates the faulty classification exactly as reproduced. CLUT entry
generation itself gives index 255 alpha 0 for keyed/light-keyed/white-keyed
variants and alpha 128 for opaque palette variants. The actual GS readback
confirms these images and TEX0 addresses agree with their selected kinds.

CLUT rebuilds advance `clut_epoch`; overwriting the currently loaded CLUT clears
`clut_loaded`. Fixed-palette refresh clears it as well. Host checks execute the
loaded-slot rewrite and palette-generation changes. VU row and sprite chunks
clear tracked TEX0/CLUT state at chunk completion and key row caches on palette,
light-table generation, keyed status, and CLUT epoch. These read-only checks
found no additional demonstrated invalidation fault in those paths.

AP88 hole/partial-alpha classification and direct-color alpha conversion use
their own scans. They do not read `ps2_h255`. This correction therefore does not
alter the separate sprite-cache prototype or those format decisions.

## Canonical deletion and texture-number reuse

The actual `hw_DeleteTexture` correctly fences queued source references even if
its VRAM image has already been evicted. Deleting an individual keyed view
intentionally retains the live sibling image and its valid source data. Deleting
the final canonical engine object, however, leaves full and derived cache levels
under its `(regen_kind, regen_id)` key. A fresh engine object with the same key
can upload those old pixels instead of its new data.

This has a production call path: `HWR_LoadMapTextures` calls
`HWR_FreeMapTextures`, which deletes the child variant, deletes the original,
and frees old metadata before allocating new same-number metadata.
`HWR_TogglePaletteRendering` calls it specifically because composite blending
can have changed even when the texture palette is unchanged.
`R_FinishLoadingTextures` also calls it when the hardware renderer is active.
The ordinary level and software/hardware transition paths already call
`HWR_ClearAllTextures`, whose driver cache flush prevents this stale-source
case. A live object's `regen_kind/id` being overwritten before deletion is not
the supported identity contract and was not tested.

The isolated actual-function witness first verifies child deletion preserves
its sibling and drains a queued source reference. It then creates both full and
derived cached levels, deletes the canonical object, and uploads a fresh object
with the same key and changed pixels. Baseline retains two old cache entries
and uploads 1,024 wrong indices. The canonical-delete correction retains zero
old entries and uploads every index exactly, selecting keyed CLUT 2. Actual
free callbacks verify no busy source is freed. Normal and packed ASan/UBSan
results agree. Report: `build/nov17-transparency-delete-host-v1/report.json`.
Candidate: `transparency-delete-lifetime-candidate.patch`.

## UpdateTexture API investigation, held separately

`hw_UpdateTexture` also retains canonical `ps2_h255`, source-cache levels, and
other image views despite its changed-texels contract. The actual-function host
probe changes an opaque source to one containing 1,024 holes. Six cases cover
copy and zero-copy uploads, resident shared views, an evicted image with pending
source references, and a missing keyed view with a resident original.

Baseline copy uploads the new indices but renders all holes opaque; baseline
zero-copy additionally uploads old cached indices. Metadata-only and
source-only controls each leave the other defect. The isolated candidate fixes
immediate admission in every case and frees cache sources only with `busy == 0`.
Normal and packed ASan/UBSan results agree. Final expanded report:
`build/nov17-transparency-update-host-v4/report.json`.

This candidate is held apart from the production fixes. Current engine
UpdateTexture callers are patch/model updates with `regen_kind == 0`. No map or
flat caller was established. The reduced SetTexture boundary proves immediate
upload and driver-owned image/DC/metadata invalidation, not the complete engine
CPU-data lifetime: an independent original variant's old `m->data` can survive
and be reused after a later eviction. A complete generic shared-map update
contract must handle that data or its regeneration source before this candidate
is considered complete. Candidate:
`transparency-update-lifetime-candidate.patch`.

## Temporary mip-source locks and reclaimable RAM

When a new cached mip is made, `tex_upload` locks its original source before an
allocation can purge it. Successful conversion then changes `data` to the new
mip pointer. The old unlock condition compares the changed `data` with the old
source pointer and skips its unlock. A zero-copy upload of the new mip also
skips the caller's normal engine-data release, so that caller does not repair
the old source's tag.

`transparency_lock_hosttest.py` executes the real frozen `z_zone.c` and
`ps2_mem.c`, the actual engine lock/unlock/steal/allocate/free/release callbacks,
the complete texture manager, and the literal caller release condition. These
are real zone allocations, tags, owner pointers, and purge operations; only DMA
completion is a host boundary service.

| Source case | Baseline after DMA | Corrected after DMA |
|---|---|---|
| Engine-owned full source → new L1 | tag 48, busy 0, remains after purge | tag 27, busy 0, owner cleared by purge |
| Idle cached L0 → new L1 | tag 48, busy 0, remains after purge | tag 27, busy 0, owner cleared by purge |
| L0 with an older pending DMA reference | tag 48/busy 1 before fence, then 27/0 | same protected lifetime |
| Idle cached L1 → new L2 | tag 48, busy 0, remains after purge | tag 27, busy 0, owner cleared by purge |

Tag 48 is `PU_HWRCACHE`, which the zone cannot purge. Tag 27 is the real LRU
cache tag selected by an actual unlock through `PU_HWRCACHE_UNLOCKED` while
the fixture's cache policy is enabled. Actual `Z_FreeTags` clears eligible
owner pointers. Baseline strands three of four sources; the correction strands
zero. Both new mip blocks and eligible old blocks can be reclaimed.

The minimal correction remembers whether the old source was locked before
`data` changes. After conversion's temporary source-busy increment is removed,
it unlocks only when that old cached source has no queued DMA references.
An intentionally unsafe unconditional-unlock control fails the pending-source
tag assertion. The queued source remains tag 48/busy 1 until the real texture
manager's fence releases it, then becomes tag 27/busy 0 and is purged safely.

All four complete uploaded-image CRCs remain exact, including `94aaa930` for
the busy L0/L1 case and `db0d34a9` for L2. Normal and ASan/UBSan executions
agree. Report: `build/nov17-transparency-lock-host-v2/report.json`.
Patch: `transparency-mip-source-unlock.patch`; exact before/after regions and
hashes: `TRANSPARENCY_LOCK_CANDIDATE_20261116.json`.
The restored amount of reclaimable RAM depends on runtime source-cache
history. Whole-game checks must account for the newly permitted purge behavior.
