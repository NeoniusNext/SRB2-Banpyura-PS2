# Private flat metadata, selected cut V2 — 2026-11-15

V2 is the preferred flat implementation for the combined candidate. It reduces
the flat candidate's actual EE text by 56 bytes relative to V1. Its isolated
`_end` and frames are unchanged. The selected combined-classic build measures
the same `_end` and `_gp` as the canonical baseline, with `.text` 32 bytes and
`.rodata` 64 bytes smaller. This is an actual combined build result, rather
than a sum of component estimates. The exact V2 cache source is now in local
implementation commit `5481bbbdc5aa3167c046836f3e51e4dbb31ad807`, alongside the
other three selected changes. All eight fresh canonical pressure cases and
all four legacy image cases pass. Publication is coordinated with the overall
pass-13 result. The original strict eviction-accounting rejection below is
retained.
The complete V1 source, tools, reports, contexts, and pressure readiness remain
unchanged. V3 and V4 each added 40 text bytes relative to V2 and are rejected
size trials. V5 remains a separate constructor-cold alternate and is not the
selected implementation.

The only V1 implementation changes are the private layout state and its uses:
`size_t gl_flats_cut` is the difference between the full map record and its
mipmap when compact, and zero when legacy. Initialization, teardown, and
allocation-failure resets store zero. The getter computes
`sizeof(GLMapTexture_t) - gl_flats_cut` and returns a mipmap pointer at that
byte offset from the allocated table. The second original full-N `calloc`
uses the same stride expression. The first allocation, read-only startup
parser, public types, generic/opt-out C paths, and callback/free order are
unchanged. No function attributes, reserves, compiler flags, padding, or caps
were changed.

Zero deliberately means the legacy stride. It preserves V1's false-layout
reset behavior when a callback invokes initialization; it cannot collapse the
entry stride to zero. Both the cut and index retain full `size_t` width.
The allocated storage has no declared effective type, the mipmap is the first
map-record member, and both supported strides preserve mipmap alignment.
The actual EE probe checks that the byte view is `unsigned char`, mipmap/map
sizes are 76/84, alignment is 4, the first-member offset is 0, and `size_t` and
pointers are both 4 bytes.

The isolated cache source SHA256 is
`869c84455d07bbf55af24d0f0fc48914809c5261e68c2a72d8baebce9551a857`.
[`next13_flats_cut_variant.py`](../tools/ps2/next13_flats_cut_variant.py)
recreates all 602 source files under
`build/nov15-next13/flats-cut-source-v2`; only `hw_cache.c` differs.

| Actual default EE component | Canonical baseline | V1 | V2 |
| --- | ---: | ---: | ---: |
| `.text` | 4,373,952 | 4,374,192 | 4,374,136 |
| `_end` | `0x8854b0` | `0x8855b0` | `0x8855b0` |
| GetLevelFlat body | 600 | 616 | 608 |
| LoadMapTextures body | 456 | 680 | 632 |
| FreeMapTextures body | 168 | 344 | 344 |
| GetLevelFlat frame | 32 | 32 | 32 |
| LoadMapTextures frame | 48 | 64 | 64 |
| FreeMapTextures frame | 32 | 64 | 64 |

`flats-cut-host-v3/report.json` is the authoritative host result: 675
comparisons across six profiles with ASan/UBSan and six detected source
corruptions. It retains the original 621 cases and adds 54 callback cases.
These include invoking the real initializer during the first `calloc`, which
must make the second allocation use the legacy stride, plus bounded reset
probes at set/delete/current-texture callbacks and argument mutation while a
table is live. Opt-out, non-PS2, and no-profile source excerpts preprocess
identically to the canonical paths. Exact-source i386 binaries compile but
remain unexecuted on this kernel. The executed tool and fixture bytes are
preserved under that host output's `frozen-tools/` directory.

`flats-cut-context-v2` and `flats-cut-control-v2` retain the complete actual
203-C + 2-VU default LTO context. Only `hw_cache.c` IR is replaced; the other
204 inputs and the changed unit's entire `.gnu.lto_.opts` section are exact.
The narrow opt-out restores the entire canonical ELF byte for byte. V2's ELF
SHA256 is
`44c9d2289e4f1064f8a562f8bb0f7f5c99a17e9b3b707b02f952cccdcd55c11c`.

`flats-cut-binding-v2/report.json` passes the complete actual linked
instruction/data/literal/ordered-RELA/frame/capacity proof for 60 baseline and
59 candidate bodies and nine inner binary controls. The complete allocated
object and ordered RELAs, all PDRs and ordered RELAs, and the shipped ELF
exactly restore under the narrow opt-out. Structured component size evidence
is in `flats-cut-cost-v2/report.json`.

The selected `combined-classic-v1` ELF SHA256 is
`ab3e3f163c1bb67b3dadacc6f368f17f6d73178ff348df186fb094474faa370a`.
Its `hw_cache.c` is exactly the cut V2 source above. The read-only
`flats-combined-binding-v1/report.json` rechecks all 602 frozen source files
and 205 actual IR inputs, then binds six complete flat root bodies and their
referenced data, literals, ordered RELAs, and PDRs to that actual ELF.
The six roots cover load/free, level-flat and wall lookup, regeneration, and
owner stealing. Other component and SDK closures have separate owners; this
report does not claim to re-prove those closures. The combined load/free
symbol extents are 628/340 bytes with 64-byte frames; their complete owned
sections, including padding, are 632/344 bytes. The getter is 608 bytes with
its 32-byte frame.

The fresh canonical Hardware build has its own generated commit metadata and
ELF SHA256
`7e4764fb609a1b3665936c0594ad4358c4bf1891545f6d4c48da13f5a0e7a9af`.
`final/flats-binding-v1/report.json` (SHA256
`74ca2dd768c3e986a738d666d82bcd158bb47b81da50e10f39b1760d316e2ca6`)
rechecks the actual 602-file source and all 205 freshly compiled inputs, then
binds 61 baseline / 60 final complete bodies, 400 / 401 complete referenced
data objects, and 1,007 / 1,008 literals, with exact ordered RELAs, PDRs,
padding, capacities, and direct interfaces. Nine inner binary corruptions are
rejected. Both final diagnostic copies preserve every byte outside the
212-byte RNG initializer. Actual final `_end` and `_gp` equal baseline.
This expands the earlier six-root all-up binding onto the full
component-selected flat/load/free/parser/wrapper/Spill/PB/caller surface plus
wall lookup. SDK/callee closures remain separately named proofs. The original
component narrow-opt-out proof is inherited by exact source identity; no
combined final opt-out compilation is claimed.

The coordinator's canonical primary integration report,
`final-seed1/next13-integration-report.json` (SHA256
`1db6fbbe9ccb0bed9e94aff1215848b02b2f1b5401cd1e787903c74da5dfcebe`),
passes all 13 complete PPM pairs and loader hashes, including three frames
each for Hardware MAP02 and full fog/NPOT/stereo. Its map-loaded zone-used
deltas are -37,920 bytes for Hardware MAP02, -37,936 bytes for Hardware fog,
and zero for Software. Those are literal zone allocation measurements in
that protocol; the fog value is distinct from the predicted 37,944-byte
table payload reduction. They are separate from the pressure resource and
actual legacy-order checks below.

## Completed canonical native gates

The final matrix runs both seeds (`01b2d3f5`, `9e3779b9`) through MAP02 and the
full 350-frame fog/NPOT/stereo scene, with a fresh baseline and current run
for each combination. All eight cases pass. The source-reviewed master policy
is `final-startup-resource-policy-v2.json` (SHA256
`6562f09bed475dcd17c64cc320f5fce9e958388ccc76682dc051e0e6ffeda21f`).
The corrected v3 parser and all four fresh readies were admitted before any
matrix run; nine quality and thirteen direct/parser controls pass. The raw
runs are under `final-hardware-pressure-v3/`, with frozen policy manifests and
pair reports under `final-hardware-pressure-v3-check/`.

Both seeds give the same non-time resource results in each scene:

| Actual canonical measurement | MAP02 | Fog/NPOT/stereo |
| --- | ---: | ---: |
| Native constructor N | 4,740 | 4,743 |
| Baseline flat ZA payload | 398,160 B | 398,412 B |
| Current flat ZA payload | 360,240 B | 360,468 B |
| Flat payload saved, exactly 8*N | 37,920 B | 37,944 B |
| Zone used/peak/global peak saved | 37,744 B | 37,968 B |
| Literal allocated residual after flat payload | -176 B | +24 B |
| Zone free increase | 37,744 B | 37,968 B |
| Largest free block increase | 37,792 B | 37,120 B |
| libc free increase | 4,624 B | 2,064 B |
| libc peak decrease | 8,192 B | 4,096 B |

N comes from the actual adjacent full-N wall/flat ZA requests, including the
flat row's cumulative spill increment. The wall request remains the full
84*N bytes. This is a shipping ZA payload measurement, not an asset census or
a plain libc request. The physical allocated residuals above remain
unattributed. Measured spill now/peak decrease by 37,920/37,944 bytes; no
per-block peak cause or additive zone/libc peak benefit is inferred.

All full ordered loader, polygon, material, texture, LOD, residency, driver,
and PB logical/capacity streams pass. Every world/final tag block count is
exact; only STATIC logical payload decreases, by 8*N. Every other logical
payload remains exact. Arena is 23,547,904 bytes and stack high-water is
132,560 bytes in both roles. Allocation failures and flushes are unchanged.

Direct reports observe 31 startup evictions and zero world count/byte
increments in each role. MAP02's per-role physical freed-byte anchors are
110,800/110,784; fog's are 110,784/110,800. They stay constant through all world
and final samples. These are cumulative physical freed extents, rather than
live consumption or quality ordering; no identity claim is made about the
31 victims. The predeclared policy records the startup deltas without a sign
gate and retains exact world/logical/capacity gates.

Both roles use common `con_hudlines "0"`, `-zspill`, and the existing
`-zreport 1` observer. The latter invokes `ZA_ResetPeak` at level exit:
`peak` is since-reset, while `gpeak` is global. The actual reporting and
`I_OutputMsg` frames are 3,296 and 1,360 bytes. Stack and peak comparisons
apply to this matched observer protocol; they do not establish default-
observer stack or old-protocol peak values. All raw timings remain available;
no FPS claim follows.

The independent aggregate `final/ram-pressure-matrix-v3/report.json` (SHA256
`60bb3bfba26e676d524712522efc8537b03f8a53ffbedbb26c149504b35d503a`)
replays all four pair reports byte for byte, rehashes all eight native outputs,
checks 138 shared frozen inputs, and confirms 2,800 world frames and 2,816
complete direct reports. Original and independent pair-report SHA256 values
are `48c19f28...`, `c0affd59...`, `d3ef57d2...`, and `844e9eb3...`.

The four separate normal-observer MAP02 image cases cover startup `-hwgo
65536` and `-hwgo 65600`, with the default five-line game config and no
`-hwstats`, `-zreport`, `-zspill`, or HUD override. All six complete raw PPM
pairs at level frames 20/70/140 are byte-identical, including headers and all
pixels; each file is 192,015 bytes. Exact staged ELF/arguments/assets/config
and archived emulator/runtime checks pass. The result is
`final-legacy-images-v3-check/report.json` (SHA256
`5cf1081f15c77c6d4a3090e4399e7cd0cdf325073c5c6eb111beef1077c2e002`).
This checks the actual legacy allocation-base/order behavior in both modes,
in addition to the source guard retaining the original 84-byte table stride.
The emulator grant was released after these twelve cases; no extra experiment
is part of this result.

## Retained rejected and development evidence

Fresh matched 350-frame MAP02 runs in `flats-combined-native-v1/map02` use
the existing official runner, audited seed-1 initializer copies, common
`con_hudlines "0"`, and `-zspill`. Actual adjacent wall/flat ZA requests confirm
N=4,740: baseline 398,160/398,160 bytes and candidate 398,160/360,240 bytes.
The measured flat payload saving is 37,920 bytes. Zone used/peak decrease
38,128 bytes (208 bytes of literal allocator residual), spill now/peak decrease
37,920 bytes, libc free increases 4,624 bytes, and libc peak decreases 8,192
bytes. Arena and stack are unchanged. Every tag block count and every other
logical tag payload is unchanged. All full ordered loader, geometry, material,
LOD, residency, driver, and PB logical/capacity outputs pass their frozen gates.

The frozen resource checker nevertheless **rejects** the pair: eviction bytes
are 110,800 in baseline and 110,784 in candidate, with 31 evictions in both.
This 16-byte difference is already present in allocated cache bytes before
the initial level-free transition. It is not assigned an allocator cause
without further proof. Further runs under that protocol were stopped; the
strict rejection is retained at `ram-combined-pressure-map02-v1/report.json` (SHA256
`59082888f896d8a26b5e510018017b993987db395bf4b9a7a4c9982399a5fd7d`).
The complete 75-artifact rejection manifest is
`flats-combined-native-v1/map02-rejection-v1/manifest.json` (SHA256
`c5878a911734504076c71126d7e6f19adbe9a31ab5e9d467e23b74c2bfc43c79`).
There is no after-result quality waiver and this old pair remains rejected.
Its separately prepared all-up fog/legacy plans were not executed.

The first canonical v2-parser baseline completed cleanly but exposed a parser
development error: the valid initial frame-0 report has an empty arena and no
tag rows. The old parser rejected it. The raw baseline and exception remain
under `final-hardware-pressure/01b2d3f5/map02/`, including
`baseline-direct-parser-failure-v2.json`; its candidate was never started.
The v3 parser accepts empty tags only with zero used bytes and includes a
nonzero-used/empty-tags negative control. The complete eight-run matrix above
was then executed afresh; the partial v2 baseline was not relabeled or reused.

An earlier preparation wrapper briefly reused the existing native-fixture
tool filename. The original tool was restored exactly from its frozen copy
before any run used that wrapper. The distinct wrapper is now
`next13_flats_combined_native.py`. Unexecuted prototype plans and the explicit
correction remain at
`flats-combined-native-v1/plans-filename-collision-v1.json`. This preparation
history changes no executed source, ELF, resource policy, or accepted output.

All evidence paths above are under `build/nov15-next13/`. No emulator was
used for the isolated component results. The later whole-game results measure
the actual canonical build and retain their separate protocol scopes.
