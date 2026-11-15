# SRB2 PS2 lossless optimization — fifth pass, 2026-11-14

This pass preserves the four preceding uncommitted passes. Four GPT-6.1 Sol
subagents at max reasoning handled Software drawing, GS texture preparation,
audio and immutable side snapshots. The coordinator reviewed the changes,
built both complete engines and ran the integrated comparisons.

The reference is the immediately preceding completed source/ELF snapshot, not
Git HEAD. `tools/ps2/prepare_next5_baseline.py` reconstructs it using verified
`next5_baseline.patch/.json` from revision
`fc7b98fe657b986d6e24972410828f09b91182b4`. Every source file of that fresh
reconstruction matches `build/nov14-next5/baseline-src` byte-for-byte.

## Changes and measured scope

| Change | Result | Scope |
| --- | --- | --- |
| Adaptive immutable side snapshots | EE payload `min(24*N, 4*N+4*B)`, B nondefault scalar words; **667,568 B less level storage on MAP11** | Network games/`-keepspawn`; dense or index-overflow cases keep the prior layout |
| Seven NPOT perspective drawers retain per-call metadata | **12.8284% lower summed EE cost** across 252 rows; 240 faster, 9 equal, 3 one-pixel rows slower by 1–2 cycles | Selected drawing functions; all sampled lengths above one improve |
| AP_88 packed byte gathers use EE PPACB | **30.7764% lower summed cost** across 36 default fast rows, individual gains 19.1387–48.3102%; all fallback rows equal or faster | Exact texture-upload preparation, not less VRAM/GPU drawing work |
| Exact EE/MMI stereo step2 mixing | **61.5871% lower summed cost** across 24 selected rows, individual gains 38.189–67.383% | Existing 44.1 kHz stereo SFX at normal pitch; unchanged rate/output/sample values |

Side snapshots store six presence bits plus a word-stream offset in one 32-bit
descriptor, followed by full-width copied values in the same allocation. The
selection happens before the original third allocation and never requests more
payload than before. Mode/root publication follows successful allocation. The
extended SIDE_UDMF build retains all 26 fields and falls back when its descriptor
index cannot represent the data; this does not reduce a map or field limit.
Only the two actual `GetSideDiff` readers reconstruct a scratch record. Live
map fields, wire/save formats, signed values and repeat semantics are unchanged.

The NPOT drawers capture dimensions, lighting addresses/offsets, blend addresses
and the column cursor anew each call. Every floating-point/interpolation/sample
expression and source/table-content read remains in the original order. The
AP_88 helper replaces only the two existing shift/mask gathers in an already
bounded four-texel loop. Its portable expression, input loads, tails, fallback
conditions, palette/alpha rules and output bytes retain their meanings.

The stereo mixer is enabled only for EE with MMI, bounded to complete source
groups, and uses the original scalar end rules. No voice/rate/format, thread,
ring, latency or cache capacity was cut. Native and no-MMI preprocessed output
and complete strict objects are byte-identical to the actual preceding source;
the final default EE object is byte-identical to the fully measured candidate.
This guard was added after host timing exposed an unnecessary portable-dispatch
cost; no reference code was normalized to hide it.

Costs are retained explicitly: five lit NPOT drawers use 16 B more transient
stack; their complete real object grows 488 B with unchanged data/BSS. The side
constructor and inlined archive caller grow 16 B in isolated non-LTO EE frame
measurements; the side getter uses 32 B and pre-count/reconstruction adds CPU
work during snapshot creation/archive. The strict mixer object grows 1080 B;
the selected stereo call chain grows 48 B, still below the existing half-rate
maximum. Its 138 unaffected timing rows have at most 0.21382% extra measured
cost (silent rows at most 0.02066%). There is no new audio/static scratch buffer.

No graphics, physics, precision, dimensions, formats, filtering, features,
Lua/UDMF/add-ons/network support or compatibility limit was reduced. This pass
claims **no VRAM saving** and no whole-game FPS percentage.

## Complete builds

Release EE GCC 15.2 with the existing `-O2`, `-G8`, engine LTO and full feature
configuration. No build-flag or capacity reduction.

| Linked text+data+BSS | Reference | Current | Change |
| --- | ---: | ---: | ---: |
| Software |6,686,961 |6,689,097 |+2136 B |
| Hardware |7,886,908 |7,888,892 |+1984 B |

Linked sections are not total process RAM. Ordinary single-player already omits
all spawn snapshots, so the side saving does not apply there. There is no new
persistent heap/VRAM buffer; linked code still occupies its reported space.
The selected default MAP11/MAP02 loaded-zone use is unchanged, while snapshot
runs retain less data. No universal resident/peak-RAM reduction is claimed.

- Software `build/ps2/SRB2.ELF`: SHA256 `c913c71773576a6f4ca6c27386e12fe3568e3e5232ecf9dffdac2b8c5421cc32`.
- Hardware `build/nov14-next5/hardware/SRB2.ELF`: SHA256 `6c86e07025f0a66047eeec845a1f2d4b6687ea2cfef19405c7bef295851d7e18`.
- Reference Software: `403462b11c3a09007a7c9c21dc198a113f99b103173212c8f3fbe2fdf976bc44`.
- Reference Hardware: `e29b72a6260ff113ed400d5513e2952920a2eaa03016a3c50b6a1a9e17515f26`.

Final build input hashes match all frozen production files. The isolated
six-file diff and source hashes are `build/nov14-next5/new-pass.diff` and
`final-source-sha256.json` in the same directory. Software retains existing
unused-code diagnostics; Hardware passes the existing diagnostic gate.

## Exactness and code checks

- Side snapshots: 656 normal and 902 full SIDE_UDMF real-zone cases, ASan/UBSan,
 red zones, signed extremes, all fields, dense/index fallbacks, OOM root/mode
 transitions and eight negative controls. All **176,574,822 actual production
 archive bytes** match the preceding source and old-layout switch. EE ABI and
 full-width conversion checks plus Software/Hardware syntax pass.
- NPOT drawing: 323,204 cases per float/double/NOOPT_DRAW/NOOPT policy, each
 comparing 3,141,155,584 framebuffer/canary bytes plus lighting/cursor state.
 ASan/UBSan and 12 negative controls pass, including actual lookup-content
 aliases and a stale-cache error caught after two pixel stores. All 252 target
 timing rows check complete output/state before timing. Driver-only final
 coverage changes reproduce the measured object/ELF byte-for-byte.
- The real final Software ELF has zero tracked metadata loads in 31 detected
 store-containing loop regions across the seven drawers. Linked frames grow
176→192 B for four lit drawers, 192→208 B for water, and stay 144 B for both
 floor-sprite drawers. Other real-object text/data sections, rodata-content
 multiset, BSS and NOOPT_DRAW text/rodata match the preceding implementation.
- AP_88: host and ASan each pass 259,092 exact/oracle rows and 63,569,830 pixels;
 EE passes 140,944 rows/1,733,071 pixels, including all byte pairs in packed
 lanes, alpha/key modes, tails, alignments and unchanged fallbacks. Four
 deliberate corruptions are detected. Separate-TU fixture text falls 144 B and
 conditional stack 64→16 B. Actual full Hardware `fill_ap88` changes 1380→1220 B,
 conditional frame 48→16 B, with 0→3 PPACB sites after LTO loop specialization.
- Audio: actual EE 2,359,968 checks/246,301,741 stereo frames compare complete
 PCM/private state/canaries; both target mutations are detected. Host and
 sanitizer proofs, 363-file PCM/cache/read/pinning checks, and the established
 audio/thread/backend/ring suites with their negative controls pass. All 14
 existing kernel/helper instruction sequences are unchanged. Guard checks
 preserve both portable policies and the measured default EE object exactly.

These are target-code microbenchmarks, not physical-console cache, frame-time
or power measurements. Physical PS2 FPS/stability has not been measured.

## Integrated game checks

**13 selected standard 32 MiB runs complete, 16 pairs of whole-frame PPMs match
byte-for-byte, and every paired loader hash matches.** No regions are masked
and no pixel tolerance is used. All selected comparison runs have no warnings,
fatal/heap errors, logged audio underruns or decoded/consumed hash mismatches.

| Scenario | Paired frames | Renderer |
| --- | ---: | --- |
| MAP01 NPOT slope/floor/FOF scene + MIDI + stereo SFX requests + snapshots |3 |Software |
| MAP11 ordinary single-player |3 |Software |
| MAP01 same scripted/audio scene |3 |Native Hardware |
| MAP02 normal digital music |3 |Native Hardware |
| MAP99 compact side fixture |2 |Software |
| MAP99 fixed side fixture |2 |Software |

`-singletics` sets the seed before map loading. Capture times 20/70/140 or 20/70
have equal logged level tics. Both Hardware pairs confirm native startup with
no automatic Software fallback. Periodic memory-stat console messages are
omitted in both versions. The separately successful initial baseline scene run
was reused only after checking its ELF, add-on/script bytes and completion.

The validated scene creates eight 63×45 non-solid floor sprites, each with a
floor-sprite slope (four opaque/four translucent, with translated variants).
It configures 74 opaque and 73 translucent FOF planes, 14 existing sloped FOFs,
and 156/59 existing floor/ceiling slopes with NPOT textures. These are configured
counts, not assertions that every plane or all seven drawers is visible.
Generator/script: `next5_render_npot_fixture.py/.lua` under `tools/ps2`; PK3
SHA256 `f6ee4484ce0eac62438ac81db80954fdc360f71e8d52424769e9b4e6b441e36c`.
`tools/ps2/next5_audio_integration.lua` requests the stock stereo DSONEUP and
DSBOWL effects at tics 10/40/80/110 with the existing normal-pitch API; all four
request logs appear in both versions without an invalid/unsupported-SFX error.
It is not a claim of bit-identical asynchronous full-game audio scheduling;
exact PCM/state is established by the controlled mixer proofs.

The side fixtures have 48 sides: compact payload 1152→520 B, fixed equality
payload 1152→1152 B. Their measured loaded-zone changes are −624 B and 0 B;
allocation rounding explains the compact payload/arena difference. MAP01 with
snapshots retains 78,992 B less used-zone storage in Software and 79,056 B less
in Hardware. Default no-snapshot MAP11/MAP02 checkpoints remain equal.

## MAP11 retained memory and cache pressure

A further `-keepspawn -zheap 1 -zstack -zquit 175` run reaches `ZQUIT DONE`
at frame 175, with the same args and 32 MiB configuration as the preceding run.

| Metric | Reference | Current |
| --- | ---: | ---: |
| Spawn checkpoint level bytes |13,246,848 |12,579,280 |
| Spawn checkpoint total used zone |17,412,224 |16,744,656 |
| Map-loaded total used zone |17,411,888 |16,744,320 |
| Final used zone |24,094,720 |23,957,760 |
| Final free arena |649,216 |786,176 |
| Final largest free block |209,136 |425,088 |
| Arena capacity |24,743,936 |24,743,936 |
| Peak used zone in this trace |24,734,208 |24,733,648 |
| Cache evictions |12,066 |11,260 |
| Failed allocation attempts |2689 |1094 |
| Main stack high-water |132,560 |132,560 |

At spawn/map-loaded checkpoints, saving is **667,568 B**. At the final frame,
used space is 136,960 B lower and free space increases by the same amount;
these checkpoints overlap the same savings and must not be added together.
Both stress traces have no warnings or reported plain-column/low-memory visual
fallbacks. Failed allocation attempts can be handled by normal eviction/reclaim
and are not a count of lost frames or downgraded textures. The earlier pass4
report's wording on warnings was corrected from these retained raw logs; the
third-pass trace did have eight texture warnings, while pass4 and pass5 do not.
No claim of universal peak reduction follows from one cache-filling trace, and
its wrapping 32-bit cycle counter is not used as a whole-game FPS metric.

Integrated evidence: `build/nov14-next5/integration-report.json`, queue/status
files and run-local boot/summary/PPMs under `build/nov14-next5/runs`.

## Standard GS suite

The unchanged standard suite with existing model checks reaches completion:
CT32 **55/56** pass and CT16S **50/51** pass. Its sole failure in each mode is
byte-identical to the reference VU0 result: 49,152 vertices, 24,625 outcode
differences, zero maximum X/Y/Z/reciprocal-W differences. Those historical
nonzero test returns are retained; the full suite is not declared all green.
All **17 complete CT32 readback PPMs** match the immediately preceding pass,
including both model scenes; both modes report no texture misses/timeouts or
dropped frames. Evidence is `gs-integration-report.json` and
`gs-integration/hwt-next5-{ct32,ct16}.{json,txt}` under `build/nov14-next5`.

Component reports: `PS2_SIDE_MEMORY_NEXT5_20261114.md`,
`PS2_SW_NEXT5_OPT_20261114.md`, `PS2_GS_NEXT5_20261114.md`,
`PS2_AUDIO_NEXT5_OPT_20261114.md`. Final `git diff --check` and frozen input/hash
checks pass; final combined evidence is `build/nov14-next5/final-proof.json`.
