# Private flat metadata candidate, pass 13 — 2026-11-15

This page preserves the original V1 component evidence. The preferred flat
implementation is [cut V2](PS2_FLAT_METADATA_CUT_V2_NEXT13_20261115.md), selected
for the final canonical build. It passes actual static fit, all eight fresh
pressure cases, and all six complete legacy PPM pairs. Native constructors
confirm N=4,740/4,743 and flat payload savings of 37,920/37,944 bytes. The old
strict all-up rejection and parser-development baseline remain archived.
V1's source and historical measurements below remain distinct.

This isolated RAM candidate removes the two unused wall scale floats from the
private flat table on the default PS2 PB path. It does not change the public
`GLMapTexture_t` type, wall records, pixel formats, texture dimensions, allocation
count, or the up-front table capacity. Production source remains unchanged.

The original V1 candidate is **pending native integration**. The pack census predicts
37,920 fewer table payload bytes; the actual default EE image adds 256 bytes to
`_end`, giving a predicted 37,664-byte net reduction before runtime allocator
accounting. This is not a live game allocation or cache/LOD measurement.

## Frozen implementation

Baseline revision `353cc94103aeb38cef87d26151c779302135d0e9` has the same 602 source
files as tested revision `d4fbc7c319f19edc70eda220c3323d0350504c98`. The actual
canonical Hardware ELF is
`efc963c7876a5f2a1c395baf5ba9982b68c6db1e56811d7a5e8a690ba840ab20`.

[`next13_flats_variant.py`](../tools/ps2/next13_flats_variant.py) recreates the
baseline and candidate under `build/nov15-next13/flats-{baseline,source}-v1`.
Only `src/hardware/hw_cache.c` differs. Its candidate SHA256 is
`35fab374191af39ba02f7ce2b5c375c25007286496a40bfa241fa829754862d1`;
the generated patch SHA256 is
`789a44fb403ea3e65eac843243ed6d20990069f055fc75e94018135976ff15b8`.

The private guard is
`PS2 && PS2_PROFILE && !PS2_NOOPT_FLAT_METADATA`. The guarded implementation
stores one `void *gl_flats` and a captured layout boolean. A load selects the
layout before the original first and second `calloc` calls. Both tables still
allocate all `gl_numtextures` entries together. The getter and destructor use
the captured constructor layout; they do not reparse arguments during access
or destruction.

The original `FreeMapTexture` remains unchanged. The new private flat cleanup
performs the original flat operations in their original order for every slot,
including unused slots: delete the child with `false`, free the child, clear
the child pointer, then delete the primary with `true`. Table destruction
clears the captured layout. Failed PS2 table allocation clears both pointers,
the count, and the captured layout before the existing guard throw.

## Legacy pointer ordering

The earlier batching path uses texture addresses in its state ordering. A
reviewer demonstrated that changing the flat stride from 84 to 76 bytes at
the same root address can reverse coplanar black/gray quads; `GEQUAL` makes the
result visible. Therefore startup `-hwgo` bit `65536` retains the original
`GLMapTexture_t[N]` allocation and stride, including combinations such as
`65600` that also select the old scalar renderer.

`HWR_GCacheParse` runs after the first table construction. The private scanner
reads `myargv` without touching the shared `M_CheckParm` cursor. It matches the
existing parser: the first case-insensitive `-hwgo` wins; a missing next token
or a token whose first byte is `+` or `-` is rejected; a valid token uses the
same `atoi` conversion and unsigned cast. Malformed strings, whitespace,
trailing text, duplicates, and rejected signed tokens retain those semantics.

Review confirmed that default PB rendering returns zero only for an empty
collection. A nonempty default PB collection cannot reach legacy pointer
sorting through a render fallback. The whole-image address change and actual
legacy ordering still require native integration checks.

## Completed evidence

| Check | Result |
| --- | --- |
| Source-extracted host execution | 621 original/candidate comparisons across six build profiles with ASan/UBSan |
| Host scope | Whole constructor/free/GetLevelFlat/regeneration/StealData bodies, canonical command parser, stable primary/twin identities, full metadata snapshots, callback order, zone owners, purge, reload, both table allocation failures, child failure, regeneration failures, original `-hwfbtex`, all unused slots, and 4,740-entry tables |
| Guard paths | Non-PS2, no-profile, and narrow-opt-out production excerpts preprocess identically to the original |
| Source negative controls | Five detected: unsafe legacy compaction, shared cursor mutation, wrong getter stride, changed child deletion policy, and primary deletion before child |
| 32-bit host ABI | Exact-source freestanding i386 binaries compile with 76/84-byte assertions and existing mipmap opt-out 88/96-byte assertions; this kernel cannot execute ELF32, so no i386 runtime claim is made |
| Actual default EE build | One actual `hw_cache.c` O2 IR replacement in the complete 203-C + 2-VU LTO graph; the other 204 IR files remain byte-identical |
| Compiler options | Entire changed-unit `.gnu.lto_.opts` section matches the canonical input; no per-unit optimization override |
| Actual EE header probe | `sizeof(GLMipmap_t)=76`, `sizeof(GLMapTexture_t)=84`, mipmap alignment 4, mipmap offset 0, scale offsets 76/80 |
| Retained linker RELAs | Diagnostic retained-RELA link preserves every normal allocated byte, address, extent, alignment, and original defined symbol |
| Narrow opt-out | Complete allocated object sections and ordered RELAs, all PDR metadata and ordered RELAs, and the entire shipping ELF exactly restore the canonical baseline |
| Strict actual linked binding | 60 baseline / 59 candidate complete shipping bodies, 400 / 401 full referenced data objects, and 1,006 / 1,007 literals; all ordered RELAs, resolved bytes, padding, frames, and capacities checked |
| Binary negative controls | Nine detected inside binding after outer hashes were refreshed, including register, linked instruction, call target/addend, frame, private BSS extent, and exact literal corruptions |
| Independent review | Separate reviewer reran all 621 host comparisons and five mutations and found no additional source lifetime/guard counterexample |

The reviewer also independently reran the strict linked proof and its nine
binary controls. An SDK addendum reconstructs eleven complete SDK bodies from
ten canonical archive members, including the `calloc`/wrapped calloc/Spill and
free routes, with six inner negative controls. The component retained-RELA
report does not contain candidate prelink SDK archive hashes; the addendum
explicitly records that omission and uses the already pinned canonical
archives plus independent exact candidate linked-member reconstruction. It
does not invent a prelink archive pin. The addendum is
`build/nov15-next13/review/flats-sdk-v2/report.json`, SHA256
`ecd1c4c2865d36b6b2e3f1d20141afe210bdbbb213dbffe8a43bdbbb79d03fec`.

The unchanged `HWR_RenderBatchesV2` has a 40,928-byte frame reserved as two
stack adjustments, `-32752` then `-8176`. The flat binder explicitly checks the
sum and normalizes saved-register offsets to the caller's stack pointer. It
retains all complete body bytes and verifies the same exact PDR facts.

The actual image has these costs:

| Quantity | Baseline | Candidate |
| --- | ---: | ---: |
| `.text` bytes | 4,373,952 | 4,374,192 |
| `_end` | `0x008854b0` | `0x008855b0` |
| `_gp` | `0x006732f0` | `0x006733f0` |
| GetLevelFlat frame | 32 | 32 |
| LoadMapTextures frame | 48 | 64 |
| FreeMapTextures frame | 32 | 64 |

The candidate Hardware ELF SHA256 is
`ecb963c787b1c13bb139a7da6abf809a97335944a3af0c7c06b0e3d7ef99bb11`.

[`next13_flats_inventory.py`](../tools/ps2/next13_flats_inventory.py) examines
the actual canonical SRP2 directories, checks their entry order against the
official assets, and verifies each `TEXTURES` body by pack CRC and decoded
length. It counts 1,386 flat lumps, 2,848 single-patch textures, and 506
composites: `N=4740`. The predicted flat table changes from 398,160 to 360,240
payload bytes. This catalogue-derived count is explicitly not a live game
observation.

## Reproduction and remaining gates

Run the tools from the repository root. The context builds require an
explicitly granted serial EE worker; they never run an emulator.

```sh
python3 tools/ps2/next13_flats_variant.py
python3 tools/ps2/next13_flats_hosttest.py --out build/nov15-next13/flats-host-new --sanitize --negative-controls
python3 tools/ps2/next13_flats_inventory.py --out build/nov15-next13/flats-inventory-new
python3 tools/ps2/next13_flats_context.py --out build/nov15-next13/flats-context-new
python3 tools/ps2/next13_flats_context.py --optout --out build/nov15-next13/flats-control-new
python3 tools/ps2/next13_flats_codecheck.py --candidate build/nov15-next13/flats-context-new --control build/nov15-next13/flats-control-new --out build/nov15-next13/flats-binding-new
```

Frozen reports currently reside in `flats-host-v8/report.json`,
`flats-inventory-v1/report.json`, `flats-context-v1/context-report.json`,
`flats-control-v1/context-report.json`, and `flats-binding-v3/report.json`
under `build/nov15-next13/`. The strict binding report SHA256 is
`10303430c5c9fb8f48e2f9e4ce139b99f4ff0ade861e40cd61611f124efafd0b`.

[`next13_flats_native.py`](../tools/ps2/next13_flats_native.py) has prepared
complete actual shipping-LTO constructor/get/free graphs and full private
state for a native callback fixture in `flats-native-v1`. It is not compiled
or executed yet. Its explicit SDK allocation callbacks can test real target
requests and identities, but they cannot establish shipping allocator or
residency behavior: `ps2_spill.c` routes these large tables to ZA first at its
existing 24 KiB threshold. Acceptance still requires the real ZA/spill
payload/free-space gate, default cache/LOD and packet/frame checks under
pressure, and legacy batching order/pixels at actual allocation bases. No
reserves, arena thresholds, texture policy, or precision may be tuned to make
those gates pass.

[`next13_flats_pressure.py`](../tools/ps2/next13_flats_pressure.py) prepares
fresh matched 350-frame shipping-game pressure runs for the default path,
`-hwgo 65536`, and `-hwgo 65600`. The current component plan is
`build/nov15-next13/flats-pressure-plan-v1/readiness.json`; no case has run.
Both roles use `-zspill` and the existing `con_hudlines "0"` setting so changed
numeric console diagnostics do not introduce different HUD glyph-cache work.
The existing spill message calls `I_OutputMsg`, which writes to stdout,
logstream, and `MemTrace` without calling the console glyph path. Its maximum
54-byte line fits the existing 1,024-byte stack buffer.

The read-only pressure inspector requires two adjacent real ZA spill rows
matching the exact bound constructor's full-N wall/flat requests. It verifies
that the second row's cumulative `spill_now` increment equals the flat
payload, infers the native table count from the actual wall request, and
preserves all spill, tag, checkpoint, geometry, material, budget, and LOD rows
for the coordinator's integration gate. Retail 32 MiB capacity, full frame
completion, exact argument order, staged assets, and diagnostic ELF scope are
checked separately. A default-path pair must save 37,920 payload bytes;
legacy pairs must save zero. This establishes a shipping ZA payload fact only
after execution, not acceptance of all RAM, cache, LOD, or image behavior.

The seeded component copy uses the existing audited local-RNG template without
compilation; only the existing `M_RandomSeedFromOS` symbol extent changes.
Normal default-config image checks without `-hwstats` remain a separate
protocol. The coordinator can prepare another fresh plan against a combined
candidate build and its already audited fixed-local-RNG ELF copies.
