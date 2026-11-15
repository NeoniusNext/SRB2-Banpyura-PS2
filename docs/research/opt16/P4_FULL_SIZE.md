# Exact PSMT4 in explicit full-size mode, 2026-11-16

**Renderer-v7 completed the checked full-size-mode game gate.** Against frozen
baseline `604cfd150`, all 15 complete PPM pairs across five scenes match byte
for byte, and all ten boots contain zero missing/skipped-draw diagnostics.
The 350-frame full-mode MAP02 profile also completes without missing content.
These results cover the frozen checkpoint through `45f8de1cb`, including the
slot-856 clipping repair, AP88 source rescue, MMI helpers and bounded allocator
recovery. They do not validate every map or subsequent opt17 loader/cooker and
sprite-cache work. See the [v7 summary](../../../build/nov16-hwopt/v7-renderer-validation-summary.json)
and the checkpoint results below. No physical-console or FPS claim is made.

Whole-game validation rejected the initial v4 source. Its isolated native
tests passed, but full MAP02 captures changed 18,550–23,713 pixels and exposed
missing draws. Review found that the P4 selector at VU data quadword 28
overwrites an immutable clipping-plane mask: boot uploads all twelve plane
quadwords at 20..31. The earlier description of 28 as unused was wrong.
The two-file repair moves the selector to 856, after the three vertex lists and
before sprite scratch at 1000. It is applied after expanded native clipping
tests and an executed old-source negative control. The initial rejected source
and the later v5/v6 missing-draw failures remain recorded below; the v7 results
do not turn those earlier gates into passes.

Ordinary planner mode retains PSMT8 and has no claimed VRAM saving.
The retained pass-12 planner and source-cache counterexamples still apply to
unrestricted packing; their restrictions have not been dismissed.
The compact machine-readable checkpoint record is
[`P4_RESULTS_20261116.json`](P4_RESULTS_20261116.json).

The bounded path uses the existing explicit `HWDBG_NOPLAN` mode:
`-hwdbg 33554432`. The `-hwfulltex` alias selects the same bit during driver
initialization, after the engine's one pre-initialization `SetTrace` call.
This is an explicit quality choice: frame and immediate mip selection stay
off. Existing fixed caps, budgets, unsupported-size handling and renderer
features remain available. Supplying another debug bit, a fixed/adaptive cap
or a configured texture budget disables the PSMT4 eligibility path.

Eligible images are immutable regenerable P8 map/flat sources with both wrap
flags, a known palette, at least 2,048 source texels, dimensions no greater
than 1,024, and a legal transfer width divisible by eight. Every source index
must belong to one aligned sixteen-index band, and index 255 is excluded.
The source must be full size: no cached mip source, no level selection, no
decimation shift, and no split image. The complete scan publishes a band only
after checking the final texel. The PSMT4 footprint must actually be smaller.

Packed nibbles go directly into the existing DMA ring. The engine/cache source
remains P8; no persistent second copy or new scratch allocation is introduced.
The original source lock is held while the ring is filled. PSMT4 allocation
failure retries the existing PSMT8 allocation and failure path. GS page/block
holes remain included in the reserved span; half the payload does not always
mean half the actual allocation for narrow or short images.

The existing full CT32 CLUT images are reused. Scalar setup loads all 256
entries with T8/CLD1/CSA0, then writes T4/CLD0/CSA=band. Each VU1 light-row
packet retains its full T8 row load and appends a T4 selector. The initial
quadword-28 placement is rejected as described above. CSA selects an internal
GS CLUT bank, not a source offset.
No colour is quantized, alpha-zero RGB is retained, and filtering inputs and
logical dimensions retain their original values. Plain/keyed views share the
same packed image. Hole-bearing, split, capture and other ineligible images
retain their existing format paths.

On a runtime change of the no-plan bit, `SetTrace` flushes queued overlays,
fences pending source references, clears the pending immediate texture and
stale frame-plan keys, resets the plan and drops all PSMT4 owners and views.
It also resets reserve/thrash controller quantities to their initialization
values and clears live images' `imgws`, allowing existing T8 images to recharge
the newly selected mode's reserve during that frame.
Subsequent ordinary uploads therefore use PSMT8. The mode is not latched;
it can be entered again. `tex_want` also rejects stale immediate/batched hints
while the no-plan bit is set. This bounds the previous feedback problem:
retirement timing can change the residency of full-size sources, but it cannot
choose a different non-associative decimation source in this mode.

## Retained development and component evidence

The isolated baseline is the complete source at
`604cfd150c5240142d8cb5367950b53d987916d1`. The exporter
[`p4_opt16_candidate.py`](../../../tools/ps2/p4_opt16_candidate.py) replays the
retained prototype at its original source revision, then performs a three-way
merge onto that pinned baseline and applies the bounded eligibility changes.
It does not write production source. Stage manifests and adapted patches are
under `build/nov15-opt16/p4-bounded-v4`. The guarded eight-file integration
preserves the coordinator's separately tested planner-scratch reduction.

The actual EE GCC 15.2 compiler and VU assembler compile/link the bounded
target fixture without diagnostics except the known SDK ABI warning.
That historical v4 output is `build/nov15-opt16/p4-native-target-v4/HW_TEST.ELF`,
2,252,864 bytes, SHA256
`1127c7b7bc8ae422cf50e108c214174c3869c3bd3d304b82d267c90d002927ed`.
An earlier v2 `candidate/targetGS` attempt was accidentally invoked from the
repository root and compiled working production source; its explicit
`NOT-CANDIDATE.txt` excludes it from candidate evidence.

The source host fixture compiles the complete texture/palette implementation,
literal register setup, unchanged `PlanEnd`, actual `tex_want` and actual
`SetTrace`. Ordinary GCC and ASan/UBSan each pass **101,434,367 checks**,
**101,369,320 decoded texels** and **2,232 uploads**, including all 1,386 raw
stock square flats, every CSA band, 32 CLUT rows and three palette epochs.
Six executed corruptions fail for index bits, footprint geometry, payload
length, incomplete CLUT loading, lost CSA and an incomplete band scan.
The old larger-budget quality counterexample is still executed and rejected.
The final fixture/runner bytes are copied into `p4-bounded-v4/host-v1` before
compilation. Separate executed witnesses reject v2's carried policy counters
and v3's missing same-frame `imgws` reset; v4 passes both checks.

Lifecycle cases execute full-size → ordinary → full-size transitions, actual
P4 owners/plain-keyed views, a separate pending T8 DMA source reference,
stale same-frame hints, fixed/adaptive cap and budget exclusions, default-mode
T8 fallback and P4-allocation failure followed by T8 retry.

Actual EE compilation of every private record member's offset/size confirms
the P4 band consumes the old padding byte. Record size remains **140 bytes**,
alignment remains four, and every previous field offset and width is retained.
There is no record-payload increase. This is measured in
the final host report's `target_record_layout`; full ABI objects and logs
remain beside it.

## Executed native GS results

The expanded source-native fixture runs the actual driver and VU1 program in
PCSX2. CT32 and explicit CT16 each finish **700/700 checks**, wrapper exit zero.
Every one of **116 complete frame pairs per format** matches byte for byte:
**8,314,880 compared pixels** per format, or **232 pairs / 16,629,760 pixels**
in total, with no crop or tolerance. The
[native summary](../../../build/nov16-hwopt/p4-clip-native-fixed-v1/native-summary.json)
includes 42 clipping pairs per format and direct checks of all 48 VU plane/mask
words. Cases include all sixteen CSA bands,
nearest/bilinear filtering, negative and repeated wrap coordinates, plain/keyed
views, alternating T8/T4, two palettes, light-table replacement, every light
row and pending draws during a runtime mode transition. Each VU1 row frame
executes 512 actual VU polygons and checks 512 independent row samples.

Actual native record queries and `H.used_blocks` charges measure:

| Full-size image | P8 allocated | P4 allocated | Usable GS saving |
|---|---:|---:|---:|
| 512×512 | 262,144 B | 131,072 B | 131,072 B |
| 1024×512 | 524,288 B | 262,144 B | 262,144 B |
| 1024×1024 | 1,048,576 B | 524,288 B | 524,288 B |

These are complete allocator reservations, including GS block holes, with the
same existing CLUT area and no additional record or VU-state payload. They are
same-mode per-image savings, not a simultaneous stock-scene estimate. The
native fixture switches scalar/VU transport symmetrically through
test-only hooks in a copied driver, and queries PSM/CSA/size/block counts so a
T8 fallback cannot masquerade as a P4 pass. Its build report records those
diagnostic edits and the original complete source hashes.

The superseded v4 native stage passed 447 checks and 74 exact frame pairs per
format (5,304,320 pixels each), but lacked the clipping cases that exposed the
slot-28 defect. Its historical artifacts remain under
`build/nov15-opt16/p4-native-target-v4`, including
`hwt-p4-ct32.json`, `hwt-p4-ct16.json`, full text logs, frozen inputs/settings
in `runtime-p4-*`, all readbacks and the compiled fixture. The first native v1
stage retains eight symmetric fixture assertion failures (a dormant VU counter
and a mismatched test palette); v2 fixes them and v4 reruns the final transition
source. No physical-console speed, full-game FPS or default-mode VRAM saving
is claimed by these component checks.

The integrated production source was also rerun with `-DPS2 -DPS2_PROFILE`,
including the compact engine mipmap header and coordinator's planner-scratch
change: `build/nov15-opt16/p4-production-host-v1/report.json` records the same
101,434,367 checks per plain/sanitized run, six detected corruptions and unchanged
target record ABI. The runner requires an explicit baseline and scratch proof,
binds the actual `PlanEnd`/`plan_lmax` bodies to that separately executed proof,
and permits only its four exact reviewed textual changes when checking the
remaining policy against the pinned baseline. The actual changed policy is
compiled and executed; it is not replaced by the old policy.

The additional [`p4_vu_memory_hosttest.py`](../../../tools/ps2/p4_vu_memory_hosttest.py)
executes the actual boot and constant-packet C functions and interprets their
actual VIF UNPACKs into a bounded VU data image. The old quadword-28 source
fails preservation of the twelve boot plane quadwords. The isolated slot-856
repair passes ordinary GCC and ASan/UBSan: 1,189 checks, 64 constant cases,
161 executed UNPACKs and every plane vector/mask unchanged. The test also
checks the selector's C/VSM address agreement and disjointness from all
polygon/sprite data regions. The final combined source repeats this proof in
its [VU data-map report](../../../build/nov16-hwopt/p4-production-recovery-host-v1/vu-memory/report.json).
It is a packet/data-map proof; the native VU execution and v7 game evidence are
separate checks.

The expanded native fixture under
`build/nov16-hwopt/p4-clip-native-fixed-v1` passes **700/700** in both CT32 and
CT16. Its **116 complete frame pairs per format** compare **8,314,880 pixels**
exactly, including 42 additional clipping cases. It directly reads all 48 VU
plane/vector words after P4 row setup; all remain intact. Cases start from an
unpoisoned P8 reference and exercise all six clipping planes, negative W,
provable guard-band crossings, P4 → clipped T8/P4 and mode exit → clipped T8.

The otherwise identical old-slot-28 negative fixture under
`p4-clip-native-old28-v1` completes 700 checks with **48 expected failures**:
42 plane-table integrity failures (three of 48 words overwritten) and six
actual complete-frame clipping failures. The original 447 checks therefore
did not establish compatibility. The repair has no new allocation, record
field, VU instruction or upload payload; only the selector destination and its
matching VSM read address move, with a source range guard added.

The first corrected whole-game MAP02 rerun (the rejected v5 stage) at
`build/nov16-hwopt/clip-fixed-map02/report.json` compares all three PPM captures
byte for byte equal to baseline 604cfd in the same full-size mode. Loader
hashes and saved configuration also agree. Its strengthened gate still fails:
two AP88 patch sources were purged before frame-129 draws, despite equal
sampled captures. The full-size restriction therefore does not by itself
establish source availability for non-regenerable patches. The later
[try-only AP88 rescue](PATCH_RESCUE_20261116.md) rebuilds these images from pinned
patch columns without adding per-texture metadata or a fatal allocation path;
its acceptance belongs to the later v7 game run, not to these three snapshots.

The later [EE MMI packing/eligibility optimization](P4_PACK_MMI_20261116.md)
keeps the same selection and storage policy. Its actual native helper/corpus
checks are separate from the GS checks above. The pre-recovery production host run at
`build/nov16-hwopt/p4-production-mmi-host-v1/report.json` uses that updated
source, passes the same 101,434,367 plain/sanitized checks and all six controls,
preserves the target 140-byte record ABI, and includes the mandatory executed
VU data-map proof (1,189 checks per run). It preceded the rejected v6 game gate.

The v6 full-game gate then recorded nine exact PPM pairs across MAP02, MAP01
and fog/NPOT. The purged-patch failures were repaired, but the fog run exposed
three new `no VRAM block (pool)` failures. An actual-source allocator witness
confirmed that the original 64-attempt eviction loop can return zero after
freeing a complete fitting range, or after choosing scattered victims without
trying a feasible window. The
[bounded post-limit recovery](VRAM_RECOVERY_20261116.md) preserves the first 64
attempts, retries the now-free range and then tries one existing protected-safe
window. The [actual-source v3 report](../../../build/nov16-hwopt/vram-recovery-v3/report.json)
passes six cases per old/repaired variant under ordinary GCC and ASan/UBSan,
with equal state at the 64-attempt boundary, an independent owner/free bitmap
and two executed negative controls. Exact sampled frames were not used to
excuse either earlier missing-draw failure class.

The final combined MMI/recovery
[production host report](../../../build/nov16-hwopt/p4-production-recovery-host-v1/report.json)
passes **101,434,367 checks / 101,369,320 texels / 2,232 uploads** in each plain
and ASan/UBSan run, all six corruptions, the 1,189-check VU data-map proof and
the unchanged 140-byte target record ABI. The complete texture source is
`b7ebecb38b30ea72a23e37366b0c5e566ed3c99851aa9c11a8215b49673888ab`.
Recovery leaves the native-tested MMI packing and band-scan bodies unchanged.

## Frozen renderer-v7 checkpoint

The tested candidate contains the renderer changes through
`45f8de1cbf33e8e7b2581afc08cb02499dd8c186`; both roles freeze 602 production
files and use 32 MiB RAM. The shipping LTO ELF is 11,268,908 bytes, SHA256
`8fabe702b3785a8a5b80b0711c0e7b2edcf95d561a58586fc2e91d07983f67df`.
The mutable repository and later opt17 work are outside this checkpoint.

The [full-mode image report](../../../build/nov16-hwopt/v7-full-images/report.json)
compares MAP02, MAP01, fog/NPOT, legacy and legacy-sprites with the same
`-hwdbg 33554432` in both roles. All **15 complete PPM pairs are byte-exact**,
with zero changed pixels and zero RGB error. Loader hashes and saved configs
match. All ten boots have zero `HWSKIP`/missing/nonresident diagnostics and no
gate errors. Full frames are compared without crop or tolerance; these five
scenes and sampled times are the extent of this quality evidence.

The [350-frame full-mode profile](../../../build/nov16-hwopt/v7-full-profile/report.json)
also completes without missing content. Available zone plus C-heap free rises
by 721,072 bytes; zone used falls by 742,336 and peak by 425,008 bytes. The
raw run retains zone failures/evictions, a smaller arena and C-free, a larger
relative C-peak and different residency. It is not a pass of the original
strict pressure gate or a comparison with default-mode quality. Observed pool
occupancy is 9,864/8,141/9,487 blocks in the baseline and 9,732/11,225/9,359 in
the candidate, against 12,032 blocks; no global VRAM reduction is claimed.

The [stock trace](../../../build/nov16-hwopt/v7-stock-trace/report.json) observes
the full 1024² `F:GFS_CLD3` as P8 in the baseline and P4 in the candidate.
It prints stored format/dimensions/address, not `nblk`. The separately queried
native footprints are 4,096 versus 2,048 blocks, a 524,288-byte per-image
saving. The source binding proves identical footprint/stride and record layout
sections, not equivalence of the complete texture-upload files.

The separate [default image report](../../../build/nov16-hwopt/v7-default-images/report.json)
also has 15 exact pairs, but its strict gate remains red: every baseline and
candidate boot retains 40 inherited `selected while collecting` diagnostics.
No classification is exempted. Those default-mode signals are separate from
the successful full-mode image gate. All three
[strict default pressure errors](../../../build/nov16-hwopt/v7-default-pressure/report.json)
and the known standard GS VU0 failure also remain unchanged in status.
Neither this checkpoint nor the component helper timings establish full-game
FPS, exhaustive compatibility or physical-console performance.

```sh
python3 tools/ps2/p4_opt16_candidate.py \
  --ref 604cfd150c5240142d8cb5367950b53d987916d1 --out build/p4-opt16-replay
python3 tools/ps2/next12_p4_corpus.py --out build/p4-opt16-replay/corpus
python3 tools/ps2/p4_opt16_hosttest.py \
  --src build/p4-opt16-replay/candidate/src \
  --out build/p4-opt16-replay/host \
  --corpus build/p4-opt16-replay/corpus/full-texels.bin \
  --ee-cc "$PS2DEV/ee/bin/mips64r5900el-ps2-elf-gcc" --negative-controls
```
