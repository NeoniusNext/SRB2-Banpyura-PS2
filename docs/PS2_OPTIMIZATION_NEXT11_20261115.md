# SRB2 PS2 exact optimizations — eleventh pass, 2026-11-15

This pass reduces repeated loads and address calculations in the Software
renderer's visplane overlap scan. Five GPT-6.1 Sol subagents at max reasoning
handled Software, memory, GS, I/O and an independent review. Only
`src/r_plane.c` is retained as a production change. Previous optimizations,
all graphics, physics, precision, features and resource limits remain.

The reference is `69a3e5319a0a2ee0afda9c460c4a158139d221dd`, whose production
source is identical to `e87421615d0271dcf9074656c513f42d0ea1028d`.
`tools/ps2/next11_baseline.json` freezes all 602 source files and the empty
source patch. The retained source revision is
`85bd9716b1ed114d46f95831c64a87623038eeaa`; exactly one of those files differs.
Both `r_defs.h` and `p_slopes.c` are restored byte for byte.

## Retained change

[`R_CheckPlane`](PS2_SOFTWARE_NEXT11_20261115.md) retains top and bottom
strip cursors after the first accepted top sample. It avoids repeatedly
loading the bottom pointer and converting the column index to a byte offset.
The inclusive scan, top-before-bottom read order, reversed/empty intervals,
shared strip contents, first rejected sample and final metadata stay exact.
Allocation, reuse, copy, reset and renderer arithmetic retain their preceding
source. There is no new cache, allocation, precision change or reduced bound.
The cursor scan is limited to Software builds with `NOHW`. Hardware,
including its Software fallback, retains the original scan.
`PS2_NOOPT_CHECKPLANE_CURSOR` also selects the original scan, as do the
existing renderer/full opt-outs and non-profile builds.

| Native EE matrix | Faster / equal / slower | Summed per-call cost reduction |
| --- | ---: | ---: |
| Warm | 166 / 30 / 0 | 13.101835% |
| Cache invalidated | 166 / 30 / 0 | 13.113686% |
| Both, equally weighted | 332 / 60 / 0 | **13.107758%** |

These are selected recycled-plane scan/helper costs, including identical
re-prime and dispatch work. Timed allocation/free hooks are zero. They do
not measure the zone allocator, weighted game FPS, frame-time variance or
a physical PS2. The native exactness corpus compares 17,026 cases and
222,836,288 complete state bytes. Each host policy and ASan/UBSan compares
194,434 cases and 2,970,951,520 bytes; all fifteen executed source mutations
are detected.

Actual release `-O2`/`-G8`/full-LTO helper code is bound to the executed
fixture, including complete instructions, register allocation, ordered RELA
targets and explicit addends, exact frames, initialized data, literal addresses
and resolved final ELF bodies. The complete Software `R_StoreWallRange` caller is
21,096 B with 604 ordered RELAs and a 336 B frame. Hardware retains its
own original 21,072 B caller with 609 ordered RELAs and the same frame size. The real recycled-plane
dependency and canonical `memcpy`/`memset` wrappers are included. Software uses its current-header old-scan control and real data capacities.
Hardware retains the complete original helper and its own original caller,
data and frames, bound to the executed reference fixture. No Hardware gain
is inferred from the Software measurement.
Ambiguous merged `"0"` strings use a separately verified exact-link
`--emit-relocs` replay; a different identical string address is rejected.

The Software helper grows 1,256 → 1,272 B; its frame remains 48 B. Software
linked text grows **16 B**, while data and BSS sizes stay equal. Hardware
uses the original 1,256 B helper; every allocated section size and alignment,
its ELF length, `_end` and `_gp` retain their reference values. Software
`_end`/`_gp` and ELF length also retain their reference values.
There is no new heap, stack, writable static storage or VRAM reservation.
This pass does not claim a net RAM or VRAM saving.

## Rejected experiments

Passing component exactness did not suffice for adoption. The controlled
MAP11 primary-seed 350-frame reference has 19,606 evictions, 849,579,632
evicted bytes and 2,513 failed allocation attempts. All effects and sound stay
enabled in these comparisons.

| Candidate as built | Evictions | Evicted bytes | Failed attempts | Elapsed ms |
| --- | ---: | ---: | ---: | ---: |
| Reference | 19,606 | 849,579,632 | 2,513 | 106,884 |
| 112 B slope, 64 slots | 20,834 | 920,223,680 | 2,663 | 115,557 |
| 112 B slope, old 8 KiB byte budget | 20,853 | 920,400,560 | 2,664 | 115,657 |
| PAK header overlay only | 21,495 | 962,734,224 | 2,868 | 117,334 |
| Scan only, original slope storage | 19,606 | 849,579,632 | 2,513 | 106,891 |

[Slope storage](PS2_MEMORY_NEXT11_20261115.md) preserves every numeric field
and pointer offset while moving two byte fields into an alignment hole.
The real EE type is 120 → 112 B. The first 64-slot variant saves 33,792 B
at the measured slopes checkpoint. One bounded follow-up uses the original
8 KiB payload budget for 73 complete records, saving 33,296 B at that
checkpoint. Both nevertheless worsen cache reconstruction in the game and
are rejected. The scan-only counterfactual restores every reference memory,
cache, libc, stack and spill metric. Its observed +7 ms is recorded; it does
not demonstrate a whole-game speedup. No additional memory variants were
adopted. The first stage's three native witnesses ran; the follow-up's four
native witnesses were built but **not executed** after its game gate failed.

The [PAK header overlay](PS2_IO_NEXT11_20261115.md) would save 768 B of
fixed BSS, with 208 B additional selected text and greater late-probe local
stack depth. Its isolated actual default-LTO Software candidate alone fails
the matched game gate. Production `w_pack.c` was never edited. The old
unseeded pass-9 causal wording is explicitly withdrawn without altering
historical measurements.

The [GS palette swizzle](PS2_GS_NEXT11_20261115.md) preserves complete palette
and packet bytes, but its final native matrix contains twelve slower supported
rows, including six normal tint rows. It is rejected despite improvements
elsewhere. All 34 GS source files match the reference exactly. Prior GS
allocator and full texture-upload optimizations remain separately bound.

## Hardware image capacity control

The initial `761efcda8` candidate applied the scan in both builds. Although
it passed all thirteen complete-frame comparisons, Hardware's extra 16 B
of text moved `_end` and `_gp` by 128 B. In the fog/add-on scene the existing
4 KiB arena-sizing granularity yielded a **4,096 B smaller arena**, with
3,968 B more libc availability. MAP02 kept its arena size. This is a real
capacity/footprint tradeoff, not a RAM saving; lower used-zone samples did
not excuse it. Individual pre-arena allocation events were not logged, so
an exact allocation-event cause is not claimed.

That Hardware version is rejected. The bounded correction selects the cursor
scan only with `NOHW`, keeping Hardware's original scan and all its fallback
features. The complete builds from `85bd9716b` restore both renderer image
end/GP values. Fresh actual release runs reproduce the Hardware reference
arena and every non-time checkpoint metric, including largest free block.
All thirteen complete images match. Original candidate builds and traces
remain under `build/nov15-next11/final*`; actual release builds and runs use
`build/nov15-next11/release*`. Rejected artifacts are never relabelled as
shipping executions.

## Release validation

Final runtime results, exact ELF hashes, detailed report hashes and known
limitations are recorded in
[`validation.json`](../prebuilt/lossless-next11-20261115/validation.json).
The release directory contains complete Software and Hardware game ELFs,
checksums and a Russian README. It contains no game assets or BIOS.

Comparisons use ordinary 32 MiB EE RAM, unchanged EE cycle settings and
`EnableEECache=false`. Only the official locked emulator runners are used;
at most two EE compiler workers and one emulator run concurrently.
`next11_runtime.py` records every input hash, full before/after INI text,
every non-UI setting and the actual emulator binary around each game run.

`-singletics` fixes gameplay `P_Random`, not local weather/audio `M_Random`.
Diagnostic copies therefore use the two preselected local seeds
`0x01b2d3f5` and `0x9e3779b9`. Only the original 212 B entropy initializer
is replaced by a checked 40 B stub and 172 B of NOP fill. The stub calls
the game's unchanged real `M_RandomSeed`; every other ELF byte, symbol,
address, size and SFC32 consumer stays unchanged. Frames, seed immediates,
the actual JAL target and clobber set are checked, with executed binary
mutation controls. Published game ELFs keep normal OS random initialization.

All **13 complete PPM pairs** and all six loader-hash pairs match. Every
non-time field at all fifteen checkpoints also matches in each scenario,
including both Hardware arenas at 23,547,904 B. Six actual release scenes
are fresh; the six reference scenes reuse the exact preceding strict baseline
executions, with original commands, tool versions, inputs and full INI archives
preserved. The independent audit checks those original tool bytes rather than
substituting a subsequently edited checker.

The 175-frame trace and both preselected 350-frame traces retain all fifteen
arena/cache/libc/stack/spill fields exactly:

| Scenario | Evictions, both | Evicted bytes, both | Failed attempts, both | Elapsed ms, reference → release |
| --- | ---: | ---: | ---: | ---: |
| Primary seed, 175 frames | 9,801 | 464,359,296 | 1,020 | 59,440 → 59,449 |
| Primary seed, 350 frames | 19,606 | 849,579,632 | 2,513 | 106,884 → 106,883 |
| Secondary seed, 350 frames | 20,367 | 886,396,816 | 2,671 | 111,190 → 111,196 |

These near-zero elapsed differences are observations, not an established
whole-game FPS gain. All frames and loader hashes also pass in two separate
unmodified normal-RNG game smokes. Their Software run stopped after the full
temporary configuration write but before its atomic rename. All 8,860 actual
temporary bytes equal the complete reference config. The checker initially
assumed the final filename already existed; its correction validates complete
literal bytes without renaming or fabricating evidence. The archived original
checker and its successful actual Software run remain intact; only the missing
Hardware smoke was then executed. This is a checker recovery, not a game fix
or a repeated run selected for a favorable timing result.

The Software full rebuild repeats three existing unused warnings in two
unchanged units. Hardware has no compile diagnostics. Final links have no
new diagnostics, with the known SDK abicalls warning recorded separately.
The standard GS suite's historical VU0 failure remains an explicit nonzero
underlying result: CT32 55/56 and CT16S 50/51. It is not an all-green suite.
All seventeen complete CT32 readbacks match the preceding release, and both
underlying suite returns remain exactly one.

## Reproduction

Recreate the source reference with `prepare_next11_baseline.py`, then build
complete default Software/Hardware engines using [TOOLCHAIN.md](TOOLCHAIN.md).
Keep each build's original IR graph, generated configuration, engine LTO
object, map and ELF together. Final frozen source and complete builds are
under `build/nov15-next11/release/{source,software,hardware}`; rejected stages
remain in their separate directories. Never mix objects from different stages.

```sh
source /workspace/.onboarding/environment.sh
python3 -B tools/ps2/prepare_next11_baseline.py
python3 -B tools/ps2/next11_sw_checkplane.py --help
python3 -B tools/ps2/next11_sw_checkplane_objectcheck.py --help
python3 -B tools/ps2/next11_sw_checkplane_context.py --help
python3 -B tools/ps2/next11_sw_checkplane_codecheck.py --help
python3 -B tools/ps2/next11_sw_linker_relas.py --help

python3 -B tools/ps2/next11_local_rng.py \
  --engine build/nov15-next11/release/software/obj/engine_lto.o \
  --elf build/nov15-next11/release/software/SRB2.ELF \
  --map build/nov15-next11/release/software/SRB2.map \
  --out build/next11-replay/rng/software --assemble --negative-controls
```

Generate separate corresponding copies for the reference and Hardware using
the same seeds and assembled templates. Supply their explicit paths to
`next11_integration_check.py` and `next11_pressure_check.py`; choose new output
directories rather than replacing prior evidence. `--analyze-only` is read-only.
Use `next11_smoke_check.py` for unmodified game ELFs and
`next11_gs_integration.py` for the recorded standard GS signature and readbacks.
Component documents give the exact replay commands for rejected candidates;
their checked overlays never modify production sources.
