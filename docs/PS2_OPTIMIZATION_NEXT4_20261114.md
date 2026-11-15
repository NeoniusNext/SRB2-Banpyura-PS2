# SRB2 PS2 lossless optimization — fourth pass, 2026-11-14

This pass preserves the three preceding uncommitted passes. Four GPT-6.1 Sol
subagents at max reasoning handled Software, GS, audio and sector snapshots;
the coordinator compacted the resource directory, reviewed integration and
built both complete engines.

The reference is the immediately preceding completed pass, not Git HEAD.
`tools/ps2/prepare_next4_baseline.py` recreates it from revision
`fc7b98fe657b986d6e24972410828f09b91182b4` plus the verified
`next4_baseline.patch/.json`. Every reconstructed source file was compared
byte-for-byte with `build/nov14-next4/baseline-src`: no differences.

## Changes and measured scope

| Change | Measured result | Scope |
| --- | --- | --- |
| Compact internal lump compression selector into existing name padding | EE directory record 44→40 B; **60,208 fewer requested bytes** across all 15,052 stock records simultaneously resident | Both engines; disk formats, offsets, sizes, pointers and non-PS2 interface unchanged |
| Adaptive immutable sector snapshots | EE payload `min(104*N, 40*N + 4*B)`, where B counts nondefault optional scalar values; **306,656 B less level storage** at the measured MAP11 spawn checkpoint | Network games and `-keepspawn`; normal single-player already skips snapshots |
| Four ordinary P2 splat/floor-sprite drawers | **14.7131% lower summed EE cost**, all 112 rows faster (8.9752–28.6164%); **80 B smaller stack frame per drawer** | Selected drawing kernels, not whole-game FPS |
| O(1) GS texture footprint | Generic footprint helper **82.78–84.57% lower EE cost**, tiny cases 62.43–68.11%; local code 696→360 B and conditional stack 16→0 B | Identical footprint/placement/planner results; no VRAM reduction |
| Exact half-rate MMI mixing | 24 eligible timing rows sum **65.258% lower**, per-row 35.974–74.394% | Step 32768 with canonical low-half fraction; exact PCM/state and unchanged other-rate fallbacks |

All 22 optional sector fields retain their complete original bit patterns and
explicit defaults. Core fields, tags and colormap pointers stay in the compact
record; dense maps keep the prior fixed layout. Pre-counting chooses before
the same single allocation, and representation state follows successful
allocation. No new reserve, moving growth, persistent scratch or capacity cut.
An independent callback audit confirms that intervening zone eviction/reclaim
only frees caches/slabs or detaches private renderer state; it cannot run a Lua
collector/gameplay callback or change the live sector fields being counted.

The directory change retains natural field alignment and every original codec
value. C and C++ header syntax pass, and the non-PS2 preprocessed interface is
identical. All raw archive methods are mapped into `CM_*` before storage; an
unsupported raw method is not truncated to a byte.

Half-rate mixing repeats each source frame exactly twice, preserving existing
truncation, gain, clipping, panning, EOF and cursor behavior. Unaligned MMI loads
are bounded to the two source frames used for four outputs. An initial dispatch
candidate regressed 64 silent voices by about 19.5%; it was discarded. The final
six-argument helper plus exact zero-numerator test prevents division hoisting,
retains the old Render stack frame and leaves all 12 old kernel instruction
sequences unchanged. Final silent rows are 0.40–0.61% cheaper; other audible/
high-phase fallbacks have at most 0.070% extra measured cost. The new selected
half-rate call chain uses 48–64 B additional transient stack; strict mixer text
increases 1336 B without new data/BSS.

The sector constructor and archive each use one 104 B EE automatic scratch
record and incur a 22-field scan/pack/reconstruction when snapshots are needed.
No gameplay state, physics, precision, image dimensions, formats, filtering,
lighting, features, Lua/UDMF/add-on/network support or compatibility limit was
reduced. The existing oversized-map renderer fallback remains unchanged.

## Complete builds

Normal complete release configuration: EE GCC 15.2, existing `-O2`, `-G8`,
engine LTO and full existing feature set. Existing flags and capacities remain.

| Linked sections | Previous | Current | Change |
| --- | ---: | ---: | ---: |
| Software text+data+BSS |6,684,161 |6,686,961 |+2800 B |
| Hardware text+data+BSS |7,886,500 |7,886,908 |+408 B |

Linked sections are not complete process RAM. Directory/snapshot payload savings
and integrated checkpoint deltas below overlap; do not add them together.
Software retains existing unused-code diagnostics. Hardware compilation/linking
completed under the existing diagnostics gate.

- Software `build/ps2/SRB2.ELF`: SHA256 `403462b11c3a09007a7c9c21dc198a113f99b103173212c8f3fbe2fdf976bc44`.
- Hardware `build/nov14-next4/hardware/SRB2.ELF`: SHA256 `e29b72a6260ff113ed400d5513e2952920a2eaa03016a3c50b6a1a9e17515f26`.
- Reference Software: `20f669c6e79f14b2593e79e63323d19cad9623e08e9474b00f726d3e14ec846f`.
- Reference Hardware: `77fa8a354e54d747b000dbc46b0ecb3d6cff2e0393fd53ea6684f01ac676094c`.

All final build input hashes match the frozen live source. The isolated
seven-file production diff is `build/nov14-next4/new-pass.diff`; its source
hashes are in `final-source-sha256.json` in the same directory.

## Verification

- Metadata: actual EE ABI 44→40 B with all other field offsets unchanged;
 88 real-zone cases/version through 65,535 entries, full field/pointer/name
 preservation, owner/neighborhood checks, lower/equal measured native arena
 peaks, ASan/UBSan, C++ syntax and unchanged non-PS2 interface.
- Resources: all 15,052 stock lumps and 300,940 partial reads match original
 PK3 metadata/CRCs; v1/v2/headless, malformed layout, large indexes,
 prefetch/drop, policies and I/O fault checks pass. All four output tables match
 the preceding pass and all nine inputs remain unchanged.
- Sector snapshots: 1702 cases, real zone/red zones, allocation-site/representation
 transition failures, signed extrema/high-bit flags, copied tag and colormap
 identity, ASan/UBSan and negative controls. All **261,282,241 actual archive
 bytes** match the preceding implementation and the old-layout switch.
- Software: 752,832 differential cases per policy, including live alias contents,
 three policies plus sanitizer and six negative controls. All 112 EE timing rows
 compare full framebuffer/canaries before timing. Actual final linked Software
 ELF confirms zero parameter loads and zero stack accesses in eight pixel
 loops, frames 48/48/32/48 B and no `lw/sw` spills in those four functions.
- GS: 6,774,252 host footprint checks plus 154,080 generic plans and 56,707 block
 plans; ASan/UBSan and four negative controls. Final EE proof checks 921,772
 footprints, 62,272 plans and 50,560 block plans, reaching successful completion.
 The audited internal domain has positive dimensions through 65535 and virtual
 TBW through 1024; arbitrary-u32/unsupported-format equivalence is not claimed.
- Audio: host 1,060,272 checks/129,685,039 stereo frames, ASan/UBSan 1,059,840 /
 129,629,743 frames, and EE 1,060,272 /129,743,472 frames, with complete PCM,
 voice state and canaries. All 363 cache/read/pinning cases and the established
 audio/ring suites pass; targeted PCM/private-state failures are detected.

GS helper timing is not timing of the full `PS2HWD_PlanEnd` or a game frame.
The preceding full Hardware LTO build already folded generic divisions in its
T8 specialization; the new source also removes the remaining table scans there.
Physical PS2 FPS, frame-time stability and power draw remain unmeasured.

## Final game comparisons

**13 selected standard 32 MiB runs passed, with 16 paired complete PPMs byte-identical
and every paired loader hash equal.** No capture region was masked or given a
tolerance. All selected comparison runs have no warnings/fatal/heap errors;
logged audio underruns and decoded/consumed hash mismatches are zero.

| Scenario | Paired full frames | Renderer |
| --- | ---: | --- |
| MAP01 + P2 floor sprites/masked FOF planes + MIDI + snapshots |3 |Software |
| MAP11, ordinary single-player |3 |Software |
| MAP01 + same scripted scene/MIDI/snapshots |3 |Native Hardware |
| MAP02 + normal digital music |3 |Native Hardware |
| MAP99 compact sector fixture |2 |Software |
| MAP99 fixed sector fixture |2 |Software |

Both versions use the same add-on and `-singletics` before warp; captures are
at 20/70/140 or 20/70 with identical logged level tics. Native Hardware startup
and absence of automatic Software fallback are checked explicitly. Periodic
`-hwstats` console notifications are disabled in both comparison runs.

The focused scene successfully creates eight static 64×64 non-solid floor
sprites (four opaque, four translucent, with translated variants). It configures
67 opaque and 66 translucent nonsloped renderable FOF planes on MAP01. These
counts are configured objects/planes, not assertions that every one is visible.
Its initial smoke had a fixture-only Lua iterator error; that run was excluded.
The factory call, all FOF fields and SRB2's zero-is-false angle handling were
corrected and audited before the selected clean runs. Generator:
`tools/ps2/next4_render_sprites_fixture.py`; final PK3 SHA256
`089fc51c320385e1921145e4e4d13538303a2aa75afb240b2f30a0d70f2db176`.

The two sector add-ons have 12 sectors and 31 lines. Compact sector payload is
1248→480 B; the dense fixture retains 1248 B. Integrated level-use deltas are
−800 B and −32 B respectively, a 768 B difference matching the payload saving;
the common 32 B reflects other allocation layout, not extra sector compression.
Generator: `tools/ps2/next4_sector_fixture.py`.

At `map-loaded`, complete used-zone deltas are −60,304 B for normal Software
MAP11, −92,848 B for the Software scripted scene and −92,896 B for the native
Hardware scene. These include allocator/cache layout effects, so are not
substituted for record payload arithmetic or summed with component savings.

The extra `-keepspawn -zheap 1 -zstack -zquit 175` MAP11 run reaches `ZQUIT DONE`
on frame 175. Against the immediately preceding run with the same arguments:

| Checkpoint/metric | Previous | Current |
| --- | ---: | ---: |
| Spawn checkpoint, level bytes |13,553,504 |13,246,848 |
| Spawn checkpoint, total used zone bytes |17,778,704 |17,412,224 |
| Map-loaded total used zone bytes |17,778,384 |17,411,888 |
| Final used zone bytes |24,271,184 |24,094,720 |
| Final free arena bytes |476,848 |649,216 |
| Final evictions |14,592 |12,066 |
| Failed allocation attempts handled by existing fallbacks |2952 |2689 |
| Main stack high-water bytes |132,560 |132,560 |

Spawn-checkpoint zone use falls **366,480 B**, including both new directory and
sector savings; sector-tagged storage falls 306,656 B. At the final frame,
used zone storage falls 176,464 B. The new linked code reduces arena capacity
by 4096 B, leaving 172,368 B more final free space; this final-frame comparison
is separate from the spawn checkpoint. The preceding third-pass stress trace
logs eight large-texture fallback warnings; this fourth-pass trace logs none.
The failed-allocation counter includes attempts handled by eviction/reclaim and
is not by itself evidence of a visual fallback. No frame-capture parity claim
is made for this uncaptured stress trace. (Log wording corrected during pass5.)
More available retained-cache space can also fill further: peak used zone rises
24,729,040→24,734,208 B in this trace despite lower checkpoint/live-directory
costs. No universal whole-game peak reduction or FPS percentage is claimed.
The wrapping 32-bit cycle counter in this long trace is not used as an FPS metric.

Evidence: `build/nov14-next4/integration-report.json`, queue/status files and
run-local boot/summary/captures under `build/nov14-next4/runs`.

## Final GS readbacks

The unchanged standard driver suite with existing model checks completes:
CT32 **55/56** pass, CT16S **50/51** pass. Both failures are byte-identical to
the preceding pass: 49,152 vertices, 24,625 VU0 outcode differences, zero maximum
X/Y/Z/reciprocal-W differences. This historical failure is unchanged; the full
suite's nonzero codes are retained rather than called entirely green.

All **17 complete CT32 readback images** match the immediately preceding pass,
including the model scenes. Both modes report `tex_missing=0`, `timeouts=0`
and no dropped frames. Evidence: `gs-integration-report.json` and
`gs-integration/hwt-next4-{ct32,ct16}.{json,txt}` under `build/nov14-next4`.

Component reports: `PS2_LUMPINFO_MEMORY_NEXT4_20261114.md`,
`PS2_SECTOR_MEMORY_NEXT4_20261114.md`, `PS2_SW_NEXT4_OPT_20261114.md`,
`PS2_GS_NEXT4_20261114.md`, `PS2_AUDIO_NEXT4_OPT_20261114.md`.
