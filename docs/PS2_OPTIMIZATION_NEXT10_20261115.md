# SRB2 PS2 exact optimizations — tenth pass, 2026-11-15

This pass reduces work in visplane bounds, resource-cache lookup and the
GS free-range list. A smaller drawseg layout was rejected after controlled
whole-game cache-pressure tests. Four
GPT-6.1 Sol subagents at max reasoning handled those components; a fifth
independently audited their source, actual EE code and recorded results.
Previous optimizations remain in place.

The reference is `5134b87aeb382a7e156acb8a85462bd9059c096a`, with all 602
source files frozen by `tools/ps2/next10_baseline.json`. The implementation
candidate was `e7131dc7feca977eb62abb74a9df118e0de242cc`. The retained source is
`e87421615d0271dcf9074656c513f42d0ea1028d`; exactly five source files differ.
`r_defs.h` is restored byte for byte.

## Component results and limits

| Change | Measured result | Scope |
| --- | --- | --- |
| `R_PlaneBounds` | **7.504122% lower summed emulated EE per-call cost**, 154 faster / 42 ties / zero slower in 196 rows | Complete actual default-LTO helper 104→96 B; frame remains zero |
| `CheckLumpInCache` | **7.311075% lower summed emulated EE helper cost**, all 350 rows faster | Complete cache/cursor storage and frame unchanged; helper section grows 328 B |
| GS high-allocation/free list | **47.10614% lower summed selected helper cost**, 90 faster / 54 ties / zero slower in 144 rows | Complete list capacity, fitting order, VRAM addresses and all 4 MiB of GS memory remain |
| Complete low-allocation texture upload | **6.089279% lower summed selected cost**, all 96 rows faster | Actual 11-node code graph, 42 data nodes and memory wrappers; successful uploads and controlled diagnostic-boundary failures reported separately |

The GS high/free and complete-upload figures measure separate matrices and
must not be combined. These figures are not weighted game FPS, frame-time
variance or physical-console
measurements. No reduction in texture quality, precision, physics, geometry,
filtering, voices, rates, sampling, Lua, network, add-on support or resource
limits is proposed. VRAM capacity and DMA/GIF draw-work do not decrease.

No drawseg RAM saving is retained. Software linked text grows by 320 B and
Hardware by 56 B; writable data and BSS stay unchanged. The free-list helper's
frame falls 64→48 B, while the high-allocation frame stays 64 B. These stack
and code figures are not a reduction in whole-game peak RAM. Payload, rounded
zone blocks, fragmentation, libc requests, spill and process occupancy are
distinct quantities and must not be added.

## Exactness and target binding

[The rejected drawseg candidate](PS2_MEMORY_NEXT10_20261115.md) replaces only the two
temporary endpoints with the existing eight-byte `vector2_t`. Every one of
their 18 consumer lines accesses only full-width `.x` or `.y`. Persistent
`vertex_t` stays 20 B with all heights and flags. Actual allocator/growth,
pointer repair, slope intersections, Lua fields, polyobjects and 85 stock
maps are covered by 201 host and native cases and **38,152,928 literal bytes**.
Opt-out restores every baseline allocator row and the complete witness ELF.
Correctness alone was insufficient: with identical local seed and sound
payload, the compact candidate added 2,371 evictions, 109,404,512 evicted
bytes, 719 failed allocation attempts and 11,281 ms in the 350-frame trace.
Restoring only the 444-byte drawseg layout restored every reference zone/cache
metric while retaining the CPU and GS changes. Its 24 B per-record saving
therefore remains a rejected component result, not a shipped RAM saving.

[Bounds reduction](PS2_SOFTWARE_NEXT10_20261115.md) uses EE word min/max on
zero-extended halfwords. The original loop, initial reads and final writes
remain. Explicit old register clobbers preserve the complete actual caller.
Host policies and sanitizers each compare 78,600 cases and 213,792,000 bytes;
the final native corpus compares 1,448 cases and 3,892,224 complete bytes.
Every reversed, zero and one-column timing row ties. Faster countdown
formulations were rejected because they slowed two-column spans.

[Resource lookup](PS2_IO_NEXT10_20261115.md) scans the original 64 hashes in
two contiguous descending spans after the current-slot fast path. Full
comparison semantics and duplicate precedence remain. Normal and sanitizer
runs each pass 312,394 checks, including every cursor/hit pair, collisions
and a protected nonterminated short query. The two earlier loop variants
were rejected for slower first-hit, shallow or collision rows. The retained
wide local index removes redundant truncations; persistent indices remain
16-bit. The narrow opt-out reconstructs the complete original engine object.

The GS change stores each existing 16-bit start/length pair as one aligned
four-byte record. List shifts copy pairs together. There is no capacity,
alignment-policy, fit-order, eviction or memory-size change. Host and sanitizer
each compare 52,637 complete allocator states and 3,884,189,504 state bytes.
The complete high/free EE helpers and SDK `memmove` are bound to the timed
fixture; complete texture-upload coverage is recorded separately.

Full default-LTO objects are used for CPU adoption. Instructions, register
choices, branches, frames, ordered exact-symbol RELAs/addends, literal data
and fully resolved final helper bodies are checked. Both final bounds-caller
comparisons use the restored 444-byte drawseg ABI. Software uses the recorded
old-bounds/old-cache control; Hardware uses its original Hardware reference,
including its 68-byte `vid` object. Each gate directly checks the entire
7,608-byte caller, all 143 ordered RELAs, its 400-byte frame and every referenced
literal/capacity. There is no cross-mode data-size normalization or inference
from unrelated source changes.
Audio production sources remain unchanged and have a separate complete
Music/MIDI object-graph retention audit.

## Integration and artifacts

The initial unseeded pressure comparison was not a controlled workload.
`-singletics` fixes the network/gameplay `P_Random` seed, while local
`M_Random` is initialized from OS clocks. Rain positions and ambient thunder
variants still use that local generator. In the recorded runs, DSATHUN1 and
DSATHUN2 produced different 331,330/331,680-byte sound-cache payloads. The
unseeded matrix is retained as observations; its causal conclusions were
withdrawn. A first matched-seed comparison still reproduces the combined
cache regression, so it cannot be dismissed as only the random sound.

`next10_local_rng.py` creates diagnostic ELF copies that call the game's
unchanged `M_RandomSeed` with two preselected seeds, `0x01b2d3f5` and
`0x9e3779b9`. Only the existing 212-byte `M_RandomSeedFromOS` symbol changes:
a 40-byte EE-assembled stub and 172 bytes of NOP fill. Every other ELF byte,
symbol, address, size and actual SFC32 seeding/generator instruction stays
unchanged. The ABI, exact JAL target, seed immediates, outgoing argument area,
stack/RA restoration and original caller clobber set are independently checked.
Both weather and sound remain enabled. Published game ELFs retain their
original entropy initialization; no production RNG source changes.

The final 12 controlled scene runs pass: all **13 complete PPM pairs** match
literally, loader hashes match and all five map-loaded zone deltas are zero.
They use fresh baseline/current fixed-local-seed copies on **32 MiB**. NPOT
slopes/FOFs, fog, translucent drawing, MIDI and stereo requests remain enabled.
The primary 350-frame reference reuses the earlier verified run of the exact
same diagnostic ELF/seed/arguments/assets; the scene baselines are fresh.

Both preselected seeds pass the complete 350-frame trace. All 13 named
arena/cache/stack/spill fields match their respective references exactly:

| Scenario | Reference evictions | Final evictions | Reference/final evicted bytes | Reference/final failed attempts | Elapsed ms, reference→final |
| --- | ---: | ---: | ---: | ---: | ---: |
| Primary seed, 175 frames | 9,801 | 9,801 | 464,359,296 | 1,020 | 59,451→59,440 |
| Primary seed, 350 frames | 19,606 | 19,606 | 849,579,632 | 2,513 | 106,885→106,884 |
| Secondary seed, 350 frames | 20,367 | 20,367 | 886,396,816 | 2,671 | 111,201→111,195 |

Arena capacity remains 24,743,936 B. Controlled libc free/peak are 384 B lower
because of the larger Software code image; this is an explicit footprint
tradeoff, not RAM savings. These traces do not establish physical-console
FPS or frame-time variance. Raw 32-bit CP0 counts may wrap and are not used
as a direct FPS calculation.

Unmodified published ELFs also have separate Software/Hardware smoke runs.
The standard GS suite retains the exact historical VU0 failure, with its
nonzero underlying exit status: CT32 **55/56**, CT16S **50/51**. All **17**
complete CT32 readback hashes match the preceding release. The suite is
explicitly not all green. The Software header rebuild repeats three existing
unused warnings in two unchanged units; Hardware and final links have no
new diagnostics.

Source/proof tools are committed separately from raw `build/nov15-next10`
evidence. Game data and BIOS are not published. Final release files and
compact validation metadata will be in `prebuilt/lossless-next10-20261115`.

## Reproduction

First export the baseline with `prepare_next10_baseline.py` and build complete
baseline/current Software and Hardware engines with [TOOLCHAIN.md](TOOLCHAIN.md).
Keep the same SDK, flags and linker settings; use equally long isolated source
paths when comparing image/heap layout. Complete objects, ELF and map are
required. The measured immutable directories below are retained under `build/`;
rebuilds may have timestamp/metadata differences, so use their own matching
object/ELF/map rather than mixing artifacts. Nothing below edits game sources.

```sh
source /workspace/.onboarding/environment.sh
python3 -B tools/ps2/next10_local_rng.py \
  --engine build/nov15-next10/baseline/software/engine_lto.o \
  --elf build/nov15-next10/baseline/software/SRB2.ELF \
  --map build/nov15-next10/baseline/software/SRB2.map \
  --out build/next10-replay/rng/base-sw --assemble --negative-controls
python3 -B tools/ps2/next10_local_rng.py \
  --engine build/nov15-next10/baseline/hardware/engine_lto.o \
  --elf build/nov15-next10/baseline/hardware/SRB2.ELF \
  --map build/nov15-next10/baseline/hardware/SRB2.map \
  --out build/next10-replay/rng/base-hw \
  --template-dir build/next10-replay/rng/base-sw --negative-controls
python3 -B tools/ps2/next10_local_rng.py \
  --engine build/nov15-next10/final/software/obj/engine_lto.o \
  --elf build/nov15-next10/final/software/SRB2.ELF \
  --map build/nov15-next10/final/software/SRB2.map \
  --out build/next10-replay/rng/new-sw \
  --template-dir build/next10-replay/rng/base-sw --negative-controls
python3 -B tools/ps2/next10_local_rng.py \
  --engine build/nov15-next10/final/hardware/obj/engine_lto.o \
  --elf build/nov15-next10/final/hardware/SRB2.ELF \
  --map build/nov15-next10/final/hardware/SRB2.map \
  --out build/next10-replay/rng/new-hw \
  --template-dir build/next10-replay/rng/base-sw --negative-controls
source /workspace/.onboarding/emulator-env.sh
python3 -B tools/ps2/next10_integration_check.py \
  --out build/next10-replay/seed1 \
  --baseline-sw build/next10-replay/rng/base-sw/DIAGNOSTIC-LOCAL-RNG-01b2d3f5.ELF \
  --baseline-hw build/next10-replay/rng/base-hw/DIAGNOSTIC-LOCAL-RNG-01b2d3f5.ELF \
  --software build/next10-replay/rng/new-sw/DIAGNOSTIC-LOCAL-RNG-01b2d3f5.ELF \
  --hardware build/next10-replay/rng/new-hw/DIAGNOSTIC-LOCAL-RNG-01b2d3f5.ELF
python3 -B tools/ps2/next10_pressure_check.py \
  --out build/next10-replay/seed1 \
  --baseline build/next10-replay/rng/base-sw/DIAGNOSTIC-LOCAL-RNG-01b2d3f5.ELF \
  --software build/next10-replay/rng/new-sw/DIAGNOSTIC-LOCAL-RNG-01b2d3f5.ELF
python3 -B tools/ps2/next10_pressure_check.py \
  --out build/next10-replay/seed2 \
  --baseline build/next10-replay/rng/base-sw/DIAGNOSTIC-LOCAL-RNG-9e3779b9.ELF \
  --software build/next10-replay/rng/new-sw/DIAGNOSTIC-LOCAL-RNG-9e3779b9.ELF
python3 -B tools/ps2/next10_gs_integration.py --out build/next10-replay/gs
```

Run ordinary smoke checks separately using the unmodified game ELFs, e.g.
`opt_run.py --name smoke-sw --elf prebuilt/lossless-next10-20261115/SRB2-PS2-Software.ELF
--pak build/pak --out build/next10-replay/smoke --map MAP01 -- -singletics -zquit 175`;
for Hardware select its ELF, MAP02 and `-renderer Hardware`. These ordinary
runs validate startup/rendering and do not claim controlled random workloads.

Use empty run destinations for fresh tests. `--reuse-baseline` checks the exact
ELF, engine arguments, stock assets and scene fixtures; it must refer to the
same fixed-seed diagnostic workload. The new default integration/pressure
paths also select the prepared primary fixed-seed copies. Component documents
specify source, host, target-code and native fixture commands.
Keep the existing emulator wrapper, one emulator and at most two EE compiler
workers globally. Physical-console FPS and frame-time stability still require
measurement on real hardware.
