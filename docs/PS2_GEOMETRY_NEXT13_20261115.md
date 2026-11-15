# Pass 13 Hardware geometry submission audit

The isolated candidate removes four redundant floating-point reductions from
the existing polygon collection. It adds no visibility rejection, distance
cutoff, entity omission or draw-order change. The isolated v2 native helper
comparisons, the separate clean canonical Hardware binding and the combined
canonical game checks are complete. Their scopes are recorded separately;
the helper measurements establish no whole-game FPS gain.

`HWR_PBPut` initializes its four texture-coordinate extrema from vertex zero,
then used to reduce that same vertex into those extrema again. The v2 snapshot
retains the exact five-word vertex-zero copy and its store barriers, then
starts the unchanged reductions at vertex one. The collected blocks, links,
planner stream, capacities and allocation paths retain their original values.

This uses the private helper's existing `n >= 3` contract. Both polygon entry
paths reject smaller counts before collection; the fused sprite entry supplies
four. The two helper callers preserve the count, including allocation-retry
recursion. The cached-block replay uses its own unchanged writer. There is no
claim of private `HWR_PBPut` equivalence for an unreachable zero-vertex call.
The complete unchanged public polygon function passed 1,536 zero/one/two-vertex
cases across its gates with null and valid pointers: no PB entry or slow-path
sink was reached. Removing its guard in an executed negative was detected.

The EE instruction definitions matter here. Original SCE instruction-set
manual v6 pp363/365 specify that `MAX.S`/`MIN.S` copies one operand unchanged
and clears O/U. Two identical operand bit patterns therefore give the same
result for either comparison outcome, including signed zero and every other
encoding; this does not assume IEEE NaN behavior. Preserved vertex-one
reductions clear the same flags before the same final extent subtractions.
The four complete actual native bodies contain no FCSR read/write instruction,
and their disassemblies retain the same s-extent then t-extent subtraction
order. The reference URL, checked manual hash and full opcode inventories are
in `build/nov15-next13/geometry-fp-proof-v2/report.json`. This is an instruction
semantics argument, not a native exhaustive bit-pattern execution.

The host replay executes source-extracted baseline and candidate writers and
compares every byte of each 8,812-byte result, including the complete output
pool, metadata and host FP flags. All 262,144 legal-count cases pass, including
229,376 overlapping-source cases and explicit zero, subnormal, infinity and
signaling/quiet-NaN encodings. The full production PB/planner/host-packet replay
also retains complete event, packet and memory streams: 49,280 polygons drawn,
37,394 planned, 32 cases and 52 frames, with allocation failures and unwinds.
Both ordinary and ASan/UBSan runs agree. Two executed writer mutations fail.
Host arithmetic supplements the EE proof; its wider pointer/long ABI is not
substituted for the target ABI.

The actual default `-O2 -G8 -flto` Hardware graph uses the accepted 203 C and two
VU inputs, with only `hw_batching.c` recompiled. All 602 source files are
frozen; published `353cc94103aeb38cef87d26151c779302135d0e9` and tested
`d4fbc7c319f19edc70eda220c3323d0350504c98` have identical source. The candidate
Hardware ELF is
`d751875c82820ad35e8010e25762ecb5bb5fed39eba6a6f7c18791de8968f644`.

| Actual native body | Baseline | v2 candidate |
| --- | ---: | ---: |
| `HWR_PBAdd.lto_priv.0` | 1,952 B | 1,920 B |
| `HWR_PBSprQuad` | 1,168 B | 1,152 B |
| `HWR_ProcessPolygon.part.0` | 3,240 B | 3,192 B |
| `HWR_ProcessPolygonSlow` | 5,248 B | 5,224 B |

These are the only four changed instruction bodies; 14 further object entries
change only relocation target-size descriptors. The 36 static self-min/max
sites disappear. `.text` is 120 bytes smaller. Every one of the 5,841 original
procedure records retains its frame/save words. Data addresses, sizes and
capacities, `_end` (`0x8854b0`) and `_gp` (`0x6732f0`) remain exact. This creates
no additional zone arena or measured RAM/VRAM saving.

The strict native binder reconstructs 53 complete functions per role, every
ordered RELA and addend, referenced initialized data/BSS capacity, exact merged
literal addresses, frame/save instruction witnesses, memory-wrapper routing
and three real newlib memory bodies. All 19 binary/routing controls reject
their mutations. The retained-RELA replay preserves every original allocated
byte, address, alignment and defined symbol. These proofs establish binding,
not improved native timing.

The existing `-hwbench2` benchmark measured the actual full-ELF
`HWR_ProcessPolygon` calls, with the same flags and paths. Actual source
triggers it at the 400th batch start; its older 30th-batch comment is stale.
The preparation retains normal shipping ELFs and binds initializer-only seeded
diagnostic copies through an existing assembled template. No `-hwpolyhash` is
enabled, because that flag would select a different public collection path.
The frozen two-run readiness is `geometry-bench-v2/readiness.json`. Two fresh
official-wrapper runs completed 350 MAP02 frames with matching normal-ELF
provenance, complete initializer-only copy binding, unchanged inputs and
unchanged non-UI emulator settings. The reported COP0 helper counts are:

| Real public call | Baseline minimum / mean | v2 minimum / mean |
| --- | ---: | ---: |
| Three vertices | 269 / 278 | 265 / 274 |
| Four vertices | 288 / 299 | 285 / 297 |

Each row covers 64 passes of 1,000 calls, with exactly 1,000 polygons collected
per pass. The complete native cost report is `geometry-bench-v2/report.json`.
These helper counters do not establish a whole-game FPS or physical-console
gain.

The telemetry audit retains all timing, cadence, maximum, scene and audio
rows, including differences. All 20 final non-time resource fields match:
the arena is 23,547,904 bytes and used/peak/global-peak are 22,178,512 bytes.
The 49 resident-texture rows, two light-table rows, eleven memory rows,
eleven scene rows and complete PB/driver geometry, upload, VRAM, LOD and
budget count tails match. Regeneration rows retain their explicit cycle
fields separately. Whole elapsed time is 24,454 → 24,451 ms and final Count
is 3,855,548,261 → 3,854,700,151. Histogram bins and one vblank field differ;
the report makes no stability claim. Canonical polygon/loader hashes and
images were not requested in these helper-cost runs. Their absence is
explicit in `geometry-bench-audit-v1/report.json`.

Evidence remains under `build/nov15-next13/geometry-*`. The v1 source, host
proof, native context and inventory remain unchanged: preserving an unreachable
private zero-count case expanded `.text` by 48 bytes and moved `_end` by 128,
so v2 uses the proven caller contract instead. The initial host harness failed
to compile because its copied shim lacked `hw_model.h`; its original tools,
log and failure manifest remain in `geometry-host-v1`. The first native-bench
readiness is preserved as a superseded preparation with no emulator execution.
An initial launch then rejected a Python-interpreter spelling mismatch after
environment setup, before any emulator/runtime directory was created. Its
separate `geometry-bench-env-preflight-v1` report is retained. The successful
launch sourced both standard environment files and invoked the exact absolute
Python path already frozen in readiness; neither command nor input changed.

Additional invisible-draw removal remains blocked by the next12 sort/material/
planner feedback counterexamples. This candidate retains every original
submission and resource-policy event; it makes no new culling claim.

The selected all-up exposure ELF now has a separate geometry binding at
`build/nov15-next13/combined-classic-v1/geometry-binding-v1/report.json`.
Its SHA-256 is
`ab3e3f163c1bb67b3dadacc6f368f17f6d73178ff348df186fb094474faa370a`.
All 602 files match the four selected overlays: this PB change, scalar drawnode
indices, flat-cut v2 metadata and the classic texture filler. Only the driver
was freshly compiled in that stage; the complete actual dependency checks
justify the three reused variant IR objects and 201 canonical inputs. The
actual ordered 205-input LTO and all four whole input-option sections are
checked, including the driver's original `-O3` override.

This all-up proof reconstructs 53 complete geometry/caller functions per role,
their ordered RELAs, referenced data/literals/capacities, frame/save witnesses
and actual SDK memory bodies. All 19 binary/routing controls reject their
mutations. The four PB producer instruction bodies and PDR words exactly equal
the isolated geometry inputs measured above, and their complete scalar FP
order matches the reviewed operand-selection proof. The selected caller graph
also records the independent drawnode change's 96 → 112-byte frame; it does
not claim every all-up frame is unchanged.

The actual all-up `.text` is 32 bytes smaller and `_end`, `_gp`, data/BSS sizes
and addresses match the accepted baseline. This observation belongs to that
specific exposure ELF and its frozen d4 generated metadata. Its inputs and
proof remain separate from the following clean canonical build.

The exact four selected files are committed in source
`5481bbbdc5aa3167c046836f3e51e4dbb31ad807`. The complete 602-file canonical
snapshot is `build/nov15-next13/final/source/manifest.json`; its bytes match
that commit's actual Git blobs. The clean official Hardware build freshly
compiled all 203 C units and both VU inputs, using their original ordered
205-input graph and actual 5481 commit metadata. Every complete input
`.gnu.lto_.opts` section matches its baseline counterpart, including the
driver's existing `-O3` override. No optimization, padding, reserve or capacity
flags changed. Full input/compiler/dependency/options admission is recorded in
`build/nov15-next13/final/hardware/canonical-build-admission.json` and its
`actual-all-unit-options.json`; the ordinary official `build-report.json`
remains unchanged.

The actual canonical normal ELF is
`7e4764fb609a1b3665936c0594ad4358c4bf1891545f6d4c48da13f5a0e7a9af`.
Its `.text` is 32 bytes smaller and `.rodata` is 64 bytes smaller than next12;
`_end` (`0x8854b0`), `_gp` (`0x6732f0`), and data/sdata/sbss/bss sizes and
addresses are exact. The separate complete PDR inventory retains the four
all-up frame changes: `HWR_CreateDrawNodes` 96 → 112 bytes,
`HWR_FreeMapTextures` 32 → 64, `HWR_LoadMapTextures` 48 → 64, and removal of the
`FreeMapTexture` 48-byte record. The four PB producer frames remain
96/48/0/96 bytes in the table's order. The build's retained-RELA companion
preserves every normal-ELF allocated byte, address, alignment and defined
symbol.

The distinct canonical proof is
`build/nov15-next13/final/hardware/geometry-binding-v2/report.json`, produced
read-only by `tools/ps2/next13_geometry_canonical_bind.py`. It reconstructs
53 complete geometry/caller functions per role, 366 referenced data sections,
119 literals, every ordered RELA/addend, actual frame/save witnesses and the
three complete reached SDK memory bodies. All 19 binary/routing mutations are
rejected after refreshed outer hashes admit the mutated files. All four
canonical PB producer input bodies, including their padding and PDR words,
equal the measured isolated geometry bodies byte for byte. Their complete
scalar FP instruction order equals the reviewed operand-selection proof and
contains no FCSR access; the final actual fully relocated bodies are separately
bound to this canonical ELF.

Both canonical diagnostic copies under
`build/nov15-next13/local-rng/final-hardware` are also bound: seeds
`01b2d3f5` and `9e3779b9` have SHA-256
`95b7f6d534362de126011d1f7cdba72d15d7e6fc0ce22971981c984cee0e182f`
and `26f81d076a9a1033530641355ebc5ced348d2559dac96cc9ba8c2ba207361442`.
Each changes only the authorized 212-byte initializer extent. Every other
file byte and every original symbol address/size is identical to the canonical
ELF; all 53 resolved body spans and the three SDK memory spans remain exact.
This binds the diagnostic copies to the canonical proof. The separately
completed runtime/image and pressure checks are recorded below.

The initial canonical proof harness lacked its `Elf32` import and stopped
after the positive normal graph checks, before completing the diagnostic SDK
check. Its pinned tools and failure manifest remain under
`final/hardware/geometry-binding-v1`. The fresh v2 proof fixes that Python
import and passes; no target source, normal ELF or diagnostic copy changed.

The completed combined canonical acceptance is documented in the
[pass13 main report](PS2_OPTIMIZATION_NEXT13_20261115.md). All 25 complete
game PPM pairs match: 13 primary integration pairs, six ordinary-OS-RNG smoke
pairs and six legacy-mode pairs. The legacy cases use the normal startup
configuration and both `-hwgo 65536` and `65600`; their exact-file authority is
`final-legacy-images-v3-check/report.json`, SHA-256
`5cf1081f15c77c6d4a3090e4399e7cd0cdf325073c5c6eb111beef1077c2e002`.
All 17 separate CT32 GS readbacks also match the baseline. The standard GS
suite still fails its historical VU0 outcode comparison (CT32 55/56, CT16S
50/51, both return 1); this is no all-green conformance claim. Its zero
XYZ/reciprocal-W errors concern only vertices with both clip masks zero.

All eight fresh canonical pressure runs pass their separately frozen v3
protocol: two seeds, two scenes and both roles. Independent aggregate
`final/ram-pressure-matrix-v3/report.json`, SHA-256
`60bb3bfba26e676d524712522efc8537b03f8a53ffbedbb26c149504b35d503a`,
replays all four pair reports exactly. Geometry, PB counts, texture policy and
the complete ordered resource witnesses retain their required values; the
specified physical allocator differences and all timing observations remain
explicit in the main and RAM reports. The PB change adds no memory-accounting
exception. These completed tests cover the actual four-change canonical ELF;
they do not isolate a whole-game geometry speedup or establish physical-PS2
performance or FPS stability.
