# SRB2 PS2 lossless optimization — third pass, 2026-11-14

This pass continues the two preceding uncommitted passes without reverting them.
Four GPT-6.1 Sol subagents at max reasoning handled Software, GS, audio and map
metadata. The coordinator implemented pack-buffer reuse, reviewed integration,
and built both complete engines.

The comparison is against the immediately preceding completed source and ELF
snapshot, not Git HEAD. `tools/ps2/prepare_next3_baseline.py` reconstructs that
source from Git revision `fc7b98fe657b986d6e24972410828f09b91182b4` and the frozen
`next3_baseline.patch/.json`. A fresh reconstruction was compared byte-for-byte
with every source file in the original snapshot: no differences.

## Changes and measured scope

| Change | Result | Scope |
| --- | --- | --- |
| Reuse completed I/O scratch for synchronous LZ4 middle-block decoding | **65,536 B less persistent RAM**, and the same reduction in the guarded scratch peak | Both engines using cooked packs |
| Adaptive immutable line spawn snapshots | EE payload `min(60*N, 24*N + 40*K)`, with `K` nonzero argument vectors; **791,472 B less level storage on MAP11** | Network games and `-keepspawn`; ordinary single-player already omits these snapshots |
| Remove retained WAV/DMX metadata around PCM | All 223 stock raw samples together release **28,240 B of actual EE arena storage** | Corpus total, not a promise that every sample is simultaneously resident; original PCM alignment modulo16 preserved |
| Retain per-span parameters in six remaining P2 tilted Software drawers | **21.98% lower summed EE cost** across 144 benchmark rows; representative 100-pixel rows improve 12.92–25.17% | Selected drawer kernels, not whole-game FPS |
| Exact byte-to-GS alpha scale `(a+1)>>1` | Three CT32 fill paths improve **22.81–25.60%** at width512, and 11.27–13.33% at width1 | CPU preparation of texture-upload data, not less GS drawing work |
| Batch zero-gain voice cursor advancement | With 1/8/64 silent voices, selected EE mixer rows improve **33.36–40.22% / 79.11–83.58% / 95.50–96.61%** | Exact PCM and complete voice state, including expiry and overflow fallbacks |

The line snapshot uses a pre-count before its single allocation. Dense argument
maps keep the exact old record array; there is no larger worst-case request,
extra allocation, moving resize or shared mutable argument vector. Representation
state is published only after allocation succeeds. Network archive bytes and
field widths remain unchanged.

The raw SFX allocation shrinks in place after the same accepted input has been
read and parsed. PCM position modulo16 is retained so no MMI/native mixer path
loses alignment. Cache accounting counts retained bytes; the externally visible
original lump length is preserved. The pack-buffer change does not borrow scratch
across allocating or reentrant DEFLATE code.

Resolution, texture dimensions, pixel formats, palette/lighting precision,
framebuffer selection, game physics, features, add-ons, Lua, UDMF, network support,
cache capacities and compatibility limits were not reduced. This pass claims
**no VRAM saving**. The GS packed-palette experiment was rejected because some
paths became slower; PSMT4 conversion was left out because its complete palette,
lighting and VU state handling was not yet proved.

## Complete builds

EE GCC15.2, release `-O2`, normal `-G8`, full existing feature set and engine LTO.
No fast-math, precision change, cut-down feature list or new capacity limit.

| | Previous | Current | Difference |
| --- | ---: | ---: | ---: |
| Software text+data+BSS | 6,683,217 | 6,684,161 | +944 B |
| Hardware text+data+BSS | 7,885,724 | 7,886,500 | +776 B |

Those are linked sections, not complete process RAM. Dynamic savings above must
not be replaced by ELF file-size comparisons or added to overlapping runtime
measurements. Software builds retain existing unused-code warnings; Hardware
production compilation/linking completed under the existing diagnostic gate.

- Software: `build/ps2/SRB2.ELF`, SHA256 `20f669c6e79f14b2593e79e63323d19cad9623e08e9474b00f726d3e14ec846f`.
- Hardware: `build/nov14-next3/hardware/SRB2.ELF`, SHA256 `77fa8a354e54d747b000dbc46b0ecb3d6cff2e0393fd53ea6684f01ac676094c`.
- Previous Software: `30fef9b92c863a3a03a135a8198217232ea48ff6463cfd7ef22a2c5ee26851fc`.
- Previous Hardware: `4400bb10b7060679233788ec94fc4e28c8ff7cfec79f345ea9561a94d0b3c310`.

Every input hash in each final production build report was checked after building;
none changed. The isolated eight-file production diff for this pass is
`build/nov14-next3/new-pass.diff`; complete production hashes are in
`build/nov14-next3/final-source-sha256.json`.

## Verification and limits

The low-level proofs include four Software math/optimization policies plus
ASan/UBSan and alias-content tests; actual frozen/current GS rows and entire
upload packets; full SFX PCM/state comparisons; real allocator shrink/eviction/
OOM checks; and actual production network line-archive byte streams. Detailed
reproduction commands and evidence counts are in the component reports below.

On the standard 32 MiB PCSX2 profile, the preceding Software build could not draw
MAP11 with `-keepspawn`: it reported `Not enough memory to draw map MAP11: 65536
bytes (PU_RENDERWORK)` and returned to the title screen before a capture. This
baseline run was stopped after observing that failure; it is not counted as a
successful game test. The candidate captured frames20/70/140 in that mode. At
`map-spawnstate`, level storage fell 14,344,608→13,553,136 B and total zone use
18,627,088→17,770,080 B: **857,008 B less used** at that checkpoint. This combined
measurement already includes the pack-buffer saving; do not add it again.

That stressed candidate run still logs existing large-texture `plain column`
fallback warnings, so it is evidence of improved memory tolerance, not proof of
perfect rendering in the stressed mode. Quality comparisons use identical
settings and scenarios that both versions complete.

There are measured costs: ten one-pixel Software cases take 3–11 extra cycles,
and six drawer stack frames grow by 16–32 B. Non-silent mixer benchmark rows have
at most 0.09552% extra total cost from the zero-gain check. Compact snapshot creation
adds a ten-argument pre-count and line archiving uses a 60-byte EE scratch record.
The component reports distinguish these costs from the gains.

The historical standalone GS VU0 outcode discrepancy predates this pass and is
not declared fixed. Physical PS2 hardware FPS, frame-time stability and power
consumption have not been measured; EE microbenchmark cycle reductions are not
whole-game FPS percentages.

Component reports:

- `PS2_PACK_SCRATCH_NEXT3_20261114.md`
- `PS2_SPAWN_MEMORY_NEXT3_20261114.md`
- `PS2_SW_NEXT3_OPT_20261114.md`
- `PS2_GS_NEXT3_20261114.md`
- `PS2_AUDIO_NEXT3_OPT_20261114.md`

## Final integrated game checks

**13 selected 32 MiB runs completed successfully, with 14 paired whole-frame
PPMs byte-identical and every paired loader hash equal.** No image region was
masked or given a tolerance. The selected comparison runs have no warnings or
fatal/heap errors. Logged audio underruns and decoded/consumed hash mismatches
are zero. Frames were requested with `-singletics` at 20/70/140 or 70/140, so both
versions use identical game tics and the fixed seed is installed before map load.

| Scenario | Paired frames | Mode |
| --- | ---: | --- |
| MAP01 + Lua cropped HUD + MIDI + spawn snapshot | 2 | Software |
| MAP11, normal single-player | 3 | Software |
| MAP01 + Lua cropped HUD + MIDI + spawn snapshot | 2 | Native Hardware |
| MAP02 + normal digital music | 3 | Native Hardware |
| MAP99 compact snapshot add-on | 2 | Software, `-keepspawn` |
| MAP99 dense fixed snapshot add-on | 2 | Software, `-keepspawn` |

The two MAP99 fixtures have 31 lines: K=0 selects 744 B instead of 1860 B of payload,
and K=31 retains 1860 B. Their integrated level-storage deltas are −1120 B and 0 B;
the fixed case still gets the separate 64 KiB pack-buffer saving. All original
argument values, including harmless arg9 in the dense fixture, remain in the
loader hashes and exact archive-stream tests. The generator is
`tools/ps2/next3_spawn_fixture.py`.

At the normal Software MAP11 `map-loaded` checkpoint, used zone RAM is exactly
65,536 B lower with unchanged level storage. MAP01 with snapshots and Lua/MIDI
is 154,544 B lower in Software and 154,704 B lower in Hardware at that checkpoint;
these are complete checkpoint deltas, not additional amounts to sum with the
component savings.

The extra stressed MAP11 `-keepspawn -zheap 1 -zstack -zquit 175` run reached
`ZQUIT DONE` on the 175th level frame, with no fatal/owner-check error. Its main
stack high-water mark is 132,560/393,216 B, and final free arena space 476,848 B.
It does have 2952 failed allocation attempts, repeated existing large-texture
fallback warnings and cache eviction; no claim of warning-free rendering or
zero allocation failures is made for this stress scenario.

Six additional completed diagnostic runs are retained separately. Periodic
`-hwstats 35` prints changing memory counters through console notifications,
which consequently differ in captured pixels after an optimization. The final
Hardware comparison pairs were rerun with that periodic print disabled in both
versions; their complete frames match. Both builds also automatically switch
MAP11 from Hardware to Software because of its 15,942 subsectors under the
existing guard. That fallback was recorded, and MAP02 was used for the second
native Hardware comparison. No renderer size guard was relaxed or reduced.

Evidence: `build/nov14-next3/integration-report.json`, the selected commands in
`integration-queue.json`, original/clean/MAP02 queue/status files, and all
run-local `boot.txt`, `summary.json` and PPMs under `build/nov14-next3/runs`.
The initial baseline MAP11 spawn OOM and an early capture-only run whose
requested 175-frame quit was preempted by `-vidshot` auto-exit are preserved as
diagnostics and are excluded from the 13 selected successful checks.

## Final GS readbacks

The unchanged standard driver suite was rebuilt against the final GS source
with the existing model checks and run in both framebuffer modes:

| Mode | Passed | Failed | Completion |
| --- | ---: | ---: | --- |
| CT32 | 55 | 1 | 56 checks completed |
| CT16S (`fb16`) | 50 | 1 | 51 checks completed |

Both remaining failures are byte-for-byte the preceding pass's VU0 result:
49,152 vertices, 24,625 outcode differences; maximum X/Y/Z differences and
reciprocal-W error are zero. No new failure was added. This full suite is
**not entirely green** and its nonzero return codes are retained.

All **17 complete CT32 readback PPMs** match the immediately preceding pass
byte-for-byte, including the two model scenes. Both modes report
`tex_missing=0`, `timeouts=0`, no dropped frames and successful bounded-wait
checks. Evidence is in `build/nov14-next3/gs-integration-report.json`,
`gs-integration/hwt-next3-{ct32,ct16}.{json,txt}` and the retained PPMs.

Final `git diff --check` passes. Both engine build-report input-hash inventories
still match the live frozen source after all validation and report generation.
