# Texture record RAM, 2026-11-15

The driver keeps `blk`, `nblk`, `uw` and `vh` as `u16` in its private texture
records. Actual EE GCC 15.2 compilation measures **148 → 140 bytes** per record,
with four-byte alignment retained. The initial 512-record allocation requests
**75,776 → 71,680 bytes**, saving **4,096 bytes**. Capacities of 1,024 and 2,048
save 8,192 and 16,384 payload bytes respectively. Record counts and growth
capacities are unchanged. These are requested payload savings; libc chunk
rounding and whole-game memory pressure require separate integrated evidence.

The existing compile-time bound `VRAM_BLOCKS <= 0xffff` covers block addresses
and complete contiguous span lengths, including unused GS blocks within a
span. Valid addresses are below 16,384 and an image cannot occupy more than
16,384 blocks. The source validation bounds both texture axes by 16,384;
an AP88 crop is bounded by those source dimensions. Other planned images,
split pieces and captures have axes at most 1,024. The legacy AP88 crop can
overwrite a planned width with a crop wider than 1,024; this behavior is
preserved, without adding a rejection or changing the selected shifts.

All record consumers use named C members. Records are not DMA sources or a
serialized format. Pointer/handle fields, full-width frame counters, LRU
links and policy quantities retain their types. Admitted spans satisfy
`blk + nblk <= 16384`, so their integer promotions cannot overflow.
The `uw - 1`/`vh - 1` GS register expressions operate on positive admitted
dimensions. Texture source bytes, conversions, filtering, CLUT contents,
VRAM spans/budgets, allocation order, source-cache keys, locks and release
rules are unchanged by this representation change. No PSMT4, new visibility
skip or reduced texture quota is introduced.

[The regression runner](../../../tools/ps2/texture_record_hosttest.py) compiles
the complete actual texture, palette, register and transform sources from
`e10593b0d` and the candidate independently. Host services replace engine
allocation and DMA execution. Each ordinary and ASan/UBSan run passes
**93,394,955 checks**, **19,905,000 independently checked texels**,
**794 upload cases**, **1,603 snapshots** and **12,000 allocator operations**.
Complete normalized snapshots compare directly byte for byte across all four
runs. Their SHA-256 is
`186130f8479861a8b3c9bd2346bd72c8d144e8862fca62622148678559734858`.

The corpus covers all eight caps, NPOT dimensions and cropped strides, source
axes through 16,384, alpha-zero RGB and partial alpha, index 255, a legacy
1,536-wide AP88 crop, tall split images and keyed views, pending source
references across eviction/reconstruction, captures and palette invalidation,
512/1,024/2,048 record growth, failed-growth retry, and 8,192 fragmented free
runs coalescing at the last GS block. An independent bitmap checks low/high
aligned allocations. Executed `u8` corruptions of addresses, span lengths,
used dimensions and frame counters all fail the semantic checks.

The native compiler also emits every record member's actual offset and size.
Evidence is `build/texture-record-v6/report.json` with source hashes, binaries,
compiler objects, build logs and full snapshots beside it. This component
fixture establishes no physical PS2 cache-coherence, GS-rendering, frame-time,
FPS or whole-game pressure result. The coordinator runs those integration
checks separately.

```sh
python3 tools/ps2/texture_record_hosttest.py --negative-controls \
  --ee-cc "$PS2DEV/ee/bin/mips64r5900el-ps2-elf-gcc" \
  --out build/texture-record-replay
```
