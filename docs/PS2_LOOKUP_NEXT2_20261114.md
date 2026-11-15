# Resource lookup, iteration 2 — 2026-11-14

The reference is the cumulative source saved in
`build/nov14-next/baseline-src/src/w_wad.c`, before this iteration. No file
formats, resource names, namespaces, add-on order or lookup limits change.

## Changes

The existing 64-entry resource-number cache keeps its hashes, numbers and names
in separate arrays within the same private object. A miss examines 256 bytes
of contiguous hash words instead of one word in each 40-byte record. Both
layouts occupy **2560 bytes**, retain 64 entries, use the same circular cursor
and replacement order, and invalidate at the same add-file sites. Names are
still zero-padded and long names of 32 bytes or more still bypass this cache.
`PS2_NOOPT_LUMPCACHE` retains the previous layout. Native non-profile builds
also keep that layout.

Long-name lookups can reuse a file's existing short-name hash index: every
supported loader derives the eight-character short name from the beginning of
the same long basename. Cooked-pack loading additionally checks that the stored
long name equals the derived basename. No name mutation after loading was found
in the independent review. The index's chains are ascending, preserving the
first match at or after `startlump`.

`W_CheckNumForLongNamePwad` keeps its original 256-character normalization and
case-sensitive full comparison. Long patch lookups keep their original
case-insensitive full comparison and the original two intervals outside the
flat namespace. Only already allocated indexes are used for the new long-name
paths: an unindexed file still uses its previous scan and does not incur a new
allocation, allocation failure, or memory peak.

## Verification

```sh
source /workspace/.onboarding/environment.sh
python3 -B tools/ps2/next2_lookup_hosttest.py \
  --assets /workspace/.onboarding/srb2-assets --negative-controls \
  --out build/nov14-next/lookup-final
python3 -B tools/ps2/next2_lookup_hosttest.py \
  --assets /workspace/.onboarding/srb2-assets --sanitize --negative-controls \
  --out build/nov14-next/lookup-final-asan
```

Both final runs pass **177226 checks**, including 15052 stock resource basenames.
The fixture compiles actual frozen and current source in separate translation
units against the real engine types. It compares lookup values, namespace-cache
state and every hash, number, name byte and cursor in the 64-entry cache. It
also counts allocations: neither long-name path may allocate a new index.

Cases include 0..65535 resources, the 32767 sentinel and adjacent indices,
explicit start offsets, duplicates, dense eight-byte prefix collisions,
case variants, empty basenames, 31/32/33-byte cache boundaries, 255/256/300-byte
names, missing/reversed/empty flat ranges, wraparound and cache invalidation.
ASan/UBSan report no findings. Mutating the full long-name comparison into an
eight-byte prefix comparison or corrupting a cached result is detected.

## Local EE timing

```sh
python3 -B tools/ps2/next2_lookup_hosttest.py --ee
source /workspace/.onboarding/emulator-env.sh
python3 -B tools/ps2/run_pcsx2.py \
  --elf build/nov14-next/lookup/ee/LOOKUP.ELF \
  --log build/nov14-next/lookup/ee/run.log \
  --until 'LOOKUP DONE' --marker 'LOOKUPBENCH ' --timeout 180
```

EE GCC 15.2; extracted functions are separate non-LTO translation units,
retaining normal engine release options, rather than allowing the benchmark's
allocator/address knowledge to specialize them. Each row performs 128 queries,
best of five paired repetitions. Result checksums match for each pair.
The synthetic file has 4096 resources and an already constructed index.

| Workload | Before CP0 counts | After CP0 counts |
| --- | ---: | ---: |
| Long names near end of file | 24106135 | 158131 |
| Missing long name | 17271176 | 60040 |
| Long patch names outside flats | 27369700 | 158030 |
| Oldest-entry cache hits, rotating names | 152840 | 136712 |
| 64-entry cache miss | 122504 | 105864 |

The last two rows reduce instruction cost by about 10.6% and 13.6%. The large
long-name reductions replace complete scans with indexed candidate comparisons;
they do not represent whole-game FPS. Prefix-colliding names can still require
a long chain, and files without indexes keep their linear scans. PCSX2 does not
model physical EE data-cache behavior in this setup, so the smaller hot hash
working set is not presented as a measured physical-cache gain.

The preceding iteration's rejected streamed-DEFLATE changes remain excluded.
