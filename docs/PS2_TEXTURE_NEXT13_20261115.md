# Exact texture row iteration, thirteenth pass, 2026-11-15

The classic V3 loop is retained in pushed source commit
`5481bbbdc5aa3167c046836f3e51e4dbb31ad807`. It passes the complete actual-code native gate:
**288 timing rows improve, 96 tie and none regress**. Summed median row-call
cost falls **13.36130%** in the prescribed workload. Complete changed text
sections shrink **24 bytes**; `_end`, `_gp` and both filler frames stay at
baseline. This is emulated row preparation, not game FPS or physical PS2
CPU/cache/GS timing. Combined game, image and pressure validation is complete;
the [final main report](PS2_OPTIMIZATION_NEXT13_20261115.md) records the
finished CPU/RAM scope and its measurement limits.

Only `fill_idx` and `fill_ia88` change. Each uses one sample body and a
zero-guarded, full-width unsigned countdown. The source base stays fixed;
`u32 sx` starts at `(1u << dx) >> 1` and advances by `1u << dx`. IA88 converts
that wrapped column to `size_t` **before** scaling it to bytes. Source row,
cropped stride, texel/store order and alpha conversion stay exact. The P8
contiguous `memcpy` path is literal unchanged source. There is no threshold,
approximation, new flag, alignment requirement or lowered resource limit.

The [reviewable patch](../tools/ps2/next13_texture_fill3_candidate.patch),
[artifact pins](../tools/ps2/next13_texture_fill3_candidate.json) and
[reversible variant](../tools/ps2/next13_texture_fill3_variant.py) retain V3.
The patch passes `git apply --check` against production baseline.

## Source and arithmetic contract

The reference is the complete accepted 602-file source used by
`build/nov15-next12/final/hardware`, Hardware ELF
`efc963c7876a5f2a1c395baf5ba9982b68c6db1e56811d7a5e8a690ba840ab20`.
Its source is identical to pass 13's `353cc94103aeb38cef87d26151c779302135d0e9`
baseline. The classic snapshot is `build/nov15-next13/texture-fill3-classic/measured-source`;
the other 601 files are literal identical. Reversing the two function
replacements reconstructs the whole texture file, including decimation,
CLUTs, allocator, planner, cache bookkeeping and packets.

For every defined shift `dx < 32`, column `x` is exactly
`(x << dx) + ((1u << dx) >> 1)` modulo 2^32. The full `u32` countdown executes
exactly `W2` bodies; zero width returns before the new column shift/read. The
last unused `sx` update is defined unsigned arithmetic. The destination
cursor becomes one-past the final valid output element without another
access. Advancing a source pointer directly by `step*bpp` would lose the
required wrap-before-scaling order on a 64-bit host; this source keeps the
base fixed. Original byte loads and typed, aligned IA88 word stores remain.

Both functions retain their original immutable per-call context copy. Reading
its width into `remaining` adds no alias assumption. `tex_upload` owns `fc`
on its stack; `upload_bands` obtains ring ownership with `pk_alloc` before
invoking a row callback synchronously. The separately allocated ring cannot
overwrite the caller's stack or the filler's local copy. Each source read
still precedes its output store; P8 copy retains its original nonoverlap
`memcpy` contract. No callback allocates, submits DMA, mutates policy or
retains a pointer across calls.

Both texture-level `Z_SetRebuildCost` publications finish before these row
callbacks run. `TexInfo->ps2_cost` measures regeneration before `tex_upload`.
Upload duration enters `H.st.cyc_tex`, diagnostic `HWUPSLOW` and profiler
totals; no texture-quality controller consumes it. Logical dimensions,
filtering, LOD, formats, CLUTs, footprints/capacities, locking, allocation/
retry/failure, GIF headers, tail padding, TEXFLUSH and VU/DMA ownership stay
unchanged. The source audit is `texture-fill3-classic/ownership.json`.

## Exactness and actual default compilation

Ordinary and ASan/UBSan runs each pass **25,443 complete row calls and
9,318,667 pixels**, digest `e1b80903`. The two actual source versions compile
separately. An independent byte/formula oracle uses the original source
coordinates and `(alpha*128+127)/255` rounding. It covers every output width
0..1024, source byte residues 0..15, every valid output alignment, crop stride,
later rows, shifts 0..10, reused contexts, and all 65,536 intensity/alpha
pairs in all four consecutive pixel positions. Complete output buffers/tails/
guards, source bytes and contexts are compared. Six executed source
corruptions fail: step, centre, row, alpha, extra output and stale context.

The reduced corpus passes independently on host and EE: **11,703 calls and
3,390,711 pixels**, digest `ead0caa8`. Kernel, oracle, guards and the whole
prescribed timing matrix are byte-identical C/template inputs to the earlier
recurrence and peeled runs. Testing is not narrowed to former regressions.

The complete build copies all **203 C + 2 VU** canonical inputs and recompiles
only `ps2_hwd.c`. Global `-O2 -G8` LTO and the existing per-unit driver `-O3`
from `opt_units.txt` remain. The **entire baseline/candidate `.gnu.lto_.opts`
byte strings must match** before merging. All other 204 inputs and archived
generated configuration stay exact. Full default merge, normal link and
retained-RELA diagnostic link pass without new diagnostics. The companion
preserves every allocated byte/address/capacity and original defined symbol.

Only the two row functions change instructions. Eight other complete code
and PDR templates are literal equal but reference renamed compiler switch
tables `CSWTCH.1989→1991` and `CSWTCH.2091→2093`. Their full 59-byte/48-byte
payloads, flags, alignment, target binding/type/value/extent and empty data
RELA lists are equal. All **28 original ordered HI16/LO16 uses** are retained
and resolved separately in each actual ELF. Nothing is masked to admit the
renames. `switch-alias-proof.json` preserves every original record pair.

Full binding reconstructs **14 code nodes, 60 referenced data nodes and 82
complete literals per version**, including both rows, actual
`tex_upload.lto_priv.0`, `upload_bands`, memory wrappers and metadata consumers.
Original ordered relocation/addend/symbol data, initializers/capacities,
exact retained linker-literal addresses, padding and PDR frames with
instruction witnesses bind. Actual SDK `memcpy`, `memset` and `memmove`
archive bodies are separately reconstructed.

| Actual complete-game property | Reference | Classic V3 |
|---|---:|---:|
| `fill_idx` symbol bytes | 136 | 120 |
| `fill_ia88` symbol bytes | 148 | 144 |
| Complete changed text sections | 288 B | 264 B |
| Both filler frames | 0 | 0 |
| `_end` | 8,934,576 | 8,934,576 |
| `_gp` | 6,763,248 | 6,763,248 |

No persistent data/BSS, heap, frame, ring or VRAM allocation is added. The
isolated game keeps the baseline arena start; this claims no additional
runtime RAM or VRAM. Isolated Hardware ELF:
`b1935097c273fc6a6625a3cf5cee31d392c912cfb49255345ce7f55ae2297671`.

The native fixture executes these full actual-LTO bodies and the real memory
wrapper/SDK path. Separately compiled helpers establish no performance claim.
Complete graph bytes/padding, ordered original RELAs, GP0, ABI and frames bind
to both games and the kernel. Utility-kernel options do not retune the rows.
Native ELF:
`d76050489a5a7fec99301d9e53055e82a132e55401ab20a40e11170f14aafa1f`.
All raw evidence is under `build/nov15-next13/texture-fill3-classic`.

## Full native cost result

All **384 rows** have nine alternating repetitions through volatile function
pointers. Every timed destination and guard is checked against the oracle.
Interrupts are masked only inside the COP0 window and restored afterward.

| Classic V3 cohort | Rows | Reference sum | Candidate sum | Change | Faster / ties / slower |
|---|---:|---:|---:|---:|---:|
| Contiguous P8 copy | 96 | 2,426,444 | 2,426,444 | 0% | 0 / 96 / 0 |
| Decimated P8 | 96 | 4,903,464 | 4,371,504 | −10.84866% | 96 / 0 / 0 |
| IA88 | 192 | 20,446,128 | 17,266,848 | −15.54955% | 192 / 0 / 0 |
| All | 384 | 27,776,036 | 24,064,796 | **−13.36130%** | **288 / 96 / 0** |

One-pixel calls improve: decimated P8 50→49 Count; IA88 60→59. Single-call
rows have 144 improvements and 48 ties. Each cache cohort also has 144
improvements, 48 ties and no regression. These are unweighted synthetic rows,
not reduced transfer bytes, GPU work, bus traffic, physical cache latency,
frame time or game FPS.

Six executed binary controls are detected: instruction bit, missing RELA,
reordered RELAs, wrong addend, GP0 and an invented PDR reservation. Eight
parser controls are detected: missing/duplicate row, changed parameters,
missing completion, injected failure, wrong digest, malformed counter and
duplicate repetition. Every raw row, tie and rejected result remains archived.

The official wrapper exits zero after `FILL NEXT13 DONE`. The inner archive
and strict outer `next12_runtime` envelope preserve frozen inputs, all
non-UI INI settings and emulator binaries. The profile uses ordinary 32 MiB
RAM, normal EE rate/skip, no patches and disabled EE-cache modeling. Warm and
FlushCache-requested counts coincide; this is not physical-cache evidence.
The accepted matrix is `native-v2/summary/benchmark-summary.json`.

Independent read-only review repeats source/host/default-code/native/SDK/
runtime and corruption checks without another EE run. Native audit
`build/nov15-next13/review/texture-fill3-native-audit-v1/report.json` has SHA256
`c1676c1ea92399c9de3e69141633924c5cc6a283e1a167d18f1b22b9f20b500d`;
metadata reconciliation has SHA256
`a99990d3a2a842a2c4d6ab735cffea1c9682dc5b02a42ffdb1ece319e5419a36`.

## Integration and retained alternatives

[next13_texture_fill_finalbind.py](../tools/ps2/next13_texture_fill_finalbind.py)
requires the accepted complete 384-row native gate before reusing timing in a
combined game. It checks exact timed row/wrapper bytes, ordered symbol/addend
metadata and PDR frames, then fully resolves current bodies, callers, data,
literals and SDK. The isolated self-binding passes. Any code drift blocks
reuse. The same tool accepts a future canonical final build and its retained
linker-RELA companion. The separate combined source/game/GS/pressure/arena
checks are complete and summarized in the main report. The historical GS VU0 outcode
issue must remain visible; this component does not fix or hide it.

Full binding also passes against the selected combined 205-input context
`build/nov15-next13/combined-classic-v1`, Hardware ELF
`ab3e3f163c1bb67b3dadacc6f368f17f6d73178ff348df186fb094474faa370a`.
All timed row/wrapper input bytes, ordered original metadata and frames
remain identical to the executed fixture; full current caller/data/literal/
SDK reconstruction passes. Its `_end` 8,934,576 and `_gp` 6,763,248 exactly
match baseline, with the same 393,216-byte stack reservation. Evidence is
`texture-fill3-classic/combined-classic-v1-binding/final-binding.json`, SHA256
`350ab0fb5541e13cace581a8346290a41ba114a2eeb5e92e9ead16de29ae6f01`.
This admits component timing reuse in that context, not its separate
whole-game/GS/pressure checks. The four selected source changes are retained in
pushed commit `5481bbbdc5aa3167c046836f3e51e4dbb31ad807`.

## Canonical Hardware and diagnostic code binding

The complete canonical 203-C/2-VU Hardware build from that commit is
`build/nov15-next13/final/hardware/SRB2.ELF`, SHA256
`7e4764fb609a1b3665936c0594ad4358c4bf1891545f6d4c48da13f5a0e7a9af`.
Its full executed native row/wrapper bytes, original ordered relocation
metadata and PDR frames remain exact. The selector-complete binding covers
six nodes, including actual `tex_upload.lto_priv.0`, both rows,
`upload_bands` and both memory wrappers; it also reconstructs all 44
referenced data nodes, 32 literals and three real SDK memory bodies. `_end`
8,934,576, `_gp` 6,763,248 and the 393,216-byte stack reservation match baseline.

The current canonical authority is
`texture-fill3-classic/final-canonical-binding-v2-pinned/final-binding.json`,
SHA256 `45a56531f5fb1e2e58730c47add225bcee78985141b9881685818985c9fc7f97`.
It snapshots the exact checker and all 23 loaded repository Python sources
before verification, then checks those sources, snapshots and canonical/native
input bytes afterward. Its checker-provenance manifest has SHA256
`0d2b150a169cc1220fb4d7b3ce5639aa09c64023e24beca4ca27edbb600c2b14`.

Both final fixed-local-RNG diagnostic copies bind to the same complete graph:
seed `0x01b2d3f5`, ELF
`95b7f6d534362de126011d1f7cdba72d15d7e6fc0ce22971981c984cee0e182f`;
seed `0x9e3779b9`, ELF
`26f81d076a9a1033530641355ebc5ced348d2559dac96cc9ba8c2ba207361442`.
The checker independently derives the 40-byte seed stub and complete NOP
fill within the existing initializer extent. Every other ELF byte and all
symbol addresses/sizes remain exact. Complete selector/row/wrapper, data,
literal, SDK and frame spans retain the canonical binding.

That proof is `final-diagnostic-binding-v2-pinned/diagnostic-binding.json`,
SHA256 `edd29da3d0fcdc016192a10b1cfd03674e5d4be2bebca8b8c7bd8ea1fc0c91ea`;
its ten-source checker snapshot manifest has SHA256
`388ca96b61faca41bd59035430f66139c68d3aa2c381182bf868d4f2988b8cef`.
These read-only bindings run no EE compiler or emulator and establish no
whole-game, image, memory-pressure or performance acceptance beyond the
already executed 384-row component workload.

Earlier canonical V1 discovered only five nodes because the full build used
section-symbol references to the row functions. It is retained as a limited
binding, with its recovered old checker explicitly labeled reconstruction.
The corrected V2 checks both target names and actual sections and requires
the selector. Its complete six-node pre-provenance reports remain immutable.
The later provenance-pinned replay has exactly the same core fields, apart
from three byte-verified SDK extraction paths and corresponding report
references. `final-proof-continuity-v1/continuity.json`, SHA256
`c20292614c8029e164cd371a3da315327062fdf6e50b972c38b8a8d17905852f`,
records that comparison. This is explicitly later corroboration; new
provenance is never described as a pre-execution input of an earlier run.

## Completed combined validation

The final canonical checks match **25 complete game PPM pairs**, separately
from **17 matching GS readbacks**. The full eight-run pressure matrix
(two local seeds × two scenes × baseline/candidate) passes, with the final
memory accounting and observer limits recorded in the
[main report](PS2_OPTIMIZATION_NEXT13_20261115.md). Ordinary OS-RNG smoke runs
and the additional legacy collector modes also complete their image checks.
These combined results support the retained four-source change; the row-loop
percentage above remains a scoped EE Count result and is not assigned to
whole-game FPS or to the other components' RAM savings.

The standard GS suite preserves its historical CT32 55/56 and CT16S 50/51
results and return code 1. Matching readbacks do not make that unresolved
VU0 outcode test pass. Frozen helpers, raw evidence, checker snapshots and
artifact pins are unchanged by this final report update.

## Preserved alternatives

Peeled V2 remains an immutable exact fallback: 272 faster rows, 112 ties,
zero regressions and −17.07745% aggregate. It adds 64 complete text bytes and
moves `_end`/`_gp` by +128 bytes. Classic V3 trades some unweighted aggregate
speed for smaller code and the baseline arena start; neither universally
outperforms the other. V2 source/native data and its report snapshot remain
under `texture-fill2`, with separate public patch/pins.

The first recurrence is exact but **rejected**: 256 faster, 104 ties and 24
slower rows despite a −17.1018% aggregate. Every slower row is width one,
+1 Count per call (P8 50→51; IA88 60→61), in both cache modes/call counts.
Results and rejection remain under `texture-fill`.

The earlier decimation-score experiment passes 4,801 host/sanitizer calls and
27,508,445 pixels, digest `ecef082d`, with six corruptions detected. It is
**rejected** because elapsed cycles feed supported `-zroomcost 1`; changed
quantized eviction costs can change the finer source of a non-associative
decimator. A query inside the measured interval also changes that score.
No corrected-default native speed claim is made.

Its first build omitted the existing driver `-O3` suffix because global flags
do not include per-unit options. Broad layout drift and 68-versus-70 LTRANS
are not production-default evidence; files remain under `texture-first`.
Later tools require identical actual IR options. A strict-between-cursors
host-only V3 precursor remains under `texture-fill3`; it has no target speed
claim. A native launch without the environment script invoked no compiler;
its prepared evidence is intact and final `native-v2` uses the actual toolchain.
