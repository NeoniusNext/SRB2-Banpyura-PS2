# Exact resource locations, seventh pass — 2026-11-14

The reference is the completed sixth pass frozen in
`build/nov14-next7/baseline-src`, source revision
`b1f243f5f768d24aef5cb3ee62b45f2341e53ff4`. Production edits are limited to
`w_wad.h`, its resource constructors/readers/free path in `w_wad.c`, the pack
constructor/readers in `w_pack.c`, the folder constructor in `filesrch.c`, and
the existing location reads/copies in `ps2_texc.c` and `i_sound.c`.

The compact layout alone failed the subsequent 175-frame MAP11 `-keepspawn`
stress gate: two final composite-growth failures substituted plain columns,
where the preceding layout had none. The old-location switch restored every
baseline cache/arena metric exactly. The metadata saving therefore requires the
independent bounded composite-reconstruction retry and its integrated quality
checks; this component's field/allocator proofs alone do not establish that
the combined default is lossless. That retry is documented separately in the
Software report. The initial compact-only stress comparison is preserved in
`memory-location-stress-review/report.json`.

The independent lifetime/readiness review subsequently approved the final retry
source `32e41d258c3892894f5a4eb017b17ab5a05e63092f8802bf2df99bad5a2f323d`
with the frozen pack helper. Both actual-default and alternate counted-post
135-case real-zone matrices pass literal-byte comparison, ASan/UBSan and five
negative controls. The EE32 default matrix also passes all 135 cases and
6,982,841 compared bytes, including natural 530,432-byte recovery in exactly two
attempts. `memory-retry-helper/independent-review.json` binds that review to
source and proof hashes. Final full-engine integration is recorded separately
by the coordinator.

## Representation and saving

Every valid resource record has one of these two states:

| Resource | Logical position | Logical disk path |
| --- | --- | --- |
| Archive, cooked pack, standalone file | Full `unsigned long` offset | NULL |
| Folder file | Zero | Full original `char *` pointer |

The PS2 record now holds the offset or path in a tagged union. Its one-byte tag
occupies existing padding after the short name and compression selector.
`W_LumpPosition` and `W_LumpDiskPath` reproduce **both** logical values, including
zero for a folder position and NULL for every archive path. No offset bits,
pointer bits, sizes, names, codec values or capacities are narrowed.

Constructors initialize the tag before any access. Folder construction uses its
existing calloc root. Archive constructors explicitly initialize offset mode.
Semantic setters preserve an archive offset during a redundant NULL-path store
and preserve a live folder pointer during a redundant zero-position store.
Clearing a folder path leaves both logical values zero/NULL. The source audit
found no valid record requiring a nonzero offset and a non-NULL path together.

Actual EE GCC/header ABI:

| Property | Before | After |
| --- | ---: | ---: |
| `sizeof(lumpinfo_t)` | 40 B | 36 B |
| Natural alignment | 4 B | 4 B |
| Full offset / full pointer | 4 / 4 B | 4 / 4 B |
| Compression offset | 17 | 17 |
| New location tag offset | — | 18 |
| Size field offset | 36 | 32 |

All remaining field offsets are unchanged. The native 64-bit type changes
64→56 B; native byte savings are not EE measurements. The four stock packs
contain 15,052 records, so their simultaneously resident directory payload
changes **602,080→541,872 B, saving 60,208 B**. Zone headers, rounding and
placement are separate from this exact payload count.

Each directory still has the same count and one allocation, at its existing
point in construction. There is no new allocation, resize, reserve, capacity
change, final shrink, deferred allocation or moving-growth peak. Tags, owners,
name/string ownership and cleanup lifetimes are unchanged. This is a directory
payload bound; it is not a universal whole-process peak or fragmentation claim.
Cache filling can consume some additional room made available by the saving.

## Reader and compatibility audit

The union members have private names, making any missed direct old-field access
fail compilation. All writers and readers were audited, including WAD and ZWAD
offset adjustment, ZIP central-directory construction and lazy local-header
resolution, folder stat/open/free, the generic folder read's offset-zero seek,
pack prefetch and partial reads, TEXC's copied temporary record, private music
streaming, and model-pack readers. TEXC's `where` temporary copies a complete,
initialized pack record before its semantic offset update. There is no exposed
field-address escape, raw resource-record serialization, Lua-facing record or
metadata DMA/SIMD consumer. The metadata retains its natural alignment; image,
texture and audio data alignment is independent.

`PS2_NOOPT_LUMPLOCATION` restores the preceding layout and direct-field
operations. Actual EE ABI and every allocated object section of all five
touched C files matched the frozen baseline with this switch in the original
pre-retry snapshot. The reproducible metadata object check now also defines
`PS2_NOOPT_TEXRETRY` to isolate those same objects from the independent retry's
new pack-readiness helper. This is not a claim that the final retry-enabled
`w_pack.c` object matches the old object. The preprocessed
non-PS2 header interface is byte-identical, and both preceding/current native
C++17 header checks pass. Disk, network, save formats, graphics, physics,
features and accepted resource/add-on limits are unchanged.

The getters add a tag test to resource-location reads. Isolated production
EE `-O2` non-LTO text grows **632 B** across the five C files; rodata, writable
data and BSS are unchanged. Every isolated function frame size is unchanged.
These are component object measurements, not final LTO size, EE runtime timing,
stack high-water, FPS or VRAM results.

The later `WPack_CanReadWithoutAlloc` helper is a read-only retry precondition:
it requires existing stream/codec buffers and a supported stack-index codec
path, without creating a stream slot or allocating anything. Retry reads still
use the direct short-count pack API; readiness does not promise I/O success.
The helper adds 176 B of isolated EE `-O2` text with no stack adjustment; it is
removed by `PS2_NOOPT_TEXRETRY`. That independent cost is separate from the
metadata-only 632-byte figure above. The reconstruction retry's remaining cost
belongs to the Software and full-engine reports.

## Verification

```sh
source /workspace/.onboarding/environment.sh
python3 -B tools/ps2/next7_lumplocation_hosttest.py --negative-controls
python3 -B tools/ps2/next7_lumplocation_hosttest.py \
  --sanitize --negative-controls --out build/nov14-next7/memory-location-asan
python3 -B tools/ps2/next7_lumplocation_objectcheck.py
python3 -B tools/ps2/next7_pack_ready_hosttest.py --negative-controls
python3 -B tools/ps2/next7_pack_ready_hosttest.py --sanitize --negative-controls \
  --out build/nov14-next7/memory-pack-ready-asan
SRB2_PS2_OUT=build/nov14-next7/memory-location-sw-syntax \
  python3 -B tools/ps2/build.py --syntax --jobs 1 \
  src/w_wad.c src/w_pack.c src/filesrch.c src/ps2/ps2_texc.c src/ps2/i_sound.c
SRB2_PS2_HW=1 SRB2_PS2_OUT=build/nov14-next7/memory-location-hw-syntax \
  python3 -B tools/ps2/build.py --syntax --jobs 1 \
  src/w_wad.c src/w_pack.c src/filesrch.c src/ps2/ps2_texc.c src/ps2/i_sound.c
python3 -B tools/ps2/test_pack_reader.py --pak build/pak \
  --src /workspace/.onboarding/srb2-assets --out build/nov14-next7/memory-pack-corpus
python3 -B tools/ps2/pack_scratch_hosttest.py \
  --out build/nov14-next7/memory-pack-scratch
```

The new native fixture extracts the actual type/accessors, standalone/WAD/ZWAD/
ZIP/folder constructors, ZIP resolver, generic reader, length reader, name-index
lookup and shutdown. It executes them with actual `z_zone.c`, `ps2_mem.c`, LZF
and system zlib; unrelated engine/platform services are boundary stubs. Cooked
pack reads are exercised separately through the full production pack reader.

Baseline/current/old-switch normal and ASan/UBSan runs pass:

- **176 real-arena table cases**, with counts 1..65,535, all four stock counts,
  both allocation directions, red zones and fragmented layouts. Full unsigned
  offsets include zero, signed-boundary/high-bit values, UINT32_MAX and native
  ULONG_MAX. All codec values, full sizes/hashes/names, pointer identities,
  redundant setter operations and complete record copies are checked.
- **20 actual constructor/read/free cases**, covering raw WAD, ZWAD with an
  independently generated literal LZF stream, stored/deflated ZIP with extra
  fields/comments, folder paths and standalone files. Every returned byte and
  full name is checked against independent raw source/oracle data. Tests include
  whole/partial/empty/end/beyond-end reads, duplicate case variants, indexed
  long-name behavior, a second add-on and unchanged root-vector growth.
- Dynamic folder append/truncate, missing-file errors and replacement by a
  directory retain existing behavior. Failed ZIP local-header resolution keeps
  the complete lazy offset; subsequent successful reads resolve it normally.
- **13 actual zone allocation-failure stages** in WAD construction preserve the
  preceding unpublished count and allow cleanup/reinitialization. The fixture
  releases libc temporaries after the caught fatal allocation path, emulating
  process cleanup; the game normally exits that path. Real cache eviction clears
  its cache owner while preserving static metadata.

The baseline and old switch pass 96,110,634 checks each; current passes
96,110,652. The extra checks come from allocator tiling, not additional logical
cases. All complete semantic/decoded-byte digests equal
`56fc0a7fe43ef4b3`; all old-switch allocator results equal baseline exactly.
There is no measured directory request, retained arena use or peak increase in
these cases. Five negative controls detect an incorrect folder offset, NULL
store clobber, lost folder tag, truncated offset and zero-store path clobber.

The full pack suite passes **15,052 stock lumps and 300,940 partial reads**,
original archive names/order/hash/longname/size/CRC comparisons, v1/v2/headless
fixtures, heap indexes, malformed layouts, prefetch/drop, device/window policies
and injected I/O failures. All nine source packs/archives/sidecars retain their
hashes. An explicitly rebuilt frozen seventh-pass reader produces four tables
that match current literally: **1,165,763 bytes**, hashes in
`memory-pack-baseline/report.json`. The scratch fixture also passes current
ASan/UBSan, 5,968 assertions, callbacks/failures and all four real packs through
system liblz4.

The readiness helper's independent actual-reader fixture passes 392 checks in
normal and ASan/UBSan builds. Each query must preserve all stream records,
buffer pointers, allocator counts/live bytes, device counters, error text and
the replacement cursor. Missing buffers/streams, RAW/LZ4 index boundaries,
unsupported codecs, prepared full RAW/LZ4 reads and eight-handle turnover are
covered. Ten tracked buffer allocations free completely. Three negative
controls detect creation of a stream slot, a missing-buffer check removal and
acceptance of a heap-index path. Evidence is in `memory-pack-ready*`;
`memory-retry-helper` freezes the independent helper and target cost proof.

Evidence is under `build/nov14-next7/memory-location*` and `memory-pack-*`.
`memory-location-production` contains frozen production copies, old/current
hashes and the isolated diff. Full-engine builds, integrated memory/frame
comparisons and final ELF hashes are recorded by the coordinator.
