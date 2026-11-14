# Hardware texture metadata — ninth pass, 2026-11-15

The reference is the completed eighth pass, source revision
`19cf5df2a82b04e37fabe76a4441f3d5062ebaf1`, frozen in
`build/nov15-next9/baseline-src`. The only memory production edit is the
PS2 member order of `GLMipmap_t` in `src/hardware/hw_data.h` (SHA256
`bd601243f88c6c0faa8cb5f3fd4857de7b70c1d85832d2d15963439b96f4d9a3`).

## Full-width representation

Eight byte fields and three `UINT16` fields now share the alignment padding
between full-width pointers and `UINT32` fields. No field is narrowed,
removed, shared, reconstructed or assigned a different lifetime. The generic
leading fields, including the pixel data pointer used as a zone owner, retain
all offsets. All scalar and pointer accesses remain naturally aligned.

Actual default-release EE GCC/header results:

| Property | Before | After |
| --- | ---: | ---: |
| `sizeof(GLMipmap_t)` | 88 B | 76 B |
| `sizeof(GLMapTexture_t)` | 96 B | 84 B |
| Natural alignment, either type | 4 B | 4 B |
| Generic leading `data` offset | 4 B | 4 B |
| Full pointer width | 4 B | 4 B |

`GLMapTexture_t` embeds the same complete mipmap plus its original two
full-width scale floats. Each dynamically allocated mipmap requests **12 B
less payload**. The two map/flat tables, each containing `N` map-texture
records, together request **24 × N B less payload**. These formulas describe
the existing objects, before allocator rounding and placement; they do not
count texture pixels or driver texture-cache records.

The ordinary standalone mipmap's minimum EE zone block changes from
`align16(16 + 88) = 112 B` to `align16(16 + 76) = 96 B`. Extra alignment gaps
and absorbed tails are separate. Three static records across the isolated
consumer objects shrink by a total **36 B** (two are in the driver and one
is in batching). These component values overlap their corresponding game
allocations and must not be added to integrated zone checkpoints.

The native 64-bit host has 112→104-byte mipmaps and 120→112-byte map-texture
records with eight-byte alignment. Host byte savings are not EE measurements.
`PS2_NOOPT_MIPMAP_LAYOUT` restores the preceding PS2 layout. The preprocessed
non-PS2 header interface is byte-identical.

## Allocation and consumer audit

Every constructor retains its preceding allocation point, count, request
expression using `sizeof`, tag, owner, publish order, free order and growth
policy. `HWR_LoadMapTextures` frees the preceding tables before making its
two replacement calloc roots. Patch metadata, chroma variants and translated
colormap lists still use their original constructors and ownership paths.
There is no new allocation, final shrink, moving resize, deferred reserve,
changed failure path or reduced limit.

The source audit finds no positional initializer, fixed byte offset,
serialized mipmap object, raw-object comparison or separately loaded binary
consumer. Temporary checks use complete typed copies and `sizeof` clears.
The GS driver reads typed C members; metadata is not a DMA source. Pixel
allocation, pixel alignment, texture sizes, format selectors, all frame and
view counters, plan indices, flags, pointers and saturating diagnostic bytes
retain their full preceding ranges. Software has no production change.

The table calloc roots use the unchanged real `ps2_spill.c` policy. A
main-thread request of at least **24 KiB** first tries the zone; smaller
requests first try libc and may spill if libc fails. Smaller records can
cross this threshold, changing which allocator holds a table. This is why
the proof exercises both branches and records zone use separately from libc
requested payload. Its libc ledger is a controlled service boundary, not a
measurement of newlib chunk rounding or whole-process peak RAM. Smaller
metadata can also change cache placement and residency; no universal total
or peak RAM reduction, fragmentation gain, frame-time improvement or FPS
claim is made.

## Host proof and isolated objects

`next9_memory_hosttest.{c,py}` extracts the actual metadata/patch types,
map-table constructor and teardown, PS2 map/flat/chroma selection,
regeneration, patch and mapped-colormap lifecycle, owner-steal operation,
selected actual driver plan/key helpers, real spill wrappers and real
`z_zone.c`/`ps2_mem.c`. GPU upload/deletion, resource conversion and libc
services are explicit controlled boundaries.

Frozen/current/opt-out and ASan/UBSan each pass **186 cases**. The complete
**29,351,712-byte** normalized state output agrees literally, SHA256
`b8de7a82421ef958d21753e120e8688906b079baa6ad8c5e2ef6377405c47136`.
Baseline and opt-out pass **90,734,851 checks** each; compact and sanitizer
pass **86,299,181**. The difference reflects shorter initialized objects and
different physical heap walks, not fewer logical cases.

The cases cover:

- Thirteen selected array counts from 1 through 65,535, with every element
  checked, all scalar bits, full pointer identities, hostile initial bytes,
  typed assignment and complete array copies, neighboring canaries, both
  arena directions and red zones. Each pure-table request, retained use and
  peak is nonincreasing; opt-out reproduces every preceding allocator row.
- Fourteen selected map-root counts, including values immediately around
  both layouts' real EE and native 24 KiB spill thresholds and 65,535 entries
  per root.
  Both libc-success and forced-spill service boundaries execute the actual
  spill code. Actual table addresses, allocator membership, static-root
  owners and payload/accounting values remain in the raw logs.
- Original/chroma map and flat pointers, cache hits, full regeneration IDs,
  literal regenerated bytes, unchanged map scales, actual owner transfer to
  the driver, owner clearing on release and rebuild into the same metadata.
  Actual nonempty table reload increases the count before final teardown.
- Patch/mipmap zone ownership, default and two translated colormaps, list
  ordering, source-pointer identity, all 256 copied bytes, cache hits,
  mutation/update of an existing colormap, release and rebuild.
- Actual driver plan eligibility, original/variant aliases, plan capacity
  fallback, high-bit frame IDs, counter wrap, visibility, full-size retention
  and immediate-level policy. Both map-root allocation failures and all
  three variant allocation failures retain their errors and unpublished
  roots, then permit successful retry and complete cleanup.

Five compiled and executed mutations are detected: truncating a diagnostic
byte, corrupting a driver frame ID, dropping a chroma twin pointer, omitting
zone owner transfer and shortening an array copy. Padding and process
addresses are excluded from literal output. Pixel cache presence is
serialized; every live pixel pointer and its exact owner transition is
asserted separately, so address reuse after a free is not mistaken for
logical state.

`next9_memory_objectcheck.py` compiles eight complete actual EE consumer
units, including the GS driver, against the frozen source with only the live
metadata header overlaid. This isolates the layout from simultaneous
independent render-kernel changes. Every allocated object section of the
opt-out equals the frozen baseline exactly. Isolated non-LTO text shrinks
**56 B**, static BSS shrinks **36 B**, and rodata/writable data remain equal.
Only three isolated frames change: `HWR_GenerateTexture` 592→576 B,
`HWR_PS2_ComposeForCheck` 240→224 B and `HWR_MakePatch` 144→128 B. These are
component compilation results, not final-LTO totals or whole-game stack
high-water measurements.

## Actual EE32 runtime

The same complete fixture runs on EE32 for the frozen/current/opt-out
layouts. All three pass **186 cases** and match all **29,351,712 literal
bytes** of the frozen host oracle. Baseline and opt-out pass **77,428,289
checks** each; compact passes **70,774,783**. Actual frame/view wrap, variant
failure/retry, libc-success and forced-spill paths, zone ownership, cleanup
and static-backing canaries pass. Opt-out reproduces every baseline state
and allocator row exactly.

Every measured pure-table request, retained zone use and peak is
nonincreasing. For the synthetic one-sided, no-red-zone case containing two
65,535-entry mipmap arrays and their unchanged 113-byte neighbor, actual zone
use/peak changes **11,534,352→9,961,520 B**. This is a component fixture,
not a resident stock-game allocation.

The real 256-entry map-table case demonstrates why allocator occupancy is
kept separate from payload saving. Its two root requests together change
**49,152→43,008 B**, an exact 6,144-byte payload difference. At that threshold,
the preceding roots occupy **49,184 B of zone**, while the compact roots
occupy **0 B of zone plus 43,008 B of libc requested payload**. Reporting the
zone difference alone as a fixed RAM saving would be incorrect. The target
logs retain both actual root addresses and allocator membership.

## Reproduction

```sh
source /workspace/.onboarding/environment.sh
python3 -B tools/ps2/next9_memory_hosttest.py --sanitize --negative-controls
python3 -B tools/ps2/next9_memory_objectcheck.py
python3 -B tools/ps2/next9_memory_eetest.py
source /workspace/.onboarding/emulator-env.sh
python3 -B tools/ps2/next9_memory_eetest.py --analyze-only --run
```

Raw snapshots, extracted sources, field/allocator rows, target ABI constants,
isolated object sections and logs are under `build/nov15-next9/memory`.
EE32 witnesses use one fixed **16 MiB** static arena backing with complete
exterior canaries; the native host retains its original malloc backing.
Production allocator policy and code are unchanged. The coordinator's
full-game Hardware image/quality/memory checks are reported separately from
these component witnesses.
