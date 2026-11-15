# PS2 light-table RAM, pass 12 — 2026-11-15

The final candidate removes the PS2 engine's persistent RGBA copy of each
colormap light table. The GS driver still owns its original 8,192-byte
canonical-index table, with the same IDs, generations, palette lookup,
lighting rows, keyed transparency and lifetime rules. The generic RGBA APIs
and driver-table layout remain compatible.

This is a RAM change. No CPU, GPU, VRAM or FPS gain is claimed. The exact five
files were merged with the tested PB change in source commit
`d4fbc7c319f19edc70eda220c3323d0350504c98`. Publication remains root-owned.
The component evidence below starts from the frozen 602-file source at
`85bd9716b1ed114d46f95831c64a87623038eeaa`; the final section separately binds
and tests the real canonical build of the merged source.

## Exact representation and lifetime

The old getter allocated 32,768 persistent zone bytes per table, expanded
8,192 engine indices through `HWR_GetTexturePalette()`, then submitted those
RGBA values to the driver. The driver immediately reverse-mapped the RGB
values into a separate 8,192-byte allocation.

The PS2 getter now passes the engine indices and that same forward palette
directly. A single private `lt_store(id, rgba_or_palette, optional_indices)`
implements both the existing RGBA driver API and the new indexed API. Each
entry still computes the exact forward-RGB to reverse-index roundtrip.
`pal_build` still selects the **lowest** index for duplicate RGB values;
lookup misses still become index zero. Copying source indices directly would
change colors under some duplicate-color/flash palettes and is rejected by
an executed source control.

The private store remains one `noinline,noclone` body in the actual full LTO
engine. On a compiler-guaranteed little-endian target, its typed packed
`RGBA_t` member supplies `c.rgba & 0x00ffffff`; other byte orders or unknown
orders keep the original byte expression. The actual EE type is four bytes,
alignment one, with channel offsets 0/1/2/3. No cast or stronger pointer
alignment is introduced. The actual linked loop uses `LWL 3`/`LWR 0` on the
formed palette pointer, followed by the RGB mask.

`PS2_NOOPT_LIGHTTABLE_INDEXED` retains the complete original two-argument
private store and generic adapters, omits the new APIs, and selects the
original engine allocation path. Non-PS2 engine paths keep the generic RGBA
path. No light-table limit, row count, entry count, shader option, graphics
format or numeric precision is reduced.

The cached RGBA allocation is not an independent historical submission
source. Both original generic engine Create/Update functions overwrite all
8,192 RGBA entries from the **current** forward palette before every
submission. A stable ID submits nothing. Driver context restart retains its
canonical tables; driver-only clear and stale engine IDs retain their
original behavior. The host corpus tests those temporal distinctions.

The within-call ownership argument is separately archived at
`build/nov15-next12/lighttable-v2/ownership-v1/ownership-report.json`, with all
602 original sources, the complete callback bodies and every actual reclaim
registration. Overlay flush emits already queued state/words; it does not
call `ramp_get`, texture upload, a game tick or a palette setter. Ring rollover
services DMA/VU ownership and unlocks texture-cache data. Actual default
allocation uses `ps2_spill.c`, restricted to the game thread, then nonfatal
zone allocation. The four registered reclaim hooks free audio/model/cache or
already empty Lua slabs. The forward palettes are static/`PU_STATIC`; source
colormaps are `PU_LEVEL`, outside the purgeable/cache tag sets. Lua cache
userdata invalidation does not execute a script or GC step. A failure returns
before reading input, or unwinds/exits without resuming the store.

That source argument is supplemented by the actual SDK/engine allocator
binding described below, rather than assuming the optional leak-trace
wrappers implement the shipping allocator.

## Frozen source and host evidence

The final five-file source is
`build/nov15-next12/lighttable-v2c-src/src`. The checked public replay is
`tools/ps2/next12_lighttable_replay.py`,
`next12_lighttable_candidate.json` and
`next12_lighttable_candidate.patch`. Replaying the patch verifies every one
of the 602 source files; exactly five differ. The null backend supplies the
new API stubs without changing the driver structure.

The final host stage is
`build/nov15-next12/lighttable-v2c/host-v1`. Its exact source/tool bytes,
headers, commands, executables and logs remain immutable. Five policies pass:
PS2 default, original opt-out, non-PS2 engine fallback, unknown-endian portable
RGB fallback, and ASan/UBSan. Each policy executes:

- 1,142 temporal operations and 11,157,029,376 compared full-state bytes;
- 67,895,296 RGB checks: all 24-bit RGB values at each packed base offset
  0/1/2/3, plus all channel-byte/alpha combinations at each offset;
- full table contents and guards, actual typed `H`/colormap state, driver
  pointers/generations, palette hash, CLUT packets/cache/images, debug text,
  outputs and the geometry-cache colormap fingerprint.

The corpus includes independent forward/reverse/screen palettes, duplicates,
misses, default/nondefault tables, scripted changes, stable IDs, before-update
flushes, source/palette aliasing, byte-aligned inputs, context/clear/reuse,
generation wraps, allocation failures and all 1,023 usable IDs. Only the
intentionally deleted engine allocation fields/storage are excluded in the
PS2 comparison; zero candidate allocations are asserted. Other policies
compare that storage too.

Nine actual source mutations are executed and rejected: direct source-index
copy, wrong forward palette, wrong miss value, wrong generation, wrong debug
sample, omitted update flush, unconditional stable-ID update, alpha leakage
and a lost red bit.

`lighttable-v2c/policy-v1/policy-report.json` separately compiles the complete
real `hw_cache.c`, real driver and null driver. It verifies complete default
preprocessed tokens/code/data/ordered RELAs/frames against a guard-free
control, and complete original opt-out restoration against the baseline. It
also records the actual EE size/alignment/channel/endianness witness.

## Actual full LTO and linked machine code

The final isolated combined context is
`build/nov15-next12/lighttable-combined-v2`: all 602 baseline sources plus
these exact five table files and the separately tested PB scratch file
`08e3ce0c810ba69f957ab8aaa0082e28bcdb1e3f7a3939f8d0da1e8133e2e350`.
The build uses all 203 C and two VU inputs, default O2/G8/LTO/features, and the
exact baseline generated configuration. The complete original PB-only
Hardware engine is the table component's matching control.

The combined ELF is
`f70138cae13a72782012e753d7f93ab7e32ccaba39833b40555f26ed93f268d6`.
Its allocated text size 4,373,952, `_end` 8,934,576 and `_gp` 6,763,248 equal
the original baseline. No padding, reserve, allocator or capacity adjustment
was used to obtain that result. The shared store is 760 bytes with its
original 96-byte frame. `HWR_Lighting` retains its complete three getter calls
and 176-byte frame.

`binding-v1/binding-report.json` reconstructs complete resolved text/padding,
ordered exact-target RELAs/addends, data, literals, capacities and frames:
26 reference roots and 25 candidate roots. These include the live getter,
shared store, indexed update, generic driver adapters, all actual getter
caller code, clear/palette paths, complete CLUT allocation/build/cache,
driver setup/flush and the actual memcpy/memset wrappers. The generic engine
Create/Update bodies and indexed Create wrapper are linker-discarded in the
candidate; no game execution is claimed for those discarded bodies.

Exact `--emit-relocs` replay first checks every original allocated byte,
address, size, alignment and original defined symbol. Its retained RELAs
identify the precise selected address of merged literals, including repeated
`"0"` strings. Generated CSWTCH numbering is comparable only through complete
value-table bytes/alignment and its no-RELA policy. Twenty-two actual binary
mutations reject, including an alternate identical aligned string pointer.

`allocator-v2/allocator-report.json` adds four complete engine spill/zone
roots and two complete SDK archive members. It resolves actual libc
`malloc` to `__wrap__malloc_r`, `Spill_Alloc` and the nonfatal zone path;
the complete 32-byte malloc member text and 2,648-byte `_malloc_r` input
section, padding, symbols, PDRs and ordered RELAs are retained. Five actual
allocator mutations reject. A low-bit HI16 addend can preserve rounded
instruction bits; independent authoritative archive-member metadata identity
rejects that change, without claiming the emitted bits expose it.

`packed-load-v1/packed-report.json` retains the actual full-LTO palette-load
instructions, complete resolved ELF binding and disassembly, plus four
load/mask mutation controls. It proves the actual `LWL`/`LWR` packed access;
it is not an unaligned emulator microbenchmark or a CPU timing claim.

## Component native process memory

The two completed runs are under
`lighttable-combined-v2/game-v1`, pinned by readiness
`18fb9de9921979ff88d42a8f9df99892c271714e6f119a6e7302c0937911987d`.
All 1,128 frozen source/IR/build/tool/reference inputs and full non-UI PCSX2
settings are checked before/after. The only diagnostic ELF change is the
audited local-RNG initializer patch; normal shipping RNG remains unchanged.
The executed diagnostic hash is
`ad4d04e9fe745692c8d3d51cb7b11443d0e7a83289535940ef7f3e3e5ec4b33b`.

Readiness checks eight initial symbol ranges. Whole-ELF initializer-only
invariance transfers the complete shipping graph, and the post-run checker
independently resolves all 25 candidate roots, four spill roots and two SDK
members into the actually staged/executed ELF. This is actual full-game
machine-code evidence, not a separate-TU performance surrogate.

| Native 350-level-frame scene | Combined live/peak/global-peak reduction vs original | Table incremental reduction vs PB-only | Arena change | Elapsed ms, original → combined |
| --- | ---: | ---: | ---: | ---: |
| MAP02 | 74,880 B | 65,600 B | 0 B | 29,354 → 29,347 |
| MAP01 fog/NPOT | 71,776 B | 65,600 B | 0 B | 26,895 → 26,900 |

Both scenes remove two observed tag24 allocations: 65,568 allocated bytes,
65,536 payload bytes. The remaining 32-byte reduction is retained as an
observed residual across other tag allocations, with complete per-tag
block/allocated/payload deltas. No exact per-allocation cause is assigned
without a trace. Free zone space increases by the measured live-use
reductions. Largest free space increases by 74,880/71,872 bytes versus the
original scenes.

Both native scenes retain exact arena sizes, libc availability/peak,
132,560-byte main-stack high-water use, spill use/peak, evictions, cache
registry capacity and all sampled texture/cache residency. All 407 ordered
polygon-hash rows, ten loader hashes, two light-table log rows and eleven
hardware-memory sample rows match both original and PB-only controls.
Failed allocation attempts remain seven in MAP02 and decrease nine to eight
in fog. The reporting gate permits only nonincreasing failures; the earlier
equality-only rejection and exact old/new checker bytes are preserved in
`reporting-failure-policy-v1`. The actual games were not rerun for that
reporting correction.

Raw reports are `gamecheck-map02-v1/game-report.json` and
`gamecheck-fog-v2/game-report.json`. Elapsed values are single observations;
they establish no CPU/FPS gain or regression rate.

The MAP02 wrapper stops on `ZQUIT DONE` before the later quit/config-save
sequence finishes. Its generated output `reference.cfg` is therefore an
observed zero-byte interrupted save; the staged input remains the exact
76-byte file. Fog's generated save is complete. No output-save/config
equality is claimed by this pressure gate; normal shutdown/storage checks
remain separate.

## Images and preserved earlier work

Two three-frame full-image runs pass at
`lighttable-combined-v2/images-v2`, using exact original and PB-only profiles
and complete 320x200 PPM references at level frames 20/70/140. All six files,
1,152,090 bytes including their headers, match both references byte for byte.
`image-report-0.json` and `image-report-1.json` retain every full-file hash and
runtime proof; readiness is
`8e1bfc9810b03e80bb25c2985422b749196bf62e1d3209178657203bfef63969`.
The full non-UI profile, emulator and every frozen input remain unchanged.

The first separate-store candidate remains frozen at
`build/nov15-next12/lighttable`. Its MAP02 run proved 65,568 fewer live zone
bytes, but `_end`/`_gp` grew 768 bytes and actual arena capacity lost 4,096
bytes. It was paused, not normalized or adopted. Its source, build, native
run and public tool bytes remain immutable. The shared-store v2 and
opt-out-guard v2b stages also remain preserved, including the null-backend
guard failure caught by complete-TU checking. The final v2c corrects that
guard and adds only the approved packed RGB extraction closure.

## Final canonical source, machine code and native games

The final source snapshot is `build/nov15-next12/final/source`, containing all
602 files at commit `d4fbc7c319f19edc70eda220c3323d0350504c98`. The real
default Hardware build uses all 203 C plus two VU inputs and its actual
generated version configuration. Its shipping ELF is
`efc963c7876a5f2a1c395baf5ba9982b68c6db1e56811d7a5e8a690ba840ab20`.
`_end` and `_gp` equal the original baseline, without capacity normalization.

`build/nov15-next12/lighttable-final/finalbind-v4/final-binding-report.json`
is the final machine-code admission. It verifies every final source against
the exact executed component source, all actual canonical source/header pins
and all 205 IR inputs. Complete instructions, ordered exact-target
RELAs/addends, frames and referenced data/capacities match the executed
component for 25 table/caller/CLUT/wrapper roots plus four spill roots.
Strict final binding checks named data extents, complete BSS ranges, every
merged literal use, and frame stack/save instruction witnesses.

`allocator-final-v1` resolves the complete actual SDK malloc members and
shipping spill/zone bodies. Both final local-seed diagnostic ELFs are fully
bound, not only eight selected byte ranges: seed `0x01b2d3f5` is
`6766d019d20ef841ee12604011c466231c4cb9ba213e0467ed92f313bbdfb122`, and seed
`0x9e3779b9` is
`6abf462952a14316408645966dd845f0178e568b24ff4179dd795fd4e5dde47d`.
Twenty-two final binary mutations and five SDK/spill mutations reject.

The final PB-active opt-out is compared against a **matched current-generated-
header control** that restores exactly the original five table files. All
5,869 complete input function sections and their ordered relocation records
match between those controls. Twenty-one explicit live legacy
table/caller/CLUT/packet/wrapper roots are resolved with exact data/capacities
and frames, including the actual out-of-line `pk_alloc`, `pk_flush` and
`vu_chunk_end` bodies. The historical source85 compilation had different
CLUT inlining; it is not substituted for this current control. The historical
26-root disposition addendum is
`lighttable-final/historical-disposition-v1/disposition-report.json`, SHA
`1c5d519f5042b711765f4155a6268e89883f9659bc88225572603fb2ae30a406`.
It records 21 exact current live names, four actual compiler variants, and
`emit_fast_fan` absent from both complete inventories. That absent function's
two source calls map to the fully bound current `emit_poly.constprop.0`.
The four renamed variants are `emit_piece.lto_priv.0`, `lit_fast_poly`,
`val_end` and `val_gsreg`. Complete actual variants and source callers total
50 bound bodies with 249 referenced data records on each control side; no
standalone body is fabricated for an absent historical spelling.

Both controls preserve their rejected initial LTO-log stages. The only
reviewed diagnostic difference from the canonical merge is the explicit
serial partition count, 70 to 68 LTRANS jobs after the indexed APIs disappear.
Every other diagnostic byte matches after independently byte-identical input
path binding. The unchanged frozen merged engines are linked in fresh
directories; neither compilation nor LTO is rerun to change a report.

The actual final native pressure report is
`build/nov15-next12/final-hardware-pressure/audit-v1/report.json`, SHA
`311272e731058f24589563a4d0f5ca3c93e8fa44153bf7ab0ef99ab6ccb0254e`.
It admits the exact final PB/LT/SDK graphs and actually staged final diagnostic
ELFs. Four fresh final-build runs, two fresh seed-two original baselines and
two explicitly reused seed-one baselines cover both local seeds in the two
Hardware scenes. Each reaches 350 native Hardware
level frames. In **each seed**, the final build has the same measured
reductions: 74,880 bytes in MAP02 and 71,776 bytes in fog. Arena size, libc
headroom/peak, stack, spill, cache/VRAM capacities and residency remain exact;
all 407 ordered geometry rows per run match. Fog's allocation-failure count
improves nine to eight; no increase is permitted.

Final combined accounting is separate from the component incremental
32-byte observation: after the observed 65,568-byte tag24 allocation removal
and PB's requested payload removal of 9,216/6,144 bytes, the final whole-zone
residual is **96 bytes in MAP02 and 64 bytes in fog**, in both seeds. Complete
raw tag/checkpoint deltas remain in the final report. No exact placement or
header cause is inferred.

The final integration matrix at `build/nov15-next12/final-seed1` checks 13
complete PPM pairs. Separate unchanged-normal-RNG games add six complete PPM
pairs, for **19 actual final image pairs**. The normal-game audit is
`build/nov15-next12/final-unmodified-audit/report.json`; four games preserve
their full saved configurations and reach the late sound/graphics shutdown
observations. This separate normal-save evidence closes the pressure
wrapper's interrupted post-marker save limitation without altering or
rerunning the earlier raw pressure results. No general CPU/GPU/FPS or
stability-rate improvement follows from these runs.
