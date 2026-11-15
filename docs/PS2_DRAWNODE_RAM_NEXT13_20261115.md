# Pass 13 scalar implicit Hardware drawnodes

**Scalar V2 is integrated in local canonical source commit
`5481bbbdc5aa3167c046836f3e51e4dbb31ad807`. Its actual canonical graph and all
eight fresh native pressure runs pass. The initial by-value candidate and both
historical strict diagnostic rejections remain rejected.**

The canonical Hardware ELF is
`7e4764fb609a1b3665936c0594ad4358c4bf1891545f6d4c48da13f5a0e7a9af`.
The combined canonical matrix measures 37,744 fewer allocated zone bytes in
MAP02 and 37,968 fewer in fog/NPOT/stereo, with libc free increases of 4,624 and
2,064 bytes respectively, repeated in both seeds. All resource/quality rules
are recorded below. These are whole-engine observations under the matched
pressure observer; no exact native DN capacity or individual DN allocation
payload was traced. In particular, the source formula `16 * retained_capacity`
must not be inferred backward from those libc totals or peak changes.

The representation removes a redundant pointer table while preserving resource
limits, sorting keys, texture selection, lighting and draw operations. The
initial experiments described next were isolated; the later scalar V2 import
and canonical acceptance are documented in the final sections. Normal-observer
whole-image and release gates are separate authorities from this DN/pressure
record.

The first candidate reaches 350 native Hardware MAP02 frames at local seed
`0x01b2d3f5`, but loses 4,096 arena bytes and changes resident/upload feedback.
The second scene is deliberately not run. Its completed component and machine
proofs do not override that rejection. A separately authorized scalar ordinal
follow-up restores the arena and frees 4,624 libc bytes, but its first diagnostic
run fails the transient texture/upload equality gate. A later, separately
specified matched pair isolates a diagnostic-console observer effect without
changing source, ELF bodies, shipping defaults or any world/texture equality
criterion. All original rejections and raw artifacts remain unchanged.

The accepted source is `d4fbc7c319f19edc70eda220c3323d0350504c98`; all 602
source files also equal published `353cc94103aeb38cef87d26151c779302135d0e9`.
Its Hardware ELF is
`efc963c7876a5f2a1c395baf5ba9982b68c6db1e56811d7a5e8a690ba840ab20`.
The isolated baseline and candidate are `build/nov15-next13/ram-baseline-v1`
and `ram-source-v1`. Only `src/hardware/hw_main.c` differs, candidate SHA-256
`8d29aef4d3b89cfbe199372d6f216405473429da499d1ad17760e62394f43b25`.

## Representation and lifetime

`HWR_CreateDrawNodes` originally allocates four pointers per retained node.
Three consecutive loops populate the first range from `planeinfo`, the next
from `polyplaneinfo`, and the last from `wallinfo`; exactly one pointer in a
tuple is nonnull. The sprite pointer is never populated. Its existing sorted
full-width index therefore already determines the complete pointer tuple.

The initial by-value candidate snapshots the original three bases and two range lengths after
the unchanged allocation phase, immediately before those loops. A private
inline accessor reconstructs the tuple by value, and the redundant `dn_buf`
allocation, zeroing and pointer writes disappear. Snapshotting retains the
original pointers even when a later draw callback rebinds a list's global
base or changes its global count. No pointer arithmetic is performed on an
empty category for a valid node ID.

Both original qsort calls, comparator arithmetic, equal-height behavior,
consecutive-plane barriers, full `size_t` indices, `UINT32` collection/draw
loops and original index-capacity growth remain. The retained index allocation
still grows through 128, 192, 288, 432, 648 and the same subsequent sequence.
All material, lighting, geometry-cache and draw calls remain in order. No
texture working-set or planner feedback is intentionally omitted.

The accessor is private to creation and its comparators. An abandoned qsort
or draw does not resume after the existing fallback jump. `HWR_Shutdown`
resets the collected counts; the next creation overwrites the whole snapshot
before any accessor call. There is no new retained consumer of the snapshot.
The generic, non-profile and narrow `PS2_NOOPT_IMPLICIT_DN` paths retain the
original allocation and representation.

## Storage calculation and actual EE build

The actual complete-TU EE ABI probe confirms a 16-byte, four-aligned node,
pointer offsets 0/4/8/12, four-byte `size_t` and pointers, and a 20-byte,
four-aligned snapshot. The removed request is `16 * original_node_capacity`
bytes. At capacity 128/288/11,070 it is 2,048/4,608/177,120 requested bytes.
These are representation calculations, **not measured whole-game savings**.
The original capacity sequence, rather than an arbitrary selected limit,
determines the request. The pass12 stock-scene item counts suggest modest
kilobyte-scale requests; their exact retained maxima have not been traced.

The actual candidate context is `ram-context-v1`, ELF
`9b8accf19e167f6fbb4e3c55b27bbc9d11042577e4c05999fdc4e2434b5af1fc`.
It retains the default O2/G8/LTO/features and all 203 C plus two VU inputs.
Only `hw_main.c` IR is recompiled; the other 204 actual accepted inputs and
the generated configuration retain their complete bytes. Both LTO diagnostic
streams match after only checked equal-input path binding; no partition count
or warning is removed. The narrow control, `ram-control-v1`, reproduces the
**entire accepted Hardware ELF byte for byte**, complete allocated input
sections, ordered explicit RELAs and every PDR record included.

The candidate `.text` grows 272 bytes. `_end` grows 8,934,576 → 8,934,960 and
`_gp` grows 6,763,248 → 6,763,632. Final `.bss`/`.sbss` sizes stay unchanged
after actual linker placement. `HWR_CreateDrawNodes` grows 1,648 → 1,760
bytes and its frame grows 96 → 112 bytes. The two comparators grow 232 → 376
and 144 → 176 bytes; both keep their original 16-byte frames. No padding,
reserve or allocator adjustment is used to compensate. An unchanged arena,
stack high-water and cache/LOD behavior must still be established natively;
smaller requested storage alone does not establish an acceptable game result.

## Completed checks

`ram-host-v3/report.json` runs exact production collection, sorting, material
selection, lighting and wall code against explicitly modeled host service
boundaries. Baseline/candidate/opt-out compare 568 main cases, including 448
callback cases, empty caller guards, single-category lists, equal-height plane
runs, every growth boundary through 8,192 nodes, and distinctive moved copies
of all three source arrays. The full semantic state, sorted pointers, every
qsort argument and every comparator visit/result agree within each executable.
The main comparison covers 2,457,722,368 state bytes per policy.

Ordinary and ASan/UBSan policies also pass common index-allocation failure and
retry, eliminated node-allocation failure and later common growth, duplicate
drawcount failure, and draw abandonment followed by a new view. Deleting a
request intentionally removes its own failure opportunity; this is tested
separately from the common retained-index failure. Ten source mutations reject
wrong source mapping, all three unsnapshotted bases, changed plane-run length,
capacity growth, omitted texture selection, shortened counts and an altered
needed capacity.

The ordinary and sanitizer aggregate digests differ because their comparator
visit streams differ. `ram-host-trace-v1` isolates the first case to two nodes:
complete semantic state, qsort arguments and sorted permutation agree, while
actual `dladdr` witnesses identify libc's qsort versus libasan's qsort. Each
executable still compares all three variants' comparator visits exactly. No
cross-provider visit-order identity is claimed.

`ram-policy-v1/report.json` checks all 602 source pins and exact preprocessed
fallback tokens for PS2 opt-out, non-PS2 profile, PS2 non-profile and generic
policies. Each executed comparator-subtraction negative is detected.

`ram-binding-v2/report.json` reconstructs 161 actual live bodies per ELF from
a 172-section changed-binding/direct-caller/allocator/render-sink union. It
retains complete instructions and padding, exact target RELAs/addends, data
and BSS extents, merged-literal addresses, input PDRs and linked stack/save
instruction witnesses. Nine actual binary mutations reject after refreshed
outer hash admission. Exact retained-RELA replay preserves every original
allocated byte/address/alignment and defined symbol in its diagnostic ELF.

There are 102 changed code-or-binding section names. Five common raw bodies
differ: the three intended node functions plus the compiler's unchanged-source
`HWR_GCVerifyPool` and `HWR_ProjectSprite` bodies. Two helper names switch
between ordinary and `.lto_priv.0` forms; every actual variant is bound and
absent spellings are recorded, without fabricating a runtime body. This is
whole-engine machine-code evidence, not a standalone performance surrogate.

## First candidate native rejection

`ram-native-v1/map02-report.json` compares the actual accepted pass12 final
MAP02 diagnostic baseline with the first candidate, using the official runner,
the exact baseline command/assets/home fixtures and complete unchanged non-UI
PCSX2 configuration. The initializer-only fixed-seed copy is
`f0aa2118cf8e5fb5fd9297de4d317b6de4eadad45b79a19a213261947b704ed0`;
all other ELF bytes, symbol extents and all 161 bound graph bodies are checked.
Physical EE memory is 32 MiB and both runs reach 350 level frames.

| Actual final metric | Accepted pass12 | First candidate | Change |
| --- | ---: | ---: | ---: |
| Arena bytes | 23,547,904 | 23,543,808 | -4,096 |
| Used / peak / global-peak bytes | 22,071,168 | 22,071,536 | +368 each |
| Free zone bytes | 1,476,736 | 1,472,272 | -4,464 |
| Largest free run bytes | 1,462,736 | 1,458,320 | -4,416 |
| Libc free bytes | 165,048 | 173,384 | +8,336 |
| Libc peak bytes | 434,688 | 426,112 | -8,576 |

Stack high-water, spill current/peak, evictions/evicted bytes, failed attempts
and flushes match. All final per-tag payloads and block counts match; the
368-byte zone increase is an observed allocated-byte residual, without assigning
a particular allocator event. Native retained node capacity was not logged,
so the libc differences are not relabeled as an exact `16 * capacity` trace.

All 407 complete polygon rows and loader/light-table rows agree. Resident
working-set rows and complete driver tails differ, including one block/record
and opposing 64-byte upload differences in successive windows. No changed
visible-image or explicit mip-level field is claimed; the strict arena and
residency/upload gates already reject the candidate. The 38-ms elapsed increase
is one controlled observation, not an FPS result. No reserve, padding, capacity
or quality change is substituted for the failed gate. The fog scene remains
unexecuted and the emulator grant is released.

The pressure wrapper stops at `ZQUIT DONE`; it is not a full-shutdown/save
test. A preserved 76-byte config observation equals the pinned runner's exact
default staging bytes. Its initial metadata incorrectly asserted a pre-marker
copy; the separate `observation-correction.json` withdraws that unverified timing
claim. No pre-boot capture or completed output-save claim is made.

## Preserved failures and replay

The two first host development stages retain their compile failures (a missing
profile define and misplaced fixture declarations). `ram-host-v2` retains a
fixture-only retry comparison failure: after the intentionally deleted
allocation fails only in the old representation, the current representation
already owns its index buffer. The corrected fresh v3 uses the next common
growth boundary to compare like requests. Candidate source is unchanged.
`ram-binding-v1` retains its overly strict missing-helper-spelling stop;
fresh v2 records and binds the emitted variants. No earlier directory is reused
to manufacture a passing result.

## Scalar ordinal follow-up

The separate `ram-source-v2` source is
`25ab400933e147ae202648629b088999f7f2b3d84a70a839aeed471cffcdd3a6`.
It keeps the same five-word captured basis, full original ordinal indices,
growth sequence and pointer representation. A scalar comparator accessor reads
only the selected original drawcount. The plane comparator checks the captured
plane range and uses its original height expressions. The draw loop reconstructs
one local pointer tuple per item, then rereads pointee fields at their original
call sites across callbacks. No tagged pointers or shortened fields are added.
The omitted unknown-node comparator branches were unreachable for the original
fully initialized `[0,p)` ordinal set; qsort only permutes that set. Original
duplicate-drawcount and plane-comparator diagnostics retain their order.

`ram-scalar-host-v1` passes the same ordinary and ASan/UBSan cases, complete
semantic/within-executable qsort traces, callback movements, failure/unwind modes
and ten mutations. Its positive digests equal the first representation's digests
under the same respective qsort provider. `ram-scalar-policy-v2` restores all
four complete original fallback token streams. Its first policy stage preserved
a reporting variable-shadowing error; the completed preprocessing and candidate
source were not changed to correct that report path.

The actual default205-input scalar candidate, `ram-scalar-context-v1`, is
`f91ad5b0e87fb011229daa9b9670e9928536223cbf15562090b3015a3f2c7f8d`.
Its `_end` and `_gp` exactly equal the accepted baseline. `.text` is 72 bytes
smaller and `.rodata` 64 bytes smaller; final BSS/SBSS sizes match. The creation
body is 1,496 bytes with a 112-byte frame; comparators are 296/160 bytes with
16-byte frames. No padding/reserve/capacity adjustment obtains those results.
The original 96-byte creation frame is not claimed restored. Its separate
`ram-scalar-control-v1` reproduces the complete accepted ELF, all allocated input
sections/ordered RELAs and all PDRs. `ram-scalar-binding-v1` reconstructs 64 live
bodies per ELF from a 70-section union with nine binary negatives. Its only
three common raw body changes are the intended creation and two comparators.

The independent `review/ram-sdk-scalar-v1/report.json`, SHA-256
`4601272717b3a3a17c7cea86f57c931f9573b6c6f2985a60953372aeeffb21fe`,
adds eleven actual SDK functions across ten authoritative archive members and
fourteen engine roots. Complete ordered RELAs/addends, data/literals, fourteen
SDK PDR instruction witnesses and actual realloc-to-shipping-wrapper routes are
bound. Six inner binary/metadata mutations reject. This is read-only machine
closure, not additional native execution.

The scalar native gate is `ram-scalar-native-v2/map02-report.json`. The earlier
`ram-scalar-native-v1` is an unexecuted preparation with an overly strict literal
memory-total rule. Before scalar execution, the coordinator explicitly approved
only the `HW mem` C-heap-use numeric total decreasing, with every other field
literal; libc free may increase and libc peak may decrease. The new checker and
readiness were frozen before execution. All driver texture bytes, HWWS, complete
driver tails, formats, dimensions, LOD and budgets still require exact equality.
Its actual initializer-only diagnostic is
`11773e08e5eebc12a69ac21d77a1855c9cafff4c934496fba3bbdf4ae7afe988`.
Independent pure staging preserves the exact 76-byte configuration inputs before
any game execution, using the same pinned official staging function and inputs
that generate the fresh runtime directory.

The scalar MAP02 run reaches 350 native Hardware frames and restores the exact
23,547,904-byte arena, zone used/peak/global peak/free/largest fields, stack,
spill, eviction/evicted-byte, failed-attempt and flush totals. Libc free improves
165,048 → 169,672 bytes (**4,624 bytes**) and libc peak improves
434,688 → 426,496 bytes (**8,192 bytes**). Native node capacity is still not
logged, so these measured totals are not presented as an allocation trace.

Nevertheless, frame 105 driver texture bytes change **2,740,480 → 2,740,224**.
The first driver window has one fewer upload and 64 fewer upload bytes; the
next has one more upload and 64 more bytes. Quadword/state counters also differ.
All 407 polygon rows, loader/light-table rows, final HWWS rows and final tag
payload/block counts agree, but the predeclared transient texture/upload gate
fails. This is a rejection despite a clean measured libc benefit. No explicit
mip-level change or visible-image difference is inferred. The 17-ms elapsed
increase is one observation. Fog and image runs are not performed for this
rejected build; the sole emulator grant is released. No quality, budget,
allocator or reserve adjustment is substituted for the failed equality gate.

## Fresh matched observer control

The later source audit identifies a concrete confound: `Impl_HWStats` uses
`CONS_Printf` for the changing C-heap total. Those lines also enter the visible
console-message HUD. `CON_DrawHudlines` calls `V_DrawCharacter`, which requests
glyph patches through the Hardware renderer. The independently pinned ten
canonical digit glyphs are 8×8 indices: 64 upload bytes and one 256-byte GS block,
matching the sizes in the original discrepancy. The source/font evidence is
`review/console-observer-v1/report.json`, SHA-256
`bab8bcf013495c4a8a00a05071b2813f02eee27f2713ef12b0638aeb4c2f1977`.
This establishes the observer path and sizes; it does not alone prove unique
causality. A startup or open full console can draw independently of this HUD.

The coordinator authorized a **new matched diagnostic experiment**, not a
changed interpretation of the old execution. Both the accepted-final baseline
and the exact same scalar ELF use the additional staged setting
`con_hudlines "0"`. Zero is a supported value; its callback clears existing
timers, sets the count and logs an acknowledgement. Linefeed guards its modulo,
and the HUD drawer returns immediately at zero. Logging continues. Production
`con_hudlines` remains its original default of five, and no ordinary game HUD,
world setting, source file or ELF body changes.

`ram-observer-control-v1/readiness.json`, SHA-256
`4b7d82cc99da5fdcc0860e09e26c8380a52e814b856fbb9fd0a5436d15b7775b`,
was frozen before either new run. It pins the full two actual 205-input build
graphs, all 602 source files per context, both initializer-only diagnostic
copies, sixty-four complete bound bodies per side, SDK/console evidence, exact
world argument order, assets and complete non-UI PCSX2 settings. The official
pure staging helper produces identical **93-byte** input configurations, SHA-256
`16be657d017cb8cb923f23a249dacaba2c65addca659eab7e7f0c663db1c8eb3`.
Both actual boots acknowledge zero before the first `HW mem` observation.

Both fresh runs reach 350 native Hardware MAP02 frames in the original 32 MiB
configuration. The **same comparator module** used for the earlier rejection,
SHA-256 `841f4d4c2bbb20d7a4dd8c91251622d3526c8cb33446e44b197bba0f6b78c8ba`,
now passes every predeclared field and stream gate. All 407 polygon, ten loader,
two light-table and forty-nine HWWS rows agree; complete geometry/upload/VRAM/
LOD/budget and PB tails agree. Every `HW mem` field other than the previously
approved C-heap accounting total is literal, including frame-105 texture bytes.
Every final per-tag block, payload and allocated-byte field agrees.

The matched controls have the same 23,547,904-byte arena, 22,060,960 used/peak/
global-peak bytes and 1,486,944 free zone bytes. Stack, spill, eviction/evicted
bytes, failed attempts and flushes agree. Scalar libc free again improves
165,048 → 169,672 bytes (**4,624 bytes**), and libc peak improves
434,688 → 426,496 bytes (**8,192 bytes**). The console intervention changes
absolute diagnostic cache use in both versions; that common difference is not
counted as an optimization saving. Elapsed differs by +8 ms in this pair,
establishing no CPU/FPS gain.

The new pair report is SHA-256
`0c9445f3e03ca1a416e31255ec97323b2856304425a7c20880b6eaf96814ff02`.
`protocol-continuity.json` reruns the unchanged comparator against the old raw
games and reproduces their two errors and all original resource deltas exactly.
`ablation-report.json` also finds the ten loader, 407 polygon and two light-table
rows unchanged from each original role to its respective observer control.
No old run is reused as either new execution or relabeled as passing.

Independent audit `review/ram-observer-audit-v2/report.json`, SHA-256
`5a896891bab6de1215bacddef77d66543c9eb42d603a4c2344236e05fddcbfa7`,
checks all 1,660 readiness files, configurations/acknowledgements, actual
commands and ELFs, archived full non-UI settings, and recomputes every serialized
comparison field. Its first audit-development stage preserves a JSON key/tuple
representation correction; no game rerun or gate change follows.

This matched intervention supports the diagnostic-console observer explanation
for the transient texture differences. It does not retroactively pass the old
protocol or establish every possible cause. At this isolated observer-control stage, default-shipping, no-`-hwstats`
complete-image comparisons and actual combined-canonical-source gates were
still prerequisites to integration. Their later canonical pressure evidence
is recorded below. No new physical-console RAM/FPS claim is made.

```sh
python3 -B tools/ps2/next13_ram_variant.py
python3 -B tools/ps2/next13_ram_hosttest.py --out build/nov15-next13/ram-replay-host
python3 -B tools/ps2/next13_ram_check.py --out build/nov15-next13/ram-replay-policy
python3 -B tools/ps2/next13_ram_scalar_variant.py
python3 -B tools/ps2/next13_ram_hosttest.py --candidate build/nov15-next13/ram-source-v2 --out build/nov15-next13/ram-scalar-replay-host
python3 -B tools/ps2/next13_ram_check.py --candidate build/nov15-next13/ram-source-v2 --scalar --out build/nov15-next13/ram-scalar-replay-policy
```

The variant tool refuses production-tree destinations and checks the complete
source set when an isolated destination already exists. Compiler/LTO/link work
uses `next13_ram_context.py` only with a coordinator-granted serial EE worker.
The read-only binding tool is `next13_ram_codecheck.py`. Each original candidate
protocol has one completed native MAP02 execution and its rejection is retained.
The later observer pair comprises two additional fresh MAP02 executions. At
that isolated stage, image, fog and final combined-source gates were still
pending; the subsequent canonical stages are recorded below. No FPS gain is claimed.

## Canonical integration and separately frozen pressure accounting

The four-source canonical integration is local commit
`5481bbbdc5aa3167c046836f3e51e4dbb31ad807`. Its clean default Hardware ELF is
`7e4764fb609a1b3665936c0594ad4358c4bf1891545f6d4c48da13f5a0e7a9af`;
all 203 C and two VU inputs are recorded. `_end` and `_gp` remain exactly the
accepted-final baseline values. `final/ram-binding-v1/report.json` (SHA-256
`f9c96193a0f446aa5114530421b74bec0378055c9912197d603750b9f065f0c3`)
independently resolves 89 actual live bodies, complete ordered relocations,
padding, literals, data and PDR records. The three DN bodies and frames match
the tested scalar component. Two Create relocations differ only in the retained
size descriptor of the independently bound changed `HWR_GetLevelFlat` callee.
The complete component and canonical main IR hashes are both retained; their
whole bytes are not claimed equal. Nine actual binary mutations reject. Both
212-byte-initializer-only diagnostic copies bind the same complete graph.

The corresponding actual allocator/qsort/SDK closure is
`review/ram-sdk-final-v1/report.json`, SHA-256
`511ff41e32f06f464c4c70a42e8e4c12bf38a31f64759bbf98d898cd86ba4d63`.
It resolves eleven SDK functions from ten authoritative archive members,
fourteen engine roots, their exact wrapper routes and PDR instruction witnesses;
six mutation controls reject. These are canonical code proofs, not native
capacity or image acceptance.

The earlier all-up strict MAP02 pressure result remains rejected:
`ram-combined-pressure-map02-v1/report.json`, SHA-256
`59082888f896d8a26b5e510018017b993987db395bf4b9a7a4c9982399a5fd7d`.
Its only failed rule is literal cumulative `evictedbytes` equality: 110,800
versus 110,784 bytes. The 31 noted events occur within the startup forced-free
interval by count checkpoints; the old trace contains no direct post-flush
byte sample or victim-identity trace. All later counts remain 31. The measured
38,128-byte used reduction comprises the exact 37,920-byte flat payload saving
plus an **unattributed 208-byte physical allocation residual**. Neither physical
residual is assigned an invented header or cache identity cause.

The source and actual producer audit establishes that every eviction note uses
`ZA_SIZE`: the allocated physical extent, including header, rounding and any
absorbed trailing fragment. Requested payload is a distinct field. Cumulative
physical extents freed during the mandatory startup purge do not order memory
consumption or graphics quality. The independent pre-result classification is
`review/eviction-policy-v2-classification.json`, SHA-256
`1d4fe3d1a2d41744cdce3a61bd1c191b4c76959d6068f3f5751edbec570ba8f3`.

A **new** v2 canonical protocol therefore records startup physical freed bytes
and their signed difference without a sign or equality gate. It requires the
same startup count 31, an empty PU_CACHE at the level-free-after checkpoint,
and direct count **and byte** increments of zero throughout the world. Both
roles explicitly enable the existing `-zreport 1` stdout observer, retain the
same HUD-zero pressure control, assets, world settings, seeds and full emulator
settings, and use fresh output directories. The first complete report after
`precache`, at frame 1, supplies the direct world anchor. A checkpoint itself
prints only the count; no immediate post-flush byte sample is invented. Reports
must cover frames 1–350 consecutively plus the existing final frame-350 report.
Every per-frame/final logical tag count and payload remains gated: STATIC
payload decreases by exactly 8*N, all other payloads agree. Actual arena/end/gp/
stack gates, nonworsening memory bounds and all full quality/cache/upload/LOD/
residency/PB streams remain required. N is 4,740 for MAP02 and 4,743 for the
three-extra-texture fog fixture, subject to actual adjacent full-N requests.

The master is `final-startup-resource-policy-v2.json`, SHA-256
`6562f09bed475dcd17c64cc320f5fce9e958388ccc76682dc051e0e6ffeda21f`;
the fresh checker is `next13_combined_pressure_check_v2.py`, SHA-256
`ea07fa2599dcac2e700d48e9045a94ff8c72671f80c7c91b52453959350c23a4`.
The old checker, old rejection, and unexecuted v1 master remain immutable.
Nine quality-stream mutation controls use the completed observer pair; eleven
host-only parser/counter controls check missing frames, stale tags, long lines,
count/byte changes and logical payload changes. The four readiness/manifest
pairs were frozen before either role ran. Independent pre-execution review
`review/canonical-pressure-readiness-v2/report.json`, SHA-256
`47061856a83013b81c42b74807531445ec0e383ab7e143853b4acbf055e71b62`,
confirms all eight native output paths were absent at review completion.

The observer has real costs and scope. Actual reporting and caller closure
`final/ram-observer-binding-v2/report.json`, SHA-256
`552b8aa93f4b73c9c83765d74275522601047672f3a6c67e98bb61c9fb93ed8a`,
binds nineteen relevant bodies in each actual baseline/canonical ELF. The
PS2Mem_Report frame is 3,296 bytes; I_OutputMsg's frame is 1,360 bytes. Fifteen
report formats have a conservative maximum of 357 bytes, below its explicit
1,024-byte dynamic-long-line branch; the native checker additionally bounds
every actual report line. This is no general claim that SDK stdio never
allocates. Report helpers use I_OutputMsg, not the console HUD. Enabling zreport
also makes the existing level-exit tag-free path reset `peak`; `gpeak` remains
global. These matched observer stack/peak results must stay separate from old
no-zreport results and normal shipping/default full-image tests.

Canonical Software is a fresh full 189-C default build, ELF
`efc840a5b9e1e52053c9c937094a90fedd926f9808f8ef49f58f56d05dc6b511`.
`final/software-input-admission-v2/report.json`, SHA-256
`42c8d5a30e8d6f56a340d3316e0142eac6eb72db87751ca25bd8603bc1a7130a`,
checks all 189 whole LTO option sections and 466 actual input files. Only the
new committed metadata config differs among actual input bytes; it equals the
canonical Hardware config. All complete IR hashes are retained without a
whole-IR equality claim. The three existing source warnings and serial
59-LTRANS warning/note logs are byte-identical. The raw linker retains its
existing ABICALL warning, differing only in the bound authoritative engine
path; both complete raw texts are recorded. No native Software outcome is
substituted by this input audit.

The first v2 baseline completes, but its direct-parser probe fails before any
candidate runs. At startup the arena has used=0 and no allocated blocks, so the
source legitimately emits no tag rows. The v2 host parser incorrectly required
a nonempty tag table. That baseline and all v2 plans/manifests remain preserved
as an incomplete protocol; they are not relabeled as a completed comparison.

`next13_combined_pressure_check_v3.py`, SHA-256
`d83b33c19dc8a36b72f2de7c890a9cb5f6b66118bde0862d35a7d7d0fa1b0469`,
removes only that invalid precondition. The existing sum of physical tag extents
must still equal used bytes, so empty tags are legal only when used is zero.
The master resource policy remains the exact v2 file above, and no resource or
quality threshold changes. The source-derived zero-used/empty positive and
nonzero-used/empty negative join the retained controls: nine quality mutations
and thirteen direct/parser controls. Their report is
`ram-pressure-parser-controls-v3/report.json`, SHA-256
`0204004a74787a289f5937d1b9eeadcc09f0fef8abd17196770ba05489384140`.
An independent read-only replay against the complete preserved baseline format
parses 352 reports, including the empty initial report and all world/final
reports, without treating it as a new native comparison.

The coordinator requires a completely fresh eight-run matrix under
`final-hardware-pressure-v3`, with fresh manifests/results under
`final-hardware-pressure-v3-check`. Both fresh role paths must be absent at the
new pre-execution freeze. The later texture proof provenance/continuity is
pinned in those new plans; it is not represented as an earlier v2 input pin.

The first fresh v3 canonical MAP02 pair passes every frozen rule. Independent
raw replay is byte-identical to the owner's complete serialized report, SHA-256
`48c19f280bf822621b2fd72ee1f359480740ec81b54c8bcdc22e25c7e9e030f6`.
Both runs provide 352 complete reports, including the empty initial report and
351 world/final snapshots. Count 31 and directly observed physical totals
110,800 / 110,784 bytes remain unchanged throughout every world frame and final
snapshot; the startup difference is recorded without a sign gate. Maximum
actual report line length is 149 bytes. All 407 polygon, ten loader, two
light-table, forty-nine HWWS, and 352 registry rows agree; full upload/VRAM/LOD/
PB tails and every per-frame logical tag gate agree.

This canonical pair measures 37,920 fewer requested flat bytes and 37,744 fewer
used/peak/global-peak zone bytes, leaving an **unattributed −176-byte residual**
after the exact flat payload saving. This differs from the old all-up residual
and is not assigned a cause. Zone free improves by 37,744 bytes, largest span
by 37,792 bytes; spill now/peak improves by 37,920 bytes. Libc free improves by
4,624 bytes and libc peak by 8,192 bytes. Arena, stack, failures, noted event
count and flushes agree. The +7 ms elapsed observation makes no CPU/FPS claim.
Other fresh scene/seed pairs and normal-observer image acceptance remain
separate pending gates at this point.

All eight fresh canonical v3 pressure executions now pass: both seeds
`01b2d3f5` and `9e3779b9`, each in MAP02 and the full fog/NPOT/stereo fixture,
with a separate fresh baseline and candidate for every comparison. Each owner's
full serialized result is byte-identical to an independent raw replay. The
aggregate `final/ram-pressure-matrix-v3/report.json`, SHA-256
`60bb3bfba26e676d524712522efc8537b03f8a53ffbedbb26c149504b35d503a`,
rehashes every native output file and 138 distinct frozen inputs. All sixteen
run/runtime directories are distinct and under the new v3 root. It accounts for
2,800 world frames and 2,816 complete direct memory reports, including the
initial and final duplicate reports in each run.

The following measurements repeat in both fresh seeds; byte values are positive
savings/increases except the explicitly signed residual and startup columns.

| Scene | N | Flat requested bytes removed | Zone used/peak/gpeak reduction | Unattributed residual after flat payload | Libc free increase | Libc peak reduction | Startup freed physical extent delta |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| MAP02 | 4,740 | 37,920 | 37,744 | −176 | 4,624 | 8,192 | −16 |
| Fog/NPOT/stereo | 4,743 | 37,944 | 37,968 | +24 | 2,064 | 4,096 | +16 |

Spill now/peak reductions equal the exact flat payload reductions in these
runs. Arena size, final stack high-water, event count, failure count and flushes
are exact within every pair. Actual `_end` and `_gp` also agree. Every directly
observed world count and byte increment is zero; all full geometry, material,
texture, upload, VRAM, LOD, residency, PB and per-frame tag residency/payload
rules pass. The signed startup freed-extent difference obeys the source-derived
policy frozen before these runs and is not treated as a consumption or quality
ordering. The residuals and libc changes are measured rather than assigned
unproven per-block origins or peak additivity.

Elapsed differences are +7 ms / +36 ms in the first seed and +7 ms / +30 ms in
the second for MAP02 / fog respectively. These diagnostic observations make no
FPS/CPU claim. Normal shipping-observer full-file image gates, including the
separate legacy modes, are independent of this pressure acceptance.

The exact four canonical independent raw replay authorities are:

- `final-hardware-pressure-v3-check/01b2d3f5/map02/ram-independent-v3/report.json`, SHA-256 `48c19f280bf822621b2fd72ee1f359480740ec81b54c8bcdc22e25c7e9e030f6`.
- `final-hardware-pressure-v3-check/01b2d3f5/fog-stereo/ram-independent-v3/report.json`, SHA-256 `c0affd5900b6abf51f1938f3adf966007e2fc99e75f391144e81299b21a633b8`.
- `final-hardware-pressure-v3-check/9e3779b9/map02/ram-independent-v3/report.json`, SHA-256 `d3ef57d2bd5bd8ff51bc034001f9afee52e208b72670eb5733a5db6a51144cb9`.
- `final-hardware-pressure-v3-check/9e3779b9/fog-stereo/ram-independent-v3/report.json`, SHA-256 `844e9eb3b67ae8ff0c7c945989bbcf22d545b8e93ad20d610457a52b275b02c4`.

Paths in this document are relative to `build/nov15-next13` unless explicitly
rooted at `tools`, `src` or `docs`. Native retained DN capacities were not logged
in any canonical pressure run. The removed original DN request is exactly
`16 * original_retained_capacity` bytes by the source/EE ABI, with the separate
20-byte static captured basis and unchanged retained index array. Capacities
128 and 288 above are formula examples, not inferred native counts. The measured
4,624 / 2,064-byte libc free improvements and 8,192 / 4,096-byte libc peak
reductions are actual whole-engine outcomes; they are not relabeled as exact
DN payload, malloc chunk sizes, or an individual allocation lifetime trace.
The four-source integration and allocator extents preclude assigning the
unattributed physical residuals to specific blocks without additional evidence.
