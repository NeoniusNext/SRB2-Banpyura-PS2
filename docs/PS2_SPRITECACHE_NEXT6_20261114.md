# Exact sprite-header cache — sixth pass, 2026-11-14

The reference is the completed fifth pass in
`build/nov14-next6/baseline-src`, reconstructed and checked by
`tools/ps2/prepare_next6_baseline.py`. Production changes are confined to
`r_state.h`, the header stores and readers in `r_things.c`, and the existing
sprite-header readers in `hardware/hw_main.c`.

## Representation and resident allocation

Every cached width, height and offset comes from the four `INT16` outputs of
`W_ReadPatchHeaderPwad`. The PNG path already converts its dimensions to
`INT16` before returning; the Doom-patch path reads signed 16-bit fields. The
single cache writer shifts these values by 16 bits and adds `FEETADJUST`, four
whole pixels, to the top offset. Every resulting 32-bit fixed-point value has
zero fractional bits.

The PS2 cache now stores those signed high words directly. Its getters promote
each word to `fixed_t` and multiply by `FRACUNIT`, reproducing every original
32-bit result. The top-offset store performs the four-pixel adjustment modulo
65536 before interpreting the high word as signed. In particular, input top
offsets 32764..32767 keep their preceding wrap to negative fixed-point values.
No source-header value, sign bit, offset, dimension or fractional precision is
discarded. The getters return full-width fixed-point values before the
existing arithmetic and float conversions.

Actual EE GCC/header ABI measurements:

| Property | Preceding | Current |
| --- | ---: | ---: |
| `sizeof(sprcache_t)` | 16 B | 8 B |
| Field storage | 4 B | 2 B |
| Natural type alignment | 4 B | 2 B |
| Initial capacity | 8192 | 8192 |
| Initial requested payload | 131,072 B | 65,536 B |

The initial reservation therefore retains **65,536 B less payload**. A 16384
entry reservation retains 131,072 B less, and subsequent reservations also use
half their preceding payload. These are simultaneous cache-reservation
savings while the root is live, not a sum of resource-corpus allocations.
The allocator still aligns every large table root to at least 64 bytes; its
individual signed-word loads need only two-byte alignment. This metadata is
not a DMA/SIMD buffer.

`R_InitSpriteLumps`, the 8192-entry initial reservation, count update, doubling
trigger, doubling factor, accepted sprite-lump bound, allocation order,
`PU_STATIC` tag and owner slot are unchanged. No final shrink, additional
reserve, new allocation site, deferred allocation or capacity reduction was
introduced. The new field names make any remaining direct old-field read fail
to compile. The complete source search finds only the one writer and the
updated Software/Hardware/hash readers; no field-address escape, raw struct
serialization, Lua exposure or external metadata writer was found.

For preceding capacity C, a growth requests `32*C` payload bytes. The compact
replacement requests `16*C`; even if it moves while the preceding table could
have grown in place, its old-plus-new payload is `8*C + 16*C = 24*C`. The
smallest C is 8192, leaving at least 65536 bytes below the preceding single
replacement payload, much more than an extra zone header/alignment gap. This
avoids the moving-growth problem of the previously rejected capacity-shrink
candidate. It is a table-allocation bound, not a universal whole-process peak
or arbitrary-fragmentation theorem.

There is no new callback or changed resource lifetime. Cache eviction can
clear a separate purgeable owner while the sprite-header root remains static.
Existing initialization and reallocation failures keep the same root/count/
capacity publication behavior. In particular, the preceding grow code already
publishes its doubled capacity before a failed allocation; this pass preserves
that behavior and does not claim to make it transactional. The fault fixture
releases the surviving root and reinitializes before retrying.

Defining `PS2_NOOPT_SPRCACHE` restores the full preceding PS2 layout and stores.
All complete old-switch object text, read-only data, writable data and BSS
match the preceding objects byte-for-byte. The preprocessed non-PS2 header
interface is also byte-identical. Disk, save/network, Lua-facing data and all
rendering/physics/add-on limits remain unchanged.

## Exactness and lifecycle proof

`tools/ps2/next6_spritecache_hosttest.py/.c` extracts the actual frozen/current
type, getters, writer block, initialization function and grow block. It uses
the actual `z_zone.c` and `ps2_mem.c`, with platform/engine boundary stubs.
Both initializer and growth source extracts must match the reference exactly.

Each baseline/current/old-switch run passes **13,044,729 checks**, including:

- All 65536 input bit patterns for each of the four signed 16-bit fields,
  permuted independently across the exhaustive run; an independent unsigned
  32-bit oracle checks all 262,144 reconstructed field values, including top
  overflow and signed minima/maxima.
- 80 reserve/lifecycle cases: ten counts 0..65534, both arena directions,
  red zones on/off, and clear/fragmented layouts. Counts straddle 8192, 16384
  and 32768, including the unchanged grow-on-equality behavior and later add-on
  style appends. All prior entries remain exact after growth.
- Same reserve capacities and owner clearing, no larger measured request,
  arena used/peak or construction allocation count, and complete release.
- Injected failures at initialization and each of the three accepted growth
  boundaries, surviving owner/content checks, release/reinitialization, and
  eviction of a real purgeable block without changing the static table.
- Four negative controls detect a removed top adjustment, a lost top sign bit,
  a wrong-height reader and a changed initial reservation.

All logical digests agree: `416160f6355b9973`. The old switch reproduces every
preceding allocator row exactly. ASan/UBSan repeat the complete suite and all
negative controls. The candidate keeps shift checking enabled. Only the
unchanged reference/old-switch signed-negative left shifts disable UBSan's
shift diagnostic; their source is not normalized. Both use the engine's
existing wrapping-arithmetic policy.

Selected real **native** arena results below include native 32-byte headers,
not EE 16-byte release headers. Clear layouts allow in-place growth; the
fragmented 8192-row forces a move. These are synthetic table lifecycles, not
whole-game RAM samples.

| Count / layout, one-sided/no red zone | Preceding peak | Current peak | Preceding retained | Current retained |
| --- | ---: | ---: | ---: | ---: |
| 0 / clear, initial reservation | 131,104 B | 65,568 B | 131,104 B | 65,568 B |
| 8192 / clear, capacity 16384 | 262,176 B | 131,104 B | 262,176 B | 131,104 B |
| 8192 / fragmented | 393,584 B | 196,976 B | 262,784 B | 131,712 B |
| 65534 / clear, capacity 65536 | 1,048,608 B | 524,320 B | 1,048,608 B | 524,320 B |

The separate-TU EE probe uses the actual writer and four-read sequences from
Software projection and Hardware extent preparation. Actual EE execution
passes **786,432 checks** over all 65536 input sets, including literal float
bit comparisons and independent fixed-point results. Digest:
`86c92995bb0d0383`. A deliberately wrong height getter is rejected on the EE
at input zero. The valid ELF SHA256 is
`536923ba3ad853adb34b56fb8104e989aa096659dd56e52bdb600f5a33c21267`;
the negative ELF is
`bf6b2f2768b5c953d15f4625d830a4b2ebb7a9a3fb3544cf7f1766c1046883f2`.

## CPU/code costs

The storage saving adds sign extension/shift work at reads. The paired EE
probe measures these exact four-value gathers with their common call/store/
loop overhead, separate translation units and no LTO:

| Selected gather | Preceding | Current | Added cost |
| --- | ---: | ---: | ---: |
| Four fixed-point fields | 32 cycles/call | 37 | +5, 15.625% |
| Four fields converted to float | 55 cycles/call | 61 | +6, 10.9091% |

All 12 rows give these results: 1/64/8192-entry working sets, sequential and
stride-17 indexes, 8192 calls, alternating order and best of five. This is an
explicit CPU tradeoff for retained RAM, not a rendering speedup. PCSX2 target
instruction timing does not measure physical-console cache effects.

Complete real release EE non-LTO objects retain the existing `-O2`, `-G8` and
per-unit policies. `r_data.c` text is unchanged. Software `r_things.c` text
grows **40 B**. Hardware adds the same 40 B plus **128 B** in `hw_main.c`.
Read-only/writable data and BSS are unchanged. These object deltas are not
final linked/LTO section measurements. The actual producer
`R_AddSingleSpriteDef` shrinks 3232→3208 B with the same 144-byte frame; its
write CPU time was not measured separately. Inlined Software sprite/precip
projection in `R_AddSprites` keeps its 384-byte frame; Hardware
`HWR_ProjectPlain`/`HWR_ProjectSprite` keep 240/320-byte frames. No new static
or transient scratch buffer was added.

Whole-game MAP01/MAP11 Software and MAP01/MAP02 Hardware frame/hash/RAM
comparisons, the 175-frame stress trace, final linked sections and physical
measurement limits belong to the coordinator's sixth-pass report. No FPS,
VRAM or universal peak-RAM improvement is inferred here.

## Reproduction

```sh
python3 -B tools/ps2/next6_spritecache_hosttest.py --negative-controls
python3 -B tools/ps2/next6_spritecache_hosttest.py \
  --out build/nov14-next6/spritecache-asan --sanitize --negative-controls
source /workspace/.onboarding/environment.sh
python3 -B tools/ps2/next6_spritecache_hosttest.py --ee-abi --negative-controls
SRB2_PS2_OUT=build/nov14-next6/spritecache-object-gen \
  python3 -B tools/ps2/next6_spritecache_objectcheck.py
python3 -B tools/ps2/next6_spritecache_eetest.py
source /workspace/.onboarding/emulator-env.sh
python3 -B tools/ps2/run_pcsx2.py \
  --elf build/nov14-next6/spritecache-ee/SPRCACHE.ELF \
  --log build/nov14-next6/spritecache-ee/run.log \
  --until 'SPRCACHE6 EE DONE' --timeout 120
python3 -B tools/ps2/run_pcsx2.py \
  --elf build/nov14-next6/spritecache-ee/SPRCACHE-NEGATIVE.ELF \
  --log build/nov14-next6/spritecache-ee/negative-run.log \
  --until 'SPRCACHE6 EE FAIL' --timeout 120
```

Reports, extracted sources, EE ABI constants, complete objects/assembly and
raw logs are under `build/nov14-next6/spritecache{,-asan,-objects,-ee}`.
Frozen production hashes are `spritecache/production-sha256.json`.
