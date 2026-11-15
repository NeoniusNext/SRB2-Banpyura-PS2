# Resource metadata memory, continuation 2 — 2026-11-14

The comparison baseline is the completed preceding iteration saved in
`build/nov14-next/baseline-src`, rather than the older Git HEAD. Production
changes are limited to `r_patch.c` and the private texture-name cache in
`r_textures.c`.

## Included changes

Embedded Doom patches still use one zone allocation for the patch, column
directory, post directory and pixels. The two typed directories now use their
natural alignment. The patch block and pixel base retain their preceding
alignment, including the 16-byte pixel alignment used by bulk-copy fast paths.
Both the ordinary constructor and its allocation-failure-tolerant counterpart
use the same layout. `PS2_NOOPT_PATCHLAYOUT` restores the preceding layout.

EE GCC confirms `sizeof(patch_t) = 48`, `sizeof(column_t) = 12` and
`sizeof(post_t) = 12`, with four-byte directory alignment. For `W` columns,
`P` posts and `N` pixel bytes, the directory/pixel portion changes from

```text
old = align16(align16(48 + 12*W) + 12*P) + N
new = align16(48 + 12*W + 12*P) + N
```

Each valid EE patch request is unchanged or 16 bytes smaller. There is no new
allocation, resize, cache capacity, shrink-after-load step, or old-plus-new
allocation peak. Dimensions, offsets, every pixel, every post, embedded flags,
optional children, owners and purge tags retain their preceding behavior.
Software consumers read the arrays with scalar accesses. Hardware conversion
also reads these patch arrays on the CPU; the patch interiors are not DMA
sources. Keeping pixel-base alignment preserves the source/destination
alignment classes of existing pixel copies.

The private texture-ID cache places its word fields together, reducing each
entry from 24 to 20 bytes on both the host and EE ABI. The nine-byte terminated
name, full-width hash and ID, namespace byte, lookup ordering, add-on
invalidation and per-entry growth policy are unchanged. This saves four bytes
per live entry before zone rounding; it does not add a new table or reserve.

## Verification and byte counts

```sh
source /workspace/.onboarding/environment.sh
python3 -B tools/ps2/nov14_resource_hosttest.py --negative-controls
python3 -B tools/ps2/nov14_resource_hosttest.py \
  --out build/nov14-next/resource-memory-asan --sanitize --negative-controls
SRB2_PS2_OUT=build/nov14-next/resource-syntax python3 -B tools/ps2/build.py \
  --syntax --jobs 1 src/r_patch.c src/r_textures.c
SRB2_PS2_HW=1 SRB2_PS2_OUT=build/nov14-next/resource-hw-syntax \
  python3 -B tools/ps2/build.py --syntax --jobs 1 src/r_patch.c src/r_textures.c
```

The dedicated native fixture executes extracted production constructors,
composite generation, cache checks, name lookup and the actual zone/arena.
It checks 10,319 validated source patches from prepared SRB2/CHARS packs and
152 synthetic patches, including odd widths, empty columns, zero-length posts,
relative tall-patch offsets and signed image offsets. Every decoded post and
pixel is independently checked against the raw source for both constructors;
each request must fit within the preceding layout's request. Attached flats,
rotated children, null sources and a failed try-allocation also pass cleanup
checks.

All 336 composite cases match the baseline's normalized pixel/post stream.
They combine seven sizes, six post/placement patterns, raw/cooked paths,
cached/uncached patches, and one-/two-sided arena modes. Eviction clears the
real cache owner and the production cache check rebuilds the cache. There is no
increased measured allocation peak or retained allocation in these cases.
An additional 1,200-entry test checks full eight-character names, case folding,
the flat namespace, absent names, clearing and a subsequent add-on override.
The old patch-layout switch and the older separate-copy composite path produce
the same streams. Deliberately corrupted patch and composite offsets are
rejected by the independent raw checks or baseline comparison.

The table below sums **all corpus allocations**, not simultaneously live game
RAM. Host blocks use the actual 64-bit allocator, with its larger headers and
red zones. EE figures are calculations using constants emitted by EE GCC and
the release 16-byte zone header; minimum rounded sizes exclude extra alignment
gaps and absorbed free tails.

| Corpus measure, 10,471 patches | Before | After | Difference |
| --- | ---: | ---: | ---: |
| Native requested bytes, measured | 219,418,301 | 219,378,989 | −39,312 |
| Native allocated block bytes, measured | 219,987,952 | 219,948,176 | −39,776 |
| EE requested bytes, calculated | 194,506,349 | 194,477,805 | −28,544 |
| EE minimum rounded blocks, calculated | 194,731,536 | 194,702,992 | −28,544 |

In the real-asset subset, 1,734 patches have the smaller EE layout: 1,348
sprites, 374 graphics, seven patch-namespace resources and five textures.
Their summed saving is 27,744 bytes. The remainder of the total belongs to
synthetic fixtures. Actual resident saving depends on which patches are live.
The 1,200-entry name cache uses 28,848 → 24,048 native arena bytes, a 4,800-byte
saving; EE payloads are 28,800 → 24,000 bytes before their common header.

ASan/UBSan and Software/Hardware target syntax checks pass. Logs, corpus
metadata, normalized-stream hashes, exact allocator results and emitted EE ABI
constants are in `build/nov14-next/resource-memory*`. The host fixture uses a
deterministic blend stub and a cooked-picture converter; it does not emulate
the PNG decoder, EE timing or GS DMA. Integrated game RAM, frame comparisons
and full builds are recorded by the coordinator. No FPS or VRAM gain is
inferred from these metadata measurements.

## Rejected scratch-table candidate

The PS2 composite path appears to discard its temporary column descriptors
before final output, suggesting that their allocation can be removed. The
real-zone comparison caught a counterexample: eliminating that allocation
lets the later prefix-index table occupy space adjacent to the pixel root.
The final root growth then moves instead of extending in place. For an empty
65×130 composite on the one-sided native arena, peak use rose from 11,664 to
18,944 bytes. That can introduce a new allocation failure. The complete
candidate was removed; the original scratch allocations and their order remain.
