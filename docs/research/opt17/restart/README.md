# CPU ownership across Hardware restarts

The current candidate is commit
`321c8d4da2cdc4d175271726b8b2bc450cbbc57b`, with two CPU ownership fixes.
Its fresh, clean Hardware/LTO build contains 204 C units and two VU programs,
with no compile/link diagnostics: **11329336 bytes**, ELF SHA256
`7bf67e21438e697cf40c112b9166c2a3018956fbe909ef34f0780de17750e96d`.
The complete 607-source manifest SHA256 is
`c35217c87713e8f4be5edd8e765bbd92ef78997f1a1aeef259b4a4b536bb078c`.
The [final validation journal](../final-321c/validation-journal.json) confirms
**all 12 required matrix stages completed**, plus resource-native and both
MAP11 observations. The [artifact validation](../../../../prebuilt/hardware-opt17-20261116/validation.json)
records `all_required_matrix_stages_completed=true`,
**`all_strict_checks_pass=false`**, and `unexpected_new_failures=[]`.
The [artifact README](../../../../prebuilt/hardware-opt17-20261116/README.md),
[ELF](../../../../prebuilt/hardware-opt17-20261116/SRB2-PS2-Hardware.ELF),
[source manifest](../../../../prebuilt/hardware-opt17-20261116/source-sha256.json),
[build report](../../../../prebuilt/hardware-opt17-20261116/build-report.json)
and [SHA256SUMS](../../../../prebuilt/hardware-opt17-20261116/SHA256SUMS)
bind this measured round to `321c8d4d` / `7bf67e21`. Older RAM, image and native
results remain scoped to their original inputs.

The previous commit `22b5620cf0710b06541b36eee8801eaad772deb3` and prepared
shipping ELF
`82bc5f2690d56e87c3f9b9d032ef0bccefb73226037aadeac96a54e36de66d7a`
are **rejected**. Both original 175-frame PAL/480p switch chains stopped at
gametic 141 with `Z_CheckHeap -1140: ... doesn't have a proper user`, after
11 captures each. The fixed prototype finishes both chains, but missing-draw
strict failures remain. See the [main report](../../../PS2_HARDWARE_OPT17_20261116.md)
for the complete history and admission limits.

## Native failure and cause

| Evidence | Result and scope |
| --- | --- |
| [Rejected PAL chain](failed-switch-pal-22b.json) | ELF `82bc5f26`; wrapper exit 4; proper-user fatal, block 2028; 11 valid PPM payloads; 40 missing-draw diagnostics |
| [Rejected 480p chain](failed-switch-480p-22b.json) | Same ELF; wrapper exit 4; proper-user fatal, block 1945; 11 valid PPM payloads; 40 missing-draw diagnostics |
| [Failure-only diagnostic chain](diagnostic-switch-pal.json) | Diagnostic ELF `c6ac6169`; the original fatal and heap-check condition remain active |
| [Owner diagnosis](gcache-owner-diagnostic.json) | A live PU_HWRPLANE block of 818176 bytes still owns `&gc.blk`, but that cell holds a different allocation |

The diagnostic payload is `0x0191db00`; its owner cell `0x0082ba18` holds
`0x01400cc0`. The link map places `gc` at `0x0082ba08`, identifying the owner
as `gc + 16`, exactly `&gc.blk`. A user cell outside the Zone arena is legal.
The error is that its value no longer equals the payload of the old live block.

Same-level Software → Hardware reentry can call
`HWR_LoadLevel → HWR_PlaneCacheReset → HWR_GCacheLevel` before level teardown
has freed PU_HWRPLANE. The old reset forgot the live cache and cleared its
owner variable; the replacement allocation overwrote that variable. The fix
first detaches interned batch-state pointers, frees any live `gc.blk` through
`Z_Free`, then performs the existing reset. Zone has already cleared the owner
after real level teardown, so that case remains safe.

The independent flat-child defect has a different owner. PS2_PROFILE keyed
flat children own separate CPU pixels, but cleanup freed child metadata while
leaving those pixels and their Zone owner live. Cleanup now frees owned pixels
before metadata in pages, compact and dense opt-out layouts. The non-PS2_PROFILE
legacy shared alias retains `delete=false`; a stolen source with NULL data
is safe. Neither fix changes raster code, tile layout, cache sizing, framebuffer
or depth precision, mipmap/driver ABI (68/140 bytes), or graphics tolerance.

## Actual-function and real-Zone proof

The [lifecycle proof index](lifecycle-host-results.json) records the combined
21-source manifest SHA256
`d102eb845754e72320653ec7093eb8404219500576d8d9bf96d89095b3a2103a`.
Only `hw_cache.c` and `hw_gcache.inc` differ from `22b5620`. The separate
[technical explanation](../LIFECYCLE_ZONE_OWNERS_20261116.md) describes the
fixture boundaries and reproduction commands.

| Public report | Result |
| --- | --- |
| [Geometry cache, normal](gcache-normal.json) / [sanitized](gcache-fixed.json) | Each: 1495 checks, 64 resets; live cache, already-Zone-freed cache and replacement ownership pass |
| [Original geometry-cache negative](gcache-baseline-red.json) | Actual old reset with real Zone reproduces the proper-user fatal |
| [Pages](pages.json), [compact](compact.json), [dense](dense.json) | Each sanitized run: 504 checks, 24 cycles, 12632064 exact source bytes; actual create/regenerate/steal/cleanup |
| [Fixed owner reuse](owner-reuse.json) | Fixed cleanup survives retired-owner storage reuse |
| [Original pages](baseline-pages-red.json) / [dense](baseline-dense-red.json) negatives | Deterministic owner reuse reproduces the proper-user error without host use-after-free |
| [Original actual-libc-free negative](baseline-libcfree-red.json) | Actual old cleanup leaves a live Zone child after libc metadata free |
| [Target syntax](target-syntax.json) | Default/dense cache and geometry-cache compilation pass |

An expected-negative report's `passed=true` means the fixture detected the
original defect. It does not mean that the old implementation passes lifecycle
admission. Driver/raster services are host fixture boundaries; these are CPU
ownership proofs, not a replacement for native engine transitions.

## Prototype native recovery

Both fixed prototype reports use a clean combined-fix ELF with older
`22b5620` build metadata, SHA256
`9c6a93248cff46fed3cc1e6eac1ecabf3481f2e4a52d4fe0eb8ed1ae3ffa85fb`.
They execute the original chain with the unmodified heap-checker on ordinary
32-MiB PCSX2:

| Report | Integration result | Retained strict gate |
| --- | --- | --- |
| [Fixed PAL prototype](fixed-switch-pal-prototype.json) | 175 frames, 12 valid PPMs; wrapper exit 0; no fatal, automatic fallback, audio errors or warnings | 40 `selected while collecting` diagnostic lines; `strict_missing_draw_passed=false` |
| [Fixed 480p prototype](fixed-switch-480p-prototype.json) | 175 frames, 12 valid PPMs; wrapper exit 0; no fatal, automatic fallback, audio errors or warnings | 40 `selected while collecting` diagnostic lines; `strict_missing_draw_passed=false` |

`integration_passed=true` is a scoped recovery result, not an all-green
admission. Capture existence and valid payloads do not establish pixel equality
for every high-resolution scene. These prototype RAM/activity samples belong
to that ELF and sequence; the fresh final-source results are listed below.

## Fresh engine matrix

Both original [PAL](../final-321c/switch-pal.json) /
[480p](../final-321c/switch-480p.json) chains also complete on the current `321c8d4d` shipping
ELF `7bf67e21`: each has **175 frames / 12 valid PPMs**, wrapper exit 0 and
`integration_passed=true`, with no fatal, automatic fallback, audio errors or
warnings. The original heap-checker remains active. Each still records
**40 `selected while collecting` diagnostic lines** and
**`strict_missing_draw_passed=false`**.

The completed report SHA256 values are PAL
`f245617af04bd079b0c6a892e416bb86ead9ac4945a1f3440b4c523533043b86`
and 480p
`2be64c38ec2445e7db9b1fdfbb507d5c55f8df4242375360d9d52b2ad5c3eed0`.
These results establish scoped native recovery of the original engine switch
sequence, with the missing-draw strict failures retained. Subsequent completed
stages and additional resource/MAP11 observations are listed below.

The fresh [default diagnostic-RNG A/B](../final-321c/default-images.json)
also completed five scenes and ten boots:
**15/15 full PPM pairs**, loader hashes and saved configs are exact. It remains
**strict FAIL** with ten errors and **400 selected-while-collecting diagnostic
lines**, 40 per boot and 200 per role; all other skip classes are zero.
Report SHA256:
`8e6693590b247107e26f33852c77b7752d3075c7e8fbca580cf7f920165a1c12`.

The fresh [full-texture A/B](../final-321c/full-images.json) completes the same
five scenes with another **15/15 exact full PPM pairs**, exact loader/config
comparisons and **zero skips, warnings or errors** across ten boots. Its report
SHA256 is `375cfcf28983a6e982d71d3df0fa53466ea57dcd29bc1bdbf7fce2329cd463e8`.
Both image suites use the diagnostic seed `0x01b2d3f5`, separately scoped from
shipping OS-seeded RNG; the full-mode PASS does not waive default strict FAIL.

The fresh [MAP02/350f pressure report](../final-321c/default-pressure.json),
SHA256 `6c2b8d7b589a7aa2ffd8976146edd3fac22d636f3a10cf934eba0f509e2cb5e1`,
measures **+1920768 bytes (1.831787109375 MiB) net available RAM** on `321c8d4d`.
Zone used/peak/global peak falls by 1974592 bytes; Zone free rises by 1933632,
while C-heap free falls by 12864. Arena falls by 40960, C-heap peak rises by
15936, largest Zone hole rises by 1929184, spill falls by 758400 and stack
use is unchanged. Spill is already counted in Zone totals. All five ordered
streams are exact (10 LHASH / 407 HWPH / 2 HWLT / 49 HWWS / 3 resource-tail
rows). Both roles have 31 startup evictions, 7 failed attempts and no additional
gameplay evictions; evicted bytes rise by 16. Strict pressure remains **FAIL**
for `libcpeak`, `arena` and `libcfree`. These fresh RAM results happen to match
the old `22b5620` deltas; they are independently measured current-source data.

All **eight shipping PAL/480p switch/heat/wipe scenarios** now complete with
`integration_passed=true` and no fatal, automatic fallback, warnings or audio
errors. They produce **46 valid full PPM captures**, not 46 baseline pixel
comparisons. Every report retains **40 selected-while-collecting diagnostic
lines** and **`strict_missing_draw_passed=false`**.

| Current shipping report | Frames | Captures | Zone global peak, bytes |
| --- | ---: | ---: | ---: |
| [Switch PAL](../final-321c/switch-pal.json) | 175 | 12 | 20668064 |
| [Switch 480p](../final-321c/switch-480p.json) | 175 | 12 | 20653008 |
| [Heat PAL](../final-321c/heat-pal.json) | 45 | 3 | 22408576 |
| [Heat 480p](../final-321c/heat-480p.json) | 45 | 3 | 23310368 |
| [Wipe PAL](../final-321c/wipe-pal.json) | **137** | 5 | **23468224** |
| [Wipe 480p](../final-321c/wipe-480p.json) | **137** | 5 | 23405504 |
| [Heat-wipe PAL](../final-321c/heat-wipe-pal.json) | 38 | 3 | 22469840 |
| [Heat-wipe 480p](../final-321c/heat-wipe-480p.json) | 38 | 3 | 23362208 |

The maximum observed Zone global peak is **23468224 bytes** during wipe PAL
against a **23506944-byte arena**, a capacity-minus-peak difference of only
**38720 bytes**. This is a Zone accounting difference, not all-RAM headroom:
C-heap has separate accounting, some caches are purgeable, and end-of-run
free values describe another point in the sequence. Spill is already included
in Zone totals. These scoped peaks do not establish that all maps or transient
working sets fit in 32 MiB. The current MAP11 observations below include
actual Software fallback.

The [correct canonical standard-GS report](../final-321c/standard-gs.json),
SHA256 `a4817f282b47282291be5074dbdfa8f0e2b1fdd7a4c4b3bb8c3c281a40001280`,
now includes both GS-list model checks through `gs_list_eetest.py`. Its separate
native fixture ELF is
`265b4b5132755eceec2b701d2d14c660e912e158f3ff2fbf928195be89cf4c2e`.
All **34/34 complete CT32/CT16 readback hashes are exact** against the unchanged
reference (`reference_unchanged=true`, `regression_errors=[]`). CT32 passes
**55/56 checks**, CT16 **50/51**. Both retain the original strict VU0 numerical
FAIL: 49152 vertices, zero outcode differences, max dX=1, dY=1, dZ=86 and max
relative 1/w error **3.61343e−5**. `strict_passed=false` and suite exit **1**
remain unchanged; no golden image or tolerance was waived.

The retained [incomplete basic-fixture run](incomplete-basic-gs-fixture.json)
([final-folder copy](../final-321c/standard-gs-incomplete-basic.json)), SHA256
`adab8e0fa6f0158eebc0de1acc5d6a0a16c511af77c65c6e91fabb22e61eb311`,
used a basic builder with 54/49 checks and 30 CT32/CT16 readbacks. The unchanged
golden fixture requires 56/51 checks and 34 readbacks, including the four
GS-list model frames absent from that attempt. All 30 common image hashes are
exact; the existing strict VU0 numerical failure is retained. This incomplete
attempt and its original journal remain history; the fresh canonical report
above supplies the complete original golden set.

The final [resource-native report](../final-321c/resource-native.json), SHA256
`96dfe5ed6cd5255509d96120a69e4d1affedd93c76de2cf075643155c16d607c`,
passes both optional gap-layout PAK and TEXC zero-prefetch sets: each has
**3/3 literal full PPM pairs**, exact loader hashes and saved config, with no
skips, warnings or errors. They are compared with stock resources on the
**same final-source diagnostic ELF** `19384ec9`, local RNG seed and
`-hwdbg 33554432`. This is scoped MAP02 visual/resource admission, without a
loading-speed, FPS, steady-state RAM or all-map claim; append-only remains
the default layout.

Both current MAP11 runs use shipping ELF `7bf67e21`:

| Report | Observed Hardware outcome |
| --- | --- |
| [Ordinary guard](../final-321c/map11-guard.json) | 15942-subsector heuristic selects Software on **frame 0**; production thresholds are unchanged |
| [Diagnostic admission](../final-321c/map11-admission.json) | Actual **17544-byte PU_RENDERWORK OOM** selects Software on **frame 1**; actual OOM recovery remains armed |

Only the second run's preemptive SS/free heuristics are set through
`-hwfbss 20000 -hwfbfree 0`; `-hwfb` still arms actual OOM/driver recovery.
Both runs reach the 35-frame quit with wrapper exit 0, no fatal and an
`I_ShutdownSystem(): end of logstream.` marker. The
[exact runtime-command record](../final-321c/map11-runtime-commands.json)
confirms the staged arguments, frozen inputs, non-UI settings, emulator binary
and shutdown markers. This is the **current** observed OOM and Software
fallback; the old core-v9 19504-byte PU_PATCH request remains historical.
The two requests are different observations, without a RAM-gain inference.
MAP11 Hardware residency is not established.

All 12 required stages and additional observations are now complete in the
[final journal](../final-321c/validation-journal.json), SHA256
`95ab1da27d8375344cdb00d2ad5396c00b22f4eef99fb9ab3798929a29fe229b`.
The [original incomplete matrix](../final-321c/original-matrix-with-incomplete-GS.json)
and [post-matrix journal](../final-321c/post-matrix-journal.json) preserve the
first basic-fixture attempt and canonical rerun. Production source and
shipping ELF stayed unchanged throughout. Completion retains every strict
red gate; it is a measured round with partial limits, not all-green admission.
High-resolution sky/heat remains CPU-heavy. General FPS gain, all-map Hardware
residency and physical PS2 execution are unestablished.

## Carrying the unchanged GS component inputs

The [production-input binding](unchanged-gs-production-inputs.json) verifies
both historical positive and halo-negative native builds against `321c8d4d`:
**102 actual compiled production inputs per build** and **35 unhooked driver
files** are byte-identical. The original [seven-run V9 index](../highres-native-borrowed-v9.json)
and all runtime hashes are retained. It records borrow 318/0, common 144/0,
attributes 46/0, coordinates 40/0, window 257/0, full 231/1 (one RGB level at
one pixel, strict FAIL), and two expected halo negatives at 1559 and 14632
wrong pixels. The first boundary-input borrow fixture's 262/20 branch-assertion
failure remains historical; the interior positive was added with the original
boundary fallback controls and no tolerance change.

The two changed CPU engine files are explicitly **not compiled** by the
standalone fixture. Their evidence is the actual-Zone proof and engine switch
chains. This binding carries unchanged GS component evidence; it claims
neither seven newly executed boots nor equality of the old 20-file guard with
the current 21-file engine admission. Fresh current-source image, ordered-stream
and RAM observations, plus all eight high-resolution integration chains, are
documented above, together with the fresh canonical standard-GS, resource
and MAP11 guard observations. No physical PS2, all-map compatibility,
never-fallback, global FPS gain or all-green release result is established.
