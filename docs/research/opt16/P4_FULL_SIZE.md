# Exact PSMT4 in explicit full-size mode, 2026-11-16

The bounded change is adopted in working production source after source-host,
EE/VU build and native GS checks. Whole-game integration remains with the
coordinator. Ordinary planner mode retains PSMT8 and has no claimed VRAM saving.
The retained pass-12 planner and source-cache counterexamples still apply to
unrestricted packing; their restrictions have not been dismissed.
The compact machine-readable component record is
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
packet retains its full T8 row load and appends the T4 selector from unused
VU data quadword 28. CSA selects an internal GS CLUT bank, not a source offset.
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

## Executed checks so far

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
The final output is `build/nov15-opt16/p4-native-target-v4/HW_TEST.ELF`,
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

The dedicated source-native fixture runs the actual driver and VU1 program in
PCSX2. CT32 and explicit CT16 each finish **447/447 checks**, wrapper exit zero,
with no missing/skipped textures or timeouts. Every one of **74 complete frame
pairs per format** matches byte for byte: **5,304,320 compared pixels** per
format, with no crop or tolerance. Cases include all sixteen CSA bands,
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

Artifacts are under `build/nov15-opt16/p4-native-target-v4`, including
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
