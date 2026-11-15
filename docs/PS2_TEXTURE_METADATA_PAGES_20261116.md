# Stable texture metadata pages — 2026-11-16

This component reduces the PS2 Hardware renderer's eager per-texture metadata.
It retains the complete texture-number range, persistent record addresses,
chroma variants, regeneration, all texture formats and dimensions, and the
existing rendering and residency policies. Whole-game memory and image results
are separate integration evidence; the component figures below are allocation
payload bounds and controlled host execution.

Baseline is `604cfd150c5240142d8cb5367950b53d987916d1`. The two product sources
are frozen at:

| File | SHA256 |
| --- | --- |
| `src/hardware/hw_cache.c` | `2ff513529707134b55d2fc6e048ecbe9177d77c96e9e4cec22da5bc69fe9e542` |
| `src/hardware/hw_data.h` | `ae19d9a3ce26bda9a11acb9fe52906712edb5b711b759cf9d070e63077c190a7` |

## Storage and lifetime

The default PS2/profile path allocates two arrays of page pointers up front.
A first wall or flat lookup allocates that kind's 16-record page, zero-filled.
The final page allocates exactly the remaining records. Page count uses division
and a remainder test, avoiding an overflowing round-up addition. Counts and
indices retain `size_t` or their original full-width types. There is no new
texture count, page count, or working-set cap.

Records never move after allocation. A lookup of transparency or a geometry
cache key reads an existing page, returning the original zero flag for an
untouched page without allocating it. Actual selection, warming and
regeneration obtain a persistent record. A failed page allocation installs no
pointer and uses the existing guarded out-of-memory path, leaving earlier live
pages reachable for normal renderer cleanup.

Teardown preserves increasing logical texture order for every allocated
record. Each wall/flat retains the original child deletion, child free, child
pointer clear, primary deletion, and primary data free order. The driver is
called before pixel storage is freed, retaining its DMA fence and owner cleanup.
Only after all records have completed that sequence are the pages and pointer
arrays freed. A never-allocated page has never supplied a mipmap address to the
driver, a frame plan, a geometry batch or a zone owner. Its absent zero-record
callbacks therefore have no owned DMA source or record to synchronize.

The existing compact-flat selection and `-hwgo` parser still choose the flat
record stride. Page allocation changes physical addresses, including legacy
pointer sorting. Small address-order edge differences remain possible in those
legacy modes and require the coordinator's image policy; no pixel identity is
claimed from local lifetime tests.

`PS2_NOOPT_TEXTURE_PAGES` selects the previous eager tables. The new
`PS2_NOOPT_MIPMAP_STORAGE` independently selects the baseline 76-byte mipmap.
Using both provides the baseline storage organization. The existing
`PS2_NOOPT_MIPMAP_LAYOUT` still selects its historical 88-byte layout.
Generic, no-profile and non-PS2 declarations retain their previous layout.

## Bounded scalar storage

The PS2 mipmap stores format and texture flags in bytes. All current format
values, including zero for an unloaded model texture, fit in `0x00..0x22`;
the defined texture flags occupy bits `0x01`, `0x02`, `0x10` and `0x40`.
`regen_kind` and the cached index-255 status each use two bits, visibility uses
one, and drop reason uses three. Complete current writers produce kinds/status
`0..2`, visibility `0/1`, and drop reasons `0..5`. These fields are scalar
loads/stores; no production caller takes their address.

Texture handles, regeneration IDs, pointers, all frame/view counters,
dimensions, plan indices, upload count, rebuild cost, desired level and hint
retain their original ranges. In particular the desired-level sentinel 255
remains valid. Texture samples, coordinates, palette entries, GS formats and
resolution are unchanged.

The actual EE compiler, using the full production header and its transitive
types, verifies:

| ABI fact | Baseline | Candidate |
| --- | ---: | ---: |
| `sizeof(GLMipmap_t)` | 76 | 68 |
| `sizeof(GLMapTexture_t)` | 84 | 76 |
| Mipmap alignment | 4 | 4 |
| Pixel/zone-owner pointer offset | 4 | 4 |
| Regeneration ID, downloaded handle, frame counter width | 4 | 4 |
| Plan index and actual patch extent width | 2 | 2 |

## Payload bounds

For `N=4740`, the old default primary wall/flat tables request
`(84+76)*N = 758400` bytes. The candidate initially requests
`2*ceil(N/16)*4 = 2376` bytes, a 756024-byte reduction before pages are used.
If every page becomes populated, the primary metadata requests
`(76+68)*N+2376 = 684936` bytes, still 73464 bytes smaller. Chroma and patch
mipmaps save a further eight bytes each. Sparse page use usually saves more.

These are exact target payload calculations, not measured game free space.
Allocator headers, page allocation count, libc-versus-zone placement, static
code size, peak memory, texture reconstruction and native pressure behavior
must be assessed together by integration. No reserve, arena boundary, quota,
spill threshold or texture budget was adjusted.

## Component execution

The original final page suite is
`build/nov15-next16/ram-pages-host-v6/report.json`, produced by
[`next16_texture_pages_hosttest.py`](../tools/ps2/next16_texture_pages_hosttest.py)
and its [service fixture](../tools/ps2/next16_texture_pages_hosttest.c).
It compiles complete actual constructor, getter, transparency/key lookup,
regeneration, owner-transfer and destructor bodies independently for baseline
and candidate. The fixture supplies checked allocation/zone/driver boundaries;
its frees release real backing allocations, allowing ASan to observe lifetime
errors. Normalized logical snapshots compare independently of pointer addresses
and C padding. The texture composer service is a stand-in; unchanged full
composition is covered by whole-game integration rather than this fixture.

The suite checks 851 cases across seven profiles, including both independent
opt-outs, both together, the historical layout, full flat records and non-PS2
profile code. Cases exercise zero and partial pages, boundaries 15/16/17 and
31/32/33, every record in 4740-entry tables, ID 65536 in a 65537-entry table,
stable primary/twin identities, scale-record selection, regeneration after
pixel purge, driver-owned sources, zone owner invalidation, reload and final
cleanup. First/second table allocation, child allocation, and both wall/flat
page failures are exercised. Five source corruptions must fail: wrong wall
offset, wrong flat offset, wrong regeneration scale record, missing flat twin,
and freeing a page before its owned records.

An independent isolated layout suite at
`build/nov15-next16/ram-storage-host-v3/report.json` retains baseline cache code
and changes only the final header. Its
[`runner`](../tools/ps2/next16_mipmap_storage_hosttest.py) passes 270 actual-source
lifetime comparisons across six profiles and 2700000 field-value/neighbor
checks per role with ASan/UBSan. Six deliberately insufficient widths are
detected. The complete pinned texture manager runs independently with each
header: 93394955 checks, 19905000 compared texels, 794 uploads and 1603 complete
normalized packet/cache/owner snapshots per role agree. Source bytes and
executed tool dependencies are retained in each output's `frozen-tools/`.

The final full cache source above also includes the independent
[try-only AP88 patch rescue](research/opt16/PATCH_RESCUE_20261116.md).
`build/nov16-hwopt/pages-after-rescue/report.json` reruns all 851 page cases
and five controls against that final source, retaining the actual EE ABI probe.
The focused page and isolated-layout runners explicitly define
`PS2_NOOPT_PATCH_RESCUE`, so their service fixtures continue to test storage
independently. The rescue has its own complete production-drawer, source-lifetime,
allocation-failure and PNG/rotation/mapping byte proof; it is not silently
replaced by a stand-in in the page tests.

Default and double-opt-out `hw_cache.c` pass actual EE build syntax checks.
Preprocessed mipmap declarations are exact to baseline for generic,
non-PS2/profile, PS2/no-profile, baseline-storage opt-out and historical-layout
opt-out configurations. No emulator or native game memory result is claimed by
these component checks.

Earlier host iterations are retained under `ram-pages-host-v1` through `v5`.
Their fixture development exposed assumptions about the generic fatal-error
boundary, driver ownership of flat-child pixels, the regeneration scale-record
oracle, and an intercepted host-free wrapper. None required a product-source
change; those incomplete runs are not promoted to the final result.

```sh
source /workspace/.onboarding/environment.sh
python3 tools/ps2/next16_texture_pages_hosttest.py \
  --out build/nov15-next16/ram-pages-replay --negative-controls
```

For isolated layout replay, provide `next16_mipmap_storage_hosttest.py` with a
`--candidate` source directory containing baseline `hw_cache.c` and the final
`hw_data.h`; the saved `ram-storage-isolated-source` is such a directory.
