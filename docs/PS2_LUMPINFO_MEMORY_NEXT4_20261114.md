# Compact resource-directory records — fourth pass, 2026-11-14

The PS2 profile now stores the internal `lumpinfo_t.compression` selector in one
byte beside the nine-byte short name. It occupies previously unused alignment
padding; removing the old trailing enum reduces the EE record from **44 to 40 B**.
All other EE field offsets and natural alignments are unchanged. On the native
64-bit host the corresponding record changes 72→64 B; host savings are not EE
measurements.

The `compmethod` enum and its values remain unchanged. All resource producers
map raw archive methods to known `CM_*` values before storing them; every value
fits the byte. Unknown archive compression remains `CM_UNSUPPORTED`, rather
than being truncated from its on-disk number. Compile-time checks enforce the
current enum bound and support both C and C++ header consumers.

No file offset, compressed/uncompressed size, hash, pointer, name or capacity is
narrowed. WAD/ZIP/SRP2 disk records and all network/save structures are separate
and unchanged. The default non-PS2 header's preprocessed interface is
byte-identical to the reference. No allocation, resize, lookup algorithm, codec
or resource lifetime was added or changed.

The four stock packs contain 15,052 records: this saves **60,208 requested bytes**
while their directories remain resident. It is a simultaneous directory-payload
saving, not a sum over evicted assets. Arena rounding and alignment are measured
separately by the coordinator's integrated memory checkpoints.

## Proof and audit

The reference is the immediately preceding completed pass in
`build/nov14-next4/baseline-src`, recreated by
`tools/ps2/prepare_next4_baseline.py` from verified frozen source patch/hashes.
Git HEAD alone is not the reference.

- Actual EE-header ABI probe: size 44→40, alignment 4 unchanged, compression
  size 4→1 and offset 40→17. Offsets position 0, disksize 4, name 8, hash 20,
  longname 24, fullname 28, diskpath 32 and size 36 are all unchanged.
- Actual `z_zone.c`/`ps2_mem.c` fixture: 88 cases per version, native allocation
  counts 1..65,535 with two-sided policy and red zones on/off. Every codec store
  preserves full-width sizes/hashes, all name bytes, pointer identity and a
  neighboring allocation. All logical field digests agree; no measured arena
  used/peak increase. The reference/current normal cases perform 6,932,871 /
  6,932,862 checks respectively; allocator internal consistency-check counts
  differ with the changed physical block layout. Both pass ASan/UBSan too.
- Actual full header: PS2-profile C++17 syntax passes for reference and current;
  the preprocessed non-PS2 C interface is identical. Existing unrelated C++
  literal-suffix warnings remain visible in the logs.
- `test_pack_reader.py`: all 15,052 stock lumps, 300,940 partial reads, metadata,
  names/order/hash/CRC, raw/LZ4/v1/v2/headless fixtures, heap indexes, malformed
  inputs, prefetch/drop, policies/custom windows and injected I/O errors pass.
  All four complete output tables are byte-identical to the preceding pass;
  all nine input packs/archives/sidecars remain unchanged.

All producers, copies and readers of `lumpinfo_t` were audited, including ZIP
lazy positions, folder resources, shader/sprite/skin lookup, private music
streaming, model packs and TEXC's typed temporary record. No field-address escape
as `compmethod*`, raw struct serialization or metadata SIMD/DMA consumer was
found. A separate Software subagent independently confirmed that review.
The previous 44-byte stride already precluded 16-byte alignment for every entry;
the 40-byte stride retains required 4-byte EE field alignment. Texture/PCM/DMA
buffer alignment is independent and unchanged.

Reproduce with SDK headers and the existing host compiler/liblz4/assets:

```sh
source /workspace/.onboarding/environment.sh
python3 -B tools/ps2/prepare_next4_baseline.py
python3 -B tools/ps2/next4_lumpinfo_hosttest.py --ee-abi
python3 -B tools/ps2/next4_lumpinfo_hosttest.py \
  --out build/nov14-next4/lumpinfo-asan --sanitize
python3 -B tools/ps2/test_pack_reader.py --pak build/pak --src /path/to/srb2-assets \
  --out build/nov14-next4/pack-corpus
```

Proofs are in `build/nov14-next4/{lumpinfo,lumpinfo-asan,pack-corpus}`. Full-engine
RAM, add-on, framebuffer comparisons and final ELF hashes are recorded in
`PS2_OPTIMIZATION_NEXT4_20261114.md`.
