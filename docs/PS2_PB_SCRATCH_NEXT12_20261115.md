# PS2 Hardware planner/walk scratch lifetime experiment

**Integrated into source `d4fbc7c3`; final combined two-seed native gates pass.**
Component and controlled native Hardware gates pass. Their source
and evidence remain frozen under `build/nov15-next12/pb-scratch-src` and
`pb-scratch`; production integration is a separate checked step. The measured
result is lower live/peak zone use in two Hardware scenes. No general map or
FPS improvement is claimed.

The baseline is published source `85bd9716b1ed114d46f95831c64a87623038eeaa`.
Exactly one of its 602 source files differs: `src/hardware/hw_pbatch.inc`,
SHA-256 `08e3ce0c810ba69f957ab8aaa0082e28bcdb1e3f7a3939f8d0da1e8133e2e350`.
The optimization applies only to `PS2 && PS2_PROFILE`. The narrow
`PS2_NOOPT_PB_SCRATCH` switch, non-PS2 and non-profile PS2 retain the original
separate planner and walk storage.

The planner's array contains two full-width words per polygon: block offset
and bucket number. `HWR_PBPlanPass` consumes all pairs in arrival order before
the draw walk begins. The walk then needs one contiguous block-offset word
per polygon. The candidate uses one `UINT32` allocation for these two phases,
with the walk borrowing its prefix after the planner returns. Both views use
the same scalar type; no struct/union alias, shortened field, reduced entry
limit, new cull or altered sorting key is introduced.

At the initial 12,288-quadword pool capacity, the entry capacity is 1,536.
The planner keeps its 12,288-byte allocation and the separate 6,144-byte walk
allocation disappears. The requested saving is `4 * ceil(pool_capacity/8)`
bytes plus one allocation root; at 62,208 quadwords it is 31,104 payload bytes.
These are storage calculations, not whole-game memory results. The original
capacity divisor, 50% growth sequence, soft-growth refusal, exact flush/retry,
bucket limits and required-allocation failure policy remain unchanged.

The actual driver's `PS2HWD_DrawBlocks` consumes indices during the call.
Its VU paths copy polygon words into the DMA ring; split/general paths copy
vertices or emit them synchronously. The index pointer itself is not retained
or submitted as a DMA reference. After successful drawing, `HWR_PBReset`
clears counts before the next collection. If a resource callback abandons a
plan or a partially written walk, `ForceSoftware` first clears
`currently_batching`, then calls `HWR_ReleaseBatching`/`HWR_PBFree`, before
renderer switching or a later collection. This cleanup guarantee is scoped
to the actual PS2 profile configuration.

`pb-scratch/lifetime-profile/report.json` archives the real PB, MIPS VU
consumer, split/general draw and guarded cleanup source chain. It is source
evidence, not a claim of native DMA execution by the host fixture.

The final host proof is `pb-scratch/host-profile/report.json`. The reference,
candidate and opt-out produce literally identical complete outputs:

| Evidence | Each variant |
| --- | ---: |
| State/planner/index/vertex/event bytes | 19,095,121 |
| C-driver packet bytes | 8,895,136 |
| Cases / frames or views | 32 / 52 |
| Planned / drawn polygons | 37,394 / 49,280 |
| Required growth failures / callback unwinds | 4 / 2 |

All three variants also pass AddressSanitizer and UndefinedBehaviorSanitizer
with identical outputs under `-O2 -fstrict-aliasing`. The complete production
PB include and real C texture planner execute. The fixture records every
sorted submission and emits C-driver packets; its synchronous index recorder
does not emulate native VU/DMA/GS execution. It covers empty/all-planned/
unplanned collections, capacity boundaries and growth, repeated views and
frames, sprite/shadow modes, transparent states, shader switches, GC replay,
state-limit flushes, soft-growth refusal, required-growth abandonment and
restart after failures both after planning and after walk writes. Exterior
allocation guards and complete backing-capacity checks stay enabled.

Four executed source mutations are rejected: planner order, normal producer
bucket, walk order and replay producer. All complete streams are compared;
neither selected fields nor a sparse pixel sample is used. Earlier fixture
development output and the earlier profile-guard source/proof remain
preserved. `profile-guard-binding/report.json` records identical selected
profile C tokens after the final scope tightening; a fresh final host proof
also runs against the final source hash.

`pb-scratch/objectcheck/report.json` is the **object-level** inventory of the
complete actual Hardware default-LTO context: 203 C inputs plus two VU
objects. Only `hw_batching.c` is recompiled with the checked include overlay;
the other 204 IR inputs and generated configuration are frozen. It archives
complete allocated section metadata, input bytes, ordered relocations/addends,
MIPS procedure records and caller targets. Its ELF hash pins are not a
reconstruction of resolved linked bytes.

That object/identity layer shows the opt-out restores the complete canonical allocated object, ordered
relocations, procedure descriptors and exact ELF. The candidate removes the
four-byte `pb_walk` global. Its total allocated text is 64 bytes smaller,
although `HWR_RenderBatches` itself is 96 bytes larger; no CPU improvement
follows from these sizes.

The separate linked layer is
`pb-scratch/resolved-binding/final-codecheck-v2/report.json`, reproduced by
`tools/ps2/next12_pb_scratch_codecheck.py`. Exact serial `--emit-relocs` replay
links first verify that every original allocated byte/address/size/alignment/
capacity and original defined symbol is preserved. Their retained linker
RELAs supply the exact selected address of each merged literal; an identical
string at another aligned address cannot substitute for it.

The 17 input code-or-binding changes comprise nine actual instruction-byte
changes and eight pure target-size descriptor changes. Together with all 42
direct callers, the inventory contains 45 sections. Two 32-byte out-of-line
wrappers, `HWR_ProcessPolygon` and `HWR_ReleaseBatching`, are genuinely
linker-discarded in both game ELFs (map address zero, no linked symbol).
Their object evidence is retained explicitly; their live `.part.0` bodies
and calling parents are reconstructed instead of claiming nonexistent bytes.

For each actual baseline/candidate game ELF, the resolved checker covers:

| Resolved evidence | Baseline | Candidate |
| --- | ---: | ---: |
| Complete live functions, including extra PB/planner/driver/wrapper/zone bodies | 65 | 65 |
| Complete referenced engine data/BSS sections | 570 | 569 |
| BSS sections with exact extents/capacities and named anchors | 454 | 453 |
| Complete distinct merged literals | 1,099 | 1,099 |
| Literal RELA uses with exact retained linker anchors | 1,504 | 1,500 |
| Ordered code RELAs | 4,720 | 4,709 |
| Ordered data-pointer RELAs | 1,170 | 1,170 |

Every complete selected text/padding body and initialized data section is
reconstructed with its actual target/type/location/addend fields, then
compared byte for byte against the linked ELF. BSS capacities, flags and named
object addresses/extents are checked without normalization. Actual memory
wrapper routes and reached ABI boundaries remain explicit; the complete SDK
`memcpy`, `memset` and `memmove` bodies are additionally reconstructed from
their exact pinned archive members. This is the stated selected linked scope,
not a claim that every function in the game was recursively reconstructed.

The shipping link discards `.pdr`. The checker therefore retains each complete
input PDR/save record and ties its advertised stack reservation and integer
save mask/offset to actual instructions in the fully resolved body. All 65
selected baseline/candidate frame/save records match. It does not pretend to
compare a nonexistent final `.pdr` table.

Nineteen executed internal controls pass: **18 real object/ELF binary
mutations**, plus an admitted `memcpy` wrapper-routing contract change.
Mutated artifact hashes are deliberately refreshed before the inner binding
checks. Register/body bytes, code RELA target/type/location/addend, data values
and pointer RELAs, frame/save metadata, BSS capacity, literal bytes and a
pointer to another identical aligned nonempty `0\0` literal all reject inside
the corresponding binding gate. Each binary and rejection reason is retained.
Both diagnostic RNG seeds are also checked anew: every file byte outside the
authorized initializer extent is unchanged, and all selected resolved body
spans retain their exact bytes.

The final linked report SHA-256 is
`6ab36c2ee73e894e62c080e3b64197b0ff5f7ea74a3f36a0568e52342caa27fe`;
its exact checker/helper snapshot is under `final-codecheck-v2/pinned-tools`.
The independent replay passes all 19 controls and reproduces the report
apart from review output paths; its addendum is
`build/nov15-next12/review/pb-resolved-audit.json`.

| Complete Hardware ELF | SHA-256 |
| --- | --- |
| Baseline and opt-out | `fc7a2919a519cc5b23be4ded8e295df41fda68c90350326cfa1113b63e46eff5` |
| Candidate | `855847697e3066e2959736ea616d8f0eb9a6b96ff98576689657a263870db390` |

Diagnostic-only copies fix M RNG with the existing independently checked
scalar template. The normal initializer in the original ELFs remains
unchanged. `-singletics` alone does not fix local M RNG. Seed `0x01b2d3f5`
diagnostics are `7542c365e7ea0383d5055bf78e45316ad9bd8f58e6950e608a3b5e1c7a6ec5ad`
for the baseline/opt-out and
`5efb99da8c880c34d160ce1e86f693aa981c3eb95864ab41ec7f2647566f1d45`
for the candidate.

Native primary gates reuse the coordinator's exact 350-frame MAP02 and
MAP01 fog/NPOT baseline runs, with the same fixed seed, command order,
assets/addons, configuration and full INI evidence. Both remain in native
Hardware throughout and complete without errors. All **407 canonical HWPH
rows per scene** and loader hashes match literally.

| Native 350-frame scene | Zone used / peak / global peak, before → after | Observed reduction | Unchanged PB capacity | Elapsed ms, before → after |
| --- | --- | ---: | ---: | --- |
| MAP02 | 22,146,048 → 22,136,768 B | 9,280 B | 18,432 QW | 29,354 → 29,351 |
| MAP01 fog/NPOT | 17,899,648 → 17,893,472 B | 6,176 B | 12,288 QW | 26,895 → 26,895 |

Zone evictions, evicted bytes and failed requests are unchanged, as are libc,
stack and spill peaks/current use. The EE remains the 32 MiB class and each
scene's arena size is unchanged: this is an observed increase in free zone
capacity, not an arena/RSS shrink claim. The payload calculations at these
capacities are 9,216 and 6,144 B; the whole-zone figures include the unchanged
allocator's rounding/placement. No particular extra allocation event or
absolute-address cause is inferred from the difference.

Every main driver polygon/vertex/upload/VRAM/LOD/budget field, every resident
texture dimension/format/block/wrap/flag row, and all PB capacity/flush/output
counters match. VRAM use is unchanged. PB general/slow/found lookup counters
change slightly with address hashing; their literal rows remain archived.
EE working cycles are slightly lower in every measured window, but individual
stage/max times, fog cadence bins and sleep differ. All timing rows are
retained, and these individual runs do not establish an FPS or stability gain.

The two separately authorized native image runs reuse the exact preceding
Hardware diagnostic and command/asset/addon/settings oracles. **All six full
320×200 PPM files at level frames 20, 70 and 140 match byte for byte**
(1,152,090 total file bytes). No crop or sparse pixel comparison is used.
Loader hashes, full non-UI emulator settings and emulator executable hashes
also match each image pair.

The complete resource/profile/VRAM/LOD and image review is
`pb-scratch/native-resource-image-audit.json`. It retains every literal
checkpoint, zone row and Hardware row, including all timing/lookup
differences. Primary reports are
`gate-pressure-map02-01b2d3f5-retry1/report.json` and
`gate-pressure-map01-01b2d3f5/report.json`; image reports are
`images-map02/report.json` and `images-fog-stereo/report.json`.

The first MAP02 wrapper attempt did not launch PCSX2 because the compiler-only
shell lacked `xvfb-run`. Its failed orchestration and unchanged-settings
archive remain under `gate-pressure-map02-01b2d3f5`; no boot log or game result
was produced. The successful retry sourced the existing emulator environment
and used a fresh directory. MAP11's frame-zero switch to Software at 15,942
subsectors is separately a compatibility fallback observation and cannot
demonstrate this Hardware scratch saving.

Replay is explicit and isolated:

```sh
python3 -B tools/ps2/next12_pb_scratch_replay.py
python3 -B tools/ps2/next12_pb_scratch_hosttest.py --negative-controls \
  --out build/nov15-next12/pb-scratch/replay-host
python3 -B tools/ps2/next12_pb_scratch_lifetime.py \
  --out build/nov15-next12/pb-scratch/replay-lifetime
```

Target context builds consume one granted EE worker serially; runtime `--run`
consumes the coordinator's separately granted emulator slot. The checked
`next12_pb_scratch_context.py`, `next12_pb_scratch_objectcheck.py` and
`next12_pb_scratch_run.py`/`next12_pb_scratch_images.py` preserve raw inputs and refuse silently measuring
the unchanged live source as a candidate. Existing completed proof outputs
are not overwritten. The public zero-context candidate patch and complete
source manifests make the proposed change reviewable without applying it to
production.

Default replay/checking uses the isolated immutable baseline plus its checked
overlay. It never assumes that live `ROOT/src` still matches the old baseline
after legitimate integration. The frozen runtime inputs contain no live
`ROOT/src` paths.

Runtime shells source both `/workspace/.onboarding/environment.sh` and
`/workspace/.onboarding/emulator-env.sh` before an authorized run.
`next12_pb_scratch_report.py` reproduces the read-only complete native audit.
The additional resolved check is read-only once the exact linker replay
artifacts exist; use a fresh destination:

```sh
python3 -B tools/ps2/next12_pb_scratch_codecheck.py --negative-controls \
  --out build/nov15-next12/pb-scratch/replay-resolved
```

`--verify-root` is optional and explicitly checks only the integrated PB
source hash. It does not require unrelated merged production files to match
the historical baseline. A new combined release build needs a fresh actual
object/map/ELF/linker-RELA binding and the final combined runtime gates.

The combined source `d4fbc7c319f19edc70eda220c3323d0350504c98` retains the
exact accepted PB include and adds the separately reviewed light table changes.
The fresh combined linked layer is
`build/nov15-next12/final/pb-binding/final-codecheck/report.json`, produced by
`next12_pb_scratch_finalbind.py`; all historical component evidence above is
unchanged. Its SHA-256 is
`2c1f88927d647fdb6bcd0eb6c100d798ed9b0c0bf808f172877928855cba43bf`;
the exact checker/helper snapshot is `final-codecheck/pinned-tools`.
It admits the complete 602-file frozen source by exact committed
Git blobs, all actual source/generated/external inputs, all 205 input objects,
and the actual final shipping Hardware ELF:
`efc963c7876a5f2a1c395baf5ba9982b68c6db1e56811d7a5e8a690ba840ab20`.
Independent replay passes all 19 controls and every record after review output
paths only; `build/nov15-next12/review/final-pb-resolved-audit.json` records the
final linked audit.

The final-source control keeps the light table active and compiles only
`hw_batching.c` with `PS2_NOOPT_PB_SCRATCH`; the other 204 actual IR objects
remain byte-identical. Its ELF is
`1a5add8da8693591f706f5b2deda7d26d5d51eb3d8fb1e19e1a904c5ed95d0b7`.
The complete final/control object difference is exactly the same 18 allocated
sections, 17 code-or-binding sections and 42 direct callers as the PB component:
nine actual instruction changes and eight size-descriptor changes, plus the
removed walk pointer. No light table or unrelated allocated-section difference
is attributed to PB. The control is a compiled attribution witness, not an
executed performance result.

Fresh serial retained-RELA links preserve every allocated shipping byte,
address, size, alignment and capacity and every original defined symbol in
each ELF. The final checker reconstructs all 65 selected live bodies per ELF,
their exact input frame/save metadata and instruction witnesses, all referenced
data/BSS and complete merged literals, actual wrapper routes and the three
SDK memory bodies. The final/control counts match the component table above
with control in the baseline column. Actual final addresses and literal
anchors are resolved anew; component-to-final body identity is not assumed.
All 19 internal binary/routing controls pass again. The final link still
discards `.pdr`, and the same two out-of-line wrappers remain explicitly
object-only evidence.

Both exact final Hardware diagnostic copies are also checked against the
actual final shipping ELF. Every file byte outside the authorized initializer
is unchanged, and every bound body retains its exact resolved bytes. Their
seed `0x01b2d3f5` and `0x9e3779b9` hashes are respectively
`6766d019d20ef841ee12604011c466231c4cb9ba213e0467ed92f313bbdfb122` and
`6abf462952a14316408645966dd845f0178e568b24ff4179dd795fd4e5dde47d`.
This read-only final binding adds no game execution, memory measurement or
FPS claim. Final combined native gates remain separate coordinator evidence.

The final control build requires one granted EE worker; the two retained-RELA
links run serially on that worker. Once those immutable artifacts exist,
replaying the final binding is read-only and requires no worker or emulator:

```sh
python3 -B tools/ps2/next12_pb_scratch_finalbind.py --negative-controls \
  --out build/nov15-next12/final/pb-binding/replay-final
```

The final checker defaults exclusively to the explicit frozen final source,
build, control, linker replay and RNG-copy directories. It does not consume
live production source or silently substitute the earlier component ELF.

The final combined pressure matrix has a separate prepared stage,
`build/nov15-next12/final-hardware-pressure/readiness.json` (SHA-256
`5c13b50e97971744f311be4891a40353dbcd73b0edb71ab81374c0c623380055`).
It pins 1,831 source/build/IR/RNG/retained-proof/runner/emulator/configuration
inputs and archives its exact controller/checker helpers. It reuses the two
completed original seed-one Hardware baselines above, prepares two fresh
original seed-two baselines, and prepares four actual final-build runs across
the two seeds and MAP02/MAP01 fog scenes. Every command clones its complete
original baseline command, changing only the run name, ELF and output path.
Preparation does not execute an emulator or compiler.

With the coordinator's sole emulator grant and both existing environment
scripts sourced, `next12_final_hardware_pressure.py --run` executes the six
new cases serially. `--run-case ID` executes one explicit case; existing
runtime evidence is never overwritten. The separate read-only
`next12_final_hardware_pressure_check.py --out <fresh-report-directory>`
checks the completed matrix and admits the exact final PB/light table/SDK
graphs, both initializer-only diagnostic copies and the complete actual
runtime settings. A later final light table report can be supplied through
`--lt-binding` without rewriting any execution readiness or tools.

Each within-seed scene pair requires all ordered loader, 407 polygon, light
table, Hardware memory, resident texture and cache-registry rows to match,
plus complete driver geometry/VRAM/LOD/budget/capacity and PB output counters.
Arena/end/GP/libc/stack/spill/eviction/flush capacities and policies stay exact;
only failed allocation attempts may decrease. Actual used/peak/global-peak
zone reductions must be observed, with the intended tag24/tag25 changes
separated from every other payload/block residency. All checkpoint, tag,
Hardware, timing, cadence and lookup differences remain in the report.
The prepared matrix is not itself a completed final native result.

The six fresh matrix executions subsequently completed, and the separate
actual final report is `final-hardware-pressure/audit-v1/report.json`,
SHA-256 `311272e731058f24589563a4d0f5ca3c93e8fa44153bf7ab0ef99ab6ccb0254e`.
Both seeds produce the same combined used/peak/global-peak reductions:
74,880 bytes in MAP02 and 71,776 bytes in MAP01 fog/NPOT. All capacities,
required streams and intended per-tag attribution pass. These are combined
PB plus light-table measurements; the isolated PB savings remain the
9,280/6,176-byte component observations above. No additional saving is
attributed to PB from the combined total. The final report binds the actual
91-function graph union and both executed diagnostic copies. The final
main and normal-RNG matrices additionally match all 19 complete PPM pairs.
