# PS2 Hardware RAM and visibility, pass 12 — 2026-11-15

This pass removes redundant light-table storage and reuses batching scratch
after its previous consumer finishes. Exactly six of the 602 source files
change. Texture resolution, lighting precision, draw order, physics,
simulation, audio, compatibility paths and resource limits are retained.
New distance cutoffs, entity removal and lower-quality graphics modes are
not introduced.

The tested production source is
`d4fbc7c319f19edc70eda220c3323d0350504c98`. The preceding published pass is
`8729f3652629286c0f38b4c203018e3bdeaa68be`, built from source
`85bd9716b1ed114d46f95831c64a87623038eeaa`. Its Hardware ELF SHA-256 is
`fc7a2919a519cc5b23be4ded8e295df41fda68c90350326cfa1113b63e46eff5`.
The complete source manifest and isolated replay are
`tools/ps2/next12_baseline.json` and `prepare_next12_baseline.py`.

## Retained changes

The PS2 light-table getter no longer retains a 32,768-byte RGBA expansion
of each 8,192-entry engine colormap. The driver already converts that
expansion back to canonical palette indices. It now accepts the original
indices and the same forward palette, preserving the complete RGB roundtrip,
lowest duplicate-color index, misses, generations, updates and flush order.
The original generic RGBA APIs and driver-table layout remain available.
Packed RGBA inputs retain alignment one; the actual EE implementation uses
the appropriate unaligned loads. Details, ownership and failure paths are in
[the light-table report](PS2_LIGHTTABLE_NEXT12_20261115.md).

The batching planner finishes using its pairs before the draw walk needs
its index array. One full-width `UINT32` allocation now serves both phases;
the walk borrows its prefix. Original allocation growth, entry limits,
sorting, failure recovery and VU/DMA ownership are preserved. The separate
walk allocation disappears. This is scoped to the actual PS2 profile path;
the opt-out and other platforms retain the old storage. Details are in
[the scratch-lifetime report](PS2_PB_SCRATCH_NEXT12_20261115.md).

The changed files are `src/hardware/hw_pbatch.inc`,
`src/hardware/hw_cache.c`, `src/ps2/hw/ps2_hw_pal.inc`,
`src/ps2/hw/ps2_hwd.c`, `src/ps2/hw/ps2_hwd.h` and
`src/ps2/hw/ps2_hwd_null.c`. No source outside those six differs from the
preceding accepted source. Narrow opt-outs remain available for direct
whole-engine attribution with the other optimization active.

## Measured memory and graphics

The final shipping Hardware ELF is
`efc963c7876a5f2a1c395baf5ba9982b68c6db1e56811d7a5e8a690ba840ab20`.
Both predefined local seeds reproduce the same following results after 350
native Hardware frames. Six fresh executions and two exact reused primary
baselines form four within-seed comparisons:

| Scene | Arena, unchanged | Allocated / peak / global peak, before → final | RAM freed | Failed allocation attempts, before → final |
| --- | ---: | --- | ---: | --- |
| MAP02 | 23,547,904 B | 22,146,048 → 22,071,168 B | 74,880 B | 7 → 7 |
| MAP01 fog/NPOT/stereo | 23,543,808 B | 17,899,648 → 17,827,872 B | 71,776 B | 9 → 8 |

Every pair retains all 407 ordered canonical polygon rows, loader/light-table/
resident-texture streams, complete driver geometry/upload/VRAM/LOD/budget
tails and PB capacity/output counters. Evictions, evicted bytes, libc, stack,
spill peaks/current use and flushes match. Other tag payload/block residency
is exact. Only the intended redundant light-table and separate walk storage
disappear. The failed-attempt policy explicitly accepts a decrease and
rejects an increase; no other capacity/cache field is relaxed.

The scratch payload reductions are 9,216 and 6,144 bytes at the unchanged
18,432/12,288-quadword PB capacities; the two removed RGBA allocations
account for 65,568 allocated zone bytes. The final whole-zone residual is
96 bytes in MAP02 and 64 bytes in fog after those terms, without assigning
a particular header/alignment event. These are distinct from the earlier
component's 32-byte incremental light-table residual. The final
report is `final-hardware-pressure/audit-v1/report.json`, SHA-256
`311272e731058f24589563a4d0f5ca3c93e8fa44153bf7ab0ef99ab6ccb0254e`.
It admits the actual final 91-function PB/table graph union and both staged
diagnostic ELFs. The earlier combined-component ELF has its own separate
evidence; its measurements are not substituted for this final execution.

Elapsed differences are −10/−8 ms for MAP02 and +5/+3 ms for fog, for seed 1/
seed 2 respectively. The fog second-seed cycle difference is also positive.
These observations do not establish an FPS or stability improvement.

These changes free space inside the existing zone. They do not shrink the
reserved zone, reduce the 32 MiB EE configuration, or establish a process-RSS
reduction. The saving scales with live light tables and current batching
capacity; two maps are not a universal bound for all content.

The final image matrix compares all 13 complete 320×200 PPM pairs and loader
hashes, including fog, NPOT sprites, lighting, stereo requests, Software
MAP11 and its later pressure frame. Every byte matches. The 175-frame
Software stress has identical non-time zone/cache/libc/stack/spill fields.
The six baseline roles are exact copies of accepted preceding-pass runs of
the same actual baseline diagnostic ELF. Their original commands, tool pins
and runtime records remain unchanged; `baseline-reuse-origin.json` explains
this reuse. They are not described as fresh emulator executions.

Ordinary shipping ELF smokes retain OS-seeded local RNG. Four fresh paired
runs compare six complete PPM pairs and full saved configurations. Their
late `I_ShutdownSystem(): end of logstream.` marker follows config save and
sound/graphics shutdown. It does not prove completion of the subsequent
`W_Shutdown`/`PS2Boot_Exit`. Timing and RAM claims do not use these
uncontrolled-RNG smokes.

The first smoke analysis incorrectly also required `ZQUIT DONE`.
`Impl_VidShot` already calls `I_Quit` after frame 140, before the requested
175-frame timer. All four late markers and completed atomic saves were
present, and all other checks passed. The original failed report/tool and
runtime evidence remain unchanged. `next12_smoke_report.py` explicitly
corrects only those four false conditions and independently rechecks the
files, profile and actual exit call witnesses. Two analysis-development
failures are also retained. No game rerun or generated-file repair was used.

## Visibility and VRAM decisions

The existing renderer already uses BSP visibility, frustum/azimuth clipping,
cached-polygon, sprite, model and shadow rejection. Additional visibility
work was investigated, including a late transparent-wall guard after both
sorts. Removing nodes earlier can change plane sorting barriers; skipping
texture selection can also change later transparency classification.
Even a fully invisible known texture contributes to working-set and
eviction feedback. Executing the real planner code demonstrates different
future visible-texture budgets when that draw is omitted. This candidate
is rejected; see [the visibility report](PS2_VISIBILITY_NEXT12_20261115.md).
No arbitrary far-distance or entity cutoff is substituted for it.

The P4 experiment finds exact 16-color-band texture candidates in stock
content. Complete host texel and CLUT checks pass, but actual planner
counterexamples show that a larger available budget can produce coarser
levels for other visible images. Lossless texel packing alone therefore
does not prove lossless game rendering. No P4 upload, palette-cache or
allocator change is shipped. VRAM occupancy and texture-policy behavior
are retained; this pass claims no new VRAM saving.

A separate design keeps the original logical T8 accounting and compacts
physical P4 blocks. Its fit/move arguments are documented, but actual ABI
padding, full-width owner IDs, cached addresses, source-cache feedback,
fences and worst-case GS movement costs remain unresolved. It is deferred
research, not a proof of impossibility or an accepted optimization. The
earlier 64 KiB duplicate-allocator estimate is explicitly not a necessary
lower bound. See [the P4 report](PS2_P4_NEXT12_20261115.md).

## Build and binding evidence

Fresh default builds contain all 189 Software C inputs and all 203 Hardware
C inputs plus two VU objects, with `-G8 -O2 -flto`, the original features and
shipping allocator wrappers. No padding, reserve adjustment, limit change,
fast-math option or rounding change is used to recover memory capacity.

Both final `.text` sizes and both `_end`/`_gp` values exactly equal their
own renderer baselines. Hardware `.sbss` is eight bytes smaller; the linked
end is unchanged. The size utility also reports 16 fewer read-only metadata
bytes per mode. That version-note change is not a code or CPU saving.

The final PB proof reconstructs 65 live functions in each matching
final/control ELF, complete referenced data/BSS, exact merged-literal
addresses, caller targets, ordered RELAs/addends and frame/save witnesses.
Its 19 internal binary/routing controls pass. Final light-table and allocator
proofs bind the actual whole-engine helper/caller/CLUT/wrapper graph, SDK
archive members and both diagnostic ELF copies; 22 graph and five allocator
controls pass. Exact retained-RELA replay links preserve every original
allocated byte, address, alignment, capacity and defined symbol.

Audio retention uses each renderer's own baseline: 42 input code sections,
215 ordered RELAs and all 42 PDR records per mode are retained; the 21 live
bodies per mode are resolved into the final ELF and the 21 discarded bodies
are identified explicitly. Six internal controls cover actual loads,
constants and addends. An initial cross-renderer pooled-constant comparison
is preserved as a corrected observation, without relocation normalization.

Controlled runs use diagnostic-only local seeds `0x01b2d3f5` and
`0x9e3779b9`, selected before execution. Only the original 212-byte OS-seed
initializer extent changes in those copies; every other ELF byte, symbol
address and size is checked. The published ELFs retain their normal OS RNG.
`-singletics` alone does not fix local weather/audio RNG.

Every actual emulator run uses the official wrapper with complete before/
after INIs, binary hashes and frozen inputs. All non-UI settings must match;
no alternate emulator or graphics setting is used for a better comparison.
Three preexisting unused warnings remain in two unchanged Software units;
Hardware has no compile warnings. Existing SDK abicalls and serial-LTO
notices remain recorded. The light-table opt-out's serial partition count
changes from 70 to 68 when its APIs are omitted; that sole diagnostic admission
is explicit and its first rejected analysis is preserved.

The standard GS suite retains its known failure: CT32 55/56 and CT16S 50/51,
each with underlying return code 1. The VU0 witness is 49,152 vertices,
24,625 outcode differences, maximum dX/dY/dZ 0 and relative 1/w error 0.
This is not an all-green conformance result. All 17 complete CT32 readbacks
must match the preceding published hashes.

The retained GS allocator/upload proof also compares the correct preceding
Hardware ELF directly with the final one: 11 low-level code/42 data sections
and 541 ordered RELAs, plus seven upload code/15 data sections and 86 RELAs.
The complete allocator bodies, capacities, GP targets, literals and aliases
are retained. The final actual resolved graph includes the real memory
wrappers and SDK bodies. It makes no new native timing claim.

The final fresh GS suites are under `gs-integration-retry1`; both underlying
returns remain 1 and all 17 readbacks match. Each runtime archives all 139
input files and unchanged non-UI settings. An initial environment error
never booted a game. The subsequent completed attempt executed at the
original output paths and was later moved to an unpublished archive; it
is not relabeled as execution at its archive location. The fresh final
suites use checked byte-identical fixture/build copies with an explicit
origin manifest. `gs-integration-relocation-manifest.json` records all three
stages and the moves/copies. No pre-move whole-tree manifest is invented.

## Research, provenance and release

[The architecture catalog](PS2_ARCHITECTURE_AND_OPTIMIZATION_SOURCES_20261115.md)
contains 47 checked primary entries covering EE, caches, MMI, DMA, VIF, GIF,
VU0/VU1, GS layouts, CLUTs, synchronization, IOP, SPU2 and developer
optimization papers. [Compact public research evidence](ps2-research-20261115/README.md)
records checks for 55 external URLs and the port source map. This is a broad
search; it does not claim to enumerate all Internet documentation or every
possible optimization. Manuals, BIOS and game assets are not redistributed.

Earlier isolated failures remain evidence: the first scratch launch lacked
the emulator environment and never booted; light-table v1 reduced arena
capacity and was rejected; v2b exposed a null-backend guard problem before
the final v2c policy checks. The combined component's MAP02 output config
was interrupted after the measurement marker, while its complete staged
input config remained exact. Final normal smokes independently check a late
post-save marker and complete saved output.

Early P4 development output was overwritten before it was archived. That
provenance gap is explicit; the later complete immutable `corpus-v1` stage
has its own manifest and independent replay. It is not relabeled as the
original development artifact. The final report also retains the unchanged
earlier review and its supersession notice.

The release directory is
[`prebuilt/lossless-next12-20261115`](../prebuilt/lossless-next12-20261115/README.md).
It contains the two original shipping ELFs, SHA256SUMS and compact validation
pins. Full builds, raw runs and detailed proofs remain under
`build/nov15-next12`. Physical-console RAM, GPU load, energy use, weighted
FPS and frame-time stability have not been measured. Individual emulated
elapsed/cycle differences do not establish an FPS improvement.
