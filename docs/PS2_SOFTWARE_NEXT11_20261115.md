# Exact visplane overlap scan — eleventh pass, 2026-11-15

The all-renderer `761efcda8` candidate is **rejected in
Hardware**: its 16-byte text growth moved the linked heap boundary and reduced
the fog scene's actual zone arena by 4,096 B. Its complete bindings under
`sw/shipping` remain immutable historical evidence. Source `85bd9716b` narrows
the cursor to `NOHW` Software builds; Hardware, including its Software fallback,
retains the original scan. Scoped release gates pass under
`sw/shipping-nohw`; the original native run and manifest are preserved.

The reference is `69a3e5319a0a2ee0afda9c460c4a158139d221dd`, with all 602
source files frozen under `build/nov15-next11/baseline-src/src`. Its complete
default Software and Hardware builds remain under
`build/nov15-next11/baseline/{software,hardware}`. This component's isolated
Software build replaces only `r_plane.o` in the complete 189-object canonical
IR graph, using the frozen headers and original generated configuration.
Other pass-11 source changes do not enter that isolated build.

## Change and exactness

Only the empty-overlap scan in `R_CheckPlane` changes, when both `PS2_OPT_REND`
and `NOHW` select the standalone Software build. After the original overlap test and first top sample,
it retains top and bottom cursors through the remaining scan. This removes
the repeated bottom-pointer load and index-to-byte conversion. The range
intersection/union setup and complete allocation, metadata copy and reset
source remain unchanged. Restoring this one helper reconstructs the entire
original `r_plane.c` byte for byte.

No cursor is formed on an empty or reversed intersection. The bottom cursor
is formed only after the first top sample equals `0xffff`. Every later column
still reads top first and reads bottom only when top accepts it. Both strips
remain live `UINT16` reads, including valid shared and shifted strip contents.
The first rejected sample, inclusive final column, resulting `x`, reuse/split
decision and all final metadata writes remain exact. There are no new resource
bounds, persistent caches, allocations, calls or callback opportunities in the
scan. All floating expressions, sampling, lighting, pixel work, precision,
geometry, physics, features and capacities remain unchanged.

The narrow `PS2_NOOPT_CHECKPLANE_CURSOR` switch, `PS2_NOOPT_REND`, full
`PS2_NOOPT`, Hardware builds and ordinary non-profile builds select the original scan. No header
changes belong to this component. In particular, prior bounds MMI, slope-light,
NPOT/water blending and sprite optimizations remain intact.

## Complete portable proof

`next11_sw_checkplane.py` extracts the complete original and production
functions into separate translation units, with `noinline`, `noipa`, no LTO
and the real frozen headers. Its typed fixture uses the complete real
`visplane_t`, all clip strips, padding and guards. The original complete
`new_visplane` source supplies host reuse/allocation/growth behavior; capture
hooks record request bytes, tag, owner, frees, all buckets and both free-list
pointers. Each comparison restores the same addresses and compares the
complete resulting state and returned pointer literally.

Fresh `NOHW` profile, narrow opt-out, renderer opt-out, full opt-out, Hardware
fallback without `NOHW`, and Software ASan+UBSan each
pass **194,434 cases**, comparing **2,970,951,520 complete state bytes**.
Widths are 1/7/16/31/319/320/512/640; 21 lengths include reversed, zero,
one-column and full-width intersections. Coverage includes both array ends,
empty/first/middle/last top or bottom rejection, random contents, four valid
strip alias modes, both ffloor hash paths, no free node, reusable nodes,
undersized nodes, remaining free-list nodes and existing bucket heads.
All 65,536 possible halfwords are tested as both first-top and first-bottom
samples. An additional 16,000 cases vary both interval endpoints. Original
empty-plane states include `minx=640` and `minx=INT32_MAX`, without forming
an invalid strip cursor.

All **15 executed actual-source controls** are detected: changed top sentinel,
bottom sentinel and final output, across all five policies. Sanitizers report
no diagnostics. Every opt-out and Hardware fallback also selects the original
complete preprocessed helper tokens. Fresh scoped evidence is
`build/nov15-next11/sw/shipping-nohw/host`; earlier `sw/host-final` remains
the original all-renderer candidate's portable evidence.

## Actual code, caller and linked addresses

Ten complete real `r_plane.c` objects compile with the default EE release
flags and `-Werror`, always using the same frozen baseline headers:

| Complete Software object scope | Reference | Cursor |
| --- | ---: | ---: |
| All `.text*` | 48,696 B | 48,712 B |
| `.rodata*` | 260 B | 260 B |
| `.data*` / `.sdata*` | 40 B | 40 B |
| `.bss*` / `.sbss*` | 146,548 B | 146,548 B |
| Complete helper text/padding | 1,256 B | 1,272 B |
| Helper stack frame | 48 B | 48 B |

Only the selected helper's text changes. Every other complete instruction and
ordered RELA, every writable initializer and every BSS name/capacity matches.
All three opt-out policies and the Hardware fallback have identical complete
code, preprocessed helper tokens, frames and data. Hardware's complete
`r_plane.c` object stays original. Fresh evidence is `sw/shipping-nohw/objects`.
The 16-byte text increase is a footprint tradeoff, not RAM savings. This
component reserves no extra heap, stack, static writable storage or VRAM.

The timed reference and production helpers match their respective **complete
actual default-LTO bodies**, including register allocation, scheduling,
branches, constants, padding, exact frame records and all ordered RELA targets
and explicit addends. Declaration-only fixture symbols are explicitly mapped
to the same real `new_visplane.lto_priv.0` and `viewwidth` definitions. The
entire **21,096-byte `R_StoreWallRange` caller**, all **604 ordered RELAs**,
its **336-byte frame** and all three helper call sites remain unchanged.
Those call sites are at offsets 12,044, 12,128 and 16,808.

The complete actual **368-byte `new_visplane` section / 364-byte symbol**,
all **22 RELAs**, full literal sections and its **48-byte frame** are copied
directly from the real LTO object into the native fixture. Both canonical
memory wrappers also match their actual game bodies and frames: complete
`memcpy` wrapper 256 B and `memset` wrapper 160 B, each with a 32-byte frame.
The fixture retains the actual `--wrap=memcpy` and `--wrap=memset` link policy.

`next11_sw_checkplane_codecheck.py` resolves every selected object field using
the corresponding exact map input contribution, defined symbol, `_gp`, link
wrapper and explicit addend. Complete helpers, caller and wrappers bind to
every filled body/padding byte in both real Software game ELFs and the exact
executed fixture ELF. The graph includes **123 complete data nodes**, their
initial pointer RELAs and capacities, and **nine complete string literals**.
The input `finecosine` pointer, for example, resolves to the exact initialized
`finesine` table and addend; it is not treated as a zero placeholder.

The full caller also references `cv_secbright`, whose default is merged string
`"0"`. Many aligned copies have identical contents, so the gate refuses to
guess an address from a string search. `next11_sw_linker_relas.py` replays each
exact pinned canonical link with only `--emit-relocs`, without rerunning the
engine LTO merge. Before using a retained linker RELA, it requires every
original allocated byte/address/size/alignment and every original defined
symbol name/type/binding/address/size to remain exact. GNU ld adds only
zero-byte `.tm_clone_table` and `.eh_frame` sections; zero-size `_gp` and
`__TMC_END__` markers may be associated with those sections while keeping
their exact addresses and sizes. Those harmless metadata differences are
recorded explicitly.

Retained RELAs independently fix each merged literal's linker-selected
address. The gate also checks its entire original NUL-inclusive content and
alignment, then reconstructs every final field. All **23 actual binary controls**
are rejected: helper/caller/dependency registers and frames; caller RELA target,
addend, type and location; initialized-data R32 target/addend; object and linked
literal bytes; linked helper/caller/dependency instructions and call pointer;
and executed helper register/symbol-size, dependency and wrapper bytes.
One control redirects the default pointer to a different aligned identical
`"0"`; it is rejected despite equal string contents.

The original component execution binding and controls are in `sw/executed-code`.
The exact linker replays are `sw/linker-relas-{baseline,isolated}-final`.
Earlier rejected checker attempts remain separate: they rejected only the
additional empty sections or zero-size markers' section ownership, before
the final gate recorded those differences explicitly.

| Isolated component artifact | SHA256 |
| --- | --- |
| Original Software engine LTO | `417eca1b22fc1254134cb1eeac94f7edcaee35d25a2f1d474488836b6f1e4545` |
| Original Software ELF | `ab5de93e035b0942a1024e3dd2ac6235ef6285a7a6406e77259946f852faaefe` |
| Cursor-only Software engine LTO | `d97a46fe218e89ee7047f4e05aa60e5b569dd5fcef75c32177bd2b1faff988ac` |
| Cursor-only Software ELF | `cca7cbaa623edf7da9ed44b5391d1bb4d44e1082aec87cdfe06cb3616d31a7d4` |
| Exact executed native ELF | `f93f2accf39713d6e91cd28e73cb6c6f196407961fde7db2f10168087c79db68` |
| Executed production helper object | `233806a5a33c098b267d5f65086a7b049bc289146dac6eee508da40ec20a79a2` |
| Native run manifest | `0ae6648d3e4c0e1a2ae8bb80f2eecb111d4269d93586529742a85d11d5a15321` |

These are the original isolated component artifacts. The final scoped release
binding follows; no Hardware cursor improvement is inferred from these rows.

## Scoped release binding

Source `85bd9716b1ed114d46f95831c64a87623038eeaa` changes only `r_plane.c`
among all 602 baseline source files. Complete canonical builds, frozen source
and generated configuration are under `release/{software,hardware,source}`
in the pass directory. Software has 189 actual IR inputs; Hardware has 203 C
inputs and two original VU objects, 205 total.

Each renderer has its own complete current-header narrow old-scan control.
Exactly `src__r_plane.o` changes; every other copied IR byte is independently
checked. Software's 1,272-byte helper binds to the existing executed
**production** body. Hardware's 1,256-byte helper binds to the existing executed
**reference** body. Both keep the original 48-byte helper frame. Software
retains its entire 21,096-byte caller and all 604 ordered RELAs; Hardware
retains its own original 21,072-byte caller and all 609 ordered RELAs. Both
retain the 336-byte caller frame, three call sites, complete dependency/wrappers,
data and resolved linked fields.

Hardware additionally passes strict complete original helper/caller/dependency/
wrapper code, ordered RELAs, frame, full data, literal and capacity identity
against the real frozen original Hardware engine and ELF. It uses that
baseline's own checked retained-RELA replay. No capacity or pooled-offset
normalization was needed. Hardware claims original-code retention, with no
new-helper cycle reduction.

Fresh preprocessed selected-function witnesses compare the narrowed Software
source to the unchanged executed production source, and Hardware fallback to
the unchanged executed reference source. Their respective token hashes are
`3f9b674c24645bc975d04b42a29b7f0627e8e5a2a5af7523e9c4279c31391e50` and
`ebc194756b5848bc597a703cd2bd0a58566e87d62061aefa706c17a26b29de2d`.
Complete actual instruction/frame/data/ELF checks independently confirm these
selections, so no repeated native benchmark or rewritten native provenance is
needed.

Both renderer gates reject **23 actual binary controls each, 46 final controls**.
Each rejects a pointer redirected to another aligned identical `"0"` literal.
Separate video ABI witnesses retain Software's actual 56-byte `vid` and
Hardware's actual 68-byte `vid`, without normalizing either. These are separate
capacity witnesses, not direct dependencies of the timed scan/caller.

Final `_end` and `_gp` match their respective original baselines exactly:

| Renderer | `_end` | `_gp` | Selected helper |
| --- | ---: | ---: | --- |
| Software | 7,735,260 | 6,003,568 | Cursor, 1,272 B |
| Hardware | 8,934,576 | 6,763,248 | Original, 1,256 B |

Every Hardware section size and its 11,261,544-byte ELF file size also matches
the original. No lost zone capacity is dismissed or normalized away. Actual
controlled scene/cache pressure remains the coordinator's integration gate.

| Final scoped artifact | SHA256 |
| --- | --- |
| Software ELF | `778dde15e01077762a68941e7c32b3670559194cecca1e0d961f4a5b4f927be4` |
| Hardware ELF | `fc7a2919a519cc5b23be4ded8e295df41fda68c90350326cfa1113b63e46eff5` |
| Scoped binding summary | `bcba449423ece7d9b9d53a23312561bd2fdb5c7c8f262137edbd6ee1b91826d5` |

Final gates and real mutation files are under
`sw/shipping-nohw/{software,hardware}/code`; complete controls/replays are their
sibling directories. `sw/shipping-nohw/binding-summary.json` pins the source,
all inputs, selected executed sides, complete reports and capacity witnesses.

The context tool retains complete raw diagnostics. Only object-path spellings
whose actual bytes independently match the same checked input identity may
normalize. A nested context log's invocation line is removed only when it
exactly equals the structured report's recorded command; it is metadata, not a
discarded warning. Real Hardware/nested-context positives pass; six changed
warning, partition-count, stdout, duplicate/missing-warning or input-byte
controls reject. `sw/context-diagnostic-normalization` archives both before and
after tool bytes and hashes. Existing byte-pool reports retain their earlier
builder version and are not rewritten or described as using the later helper.

## Native exactness and costs

EE GCC 15.2, release `-O2`, `-G8`, full features; PCSX2 2.6.3, ordinary 32 MiB
EE RAM, cycle rate/skip zero and `EnableEECache=false`. The run reaches
`CHECKPLANE DONE` and the runner exits zero. Before timing, **17,026 cases**
compare **222,836,288 complete state bytes**, including untimed controlled
allocation/growth requests and full reset guards. The actual bottom-sentinel
source mutation is separately rejected on a one-column case.

The **392 Software paired cost rows** cover fourteen lengths
−1/0/1/2/3/7/8/16/31/32/33/100/320/640; fully empty scans and first/middle/last
top or bottom rejection; both ffloor hash paths; warm and explicitly
cache-invalidated modes. Each row compares the complete result before timing.
Warm rows use 2,048 calls; invalidated rows use 32 individual calls with
`FlushCache(0)` and `FlushCache(2)` outside the timestamps. Both use best of
five paired repetitions, alternating order. Interrupts are masked only during
timing, and their previous enabled state is restored afterward.

Timed collision paths use the **actual unchanged recycled-plane dependency**.
A shared driver re-primes its original acyclic free node before each call;
its identical re-prime and dispatch work is included in both costs. Allocation,
free and fatal exits are untimed capture hooks, and timed allocation/free hooks
abort if reached. The final timed hook count is **zero**. These costs therefore
describe standalone Software recycled-plane scan/helper work, not a zone
allocator benchmark or a Hardware improvement.

| Mode | Faster / equal / slower | Summed per-call cost reduction |
| --- | ---: | ---: |
| Warm | 166 / 30 / 0 | 13.101835308% |
| Cache invalidated | 166 / 30 / 0 | 13.113685602% |
| Equally weighted combination | 332 / 60 / 0 | **13.107757777%** |

Every reversed/zero row and one-column fully empty row ties. Every other row
is faster. For 100 columns without an ffloor, warm CP0 Count ticks per measured
call, including the shared driver:

| Rejection position | Reference | Cursor | Reduction |
| --- | ---: | ---: | ---: |
| None: complete empty scan | 1668.005371 | 1371.005371 | 17.8057% |
| First top | 487.005371 | 483.005371 | 0.8213% |
| First bottom | 495.005371 | 494.005371 | 0.2020% |

`sw/native-staged` pins complete compiled inputs before/after execution,
original and staged ELF/object/source/header bytes, raw logs, runner arguments,
runner return zero, emulator binaries and archived proof tools. It archives
every full INI before and after, then reparses **every non-UI/non-table setting**
and the required standard profile, rather than checking only four selected
keys. All critical settings and compiled inputs remain exact. All **14 parser
mutations** are rejected. `sw/native-negative-staged` independently pins the
early failing actual-source control. The full game code gate reparses these
exact archived inputs and the complete native log without changing them.

Explicit invalidation under this emulator does not establish physical-console
cache timing. Selected helper costs do not establish weighted game FPS,
frame-time variance, physical-console speed or power. Whole-game images,
actual zone/cache pressure and final combined artifacts remain integration
gates. Controlled game comparisons use the coordinator's fixed-local-RNG
diagnostic copies; shipping randomness is unchanged.

## Reproduction

Use the configured SDK and emulator from [TOOLCHAIN.md](TOOLCHAIN.md), one
granted EE worker and the globally serialized emulator slot. Complete original
canonical builds, maps and generated configuration are required. Use fresh
output directories and preserve the recorded evidence.

```sh
source /workspace/.onboarding/environment.sh
python3 -B tools/ps2/next11_sw_checkplane.py \
  --out build/next11-recheck/host --base-src build/nov15-next11/baseline-src/src \
  --sanitize --negative-controls
python3 -B tools/ps2/next11_sw_checkplane_objectcheck.py \
  --out build/next11-recheck/objects --base-src build/nov15-next11/baseline-src/src
python3 -B tools/ps2/next11_sw_checkplane_context.py \
  --build build/nov15-next11/baseline/software --snapshot build/nov15-next11/baseline-src \
  --out build/next11-recheck/isolated --production-source src/r_plane.c
python3 -B tools/ps2/next11_sw_linker_relas.py \
  --build build/nov15-next11/baseline/software --out build/next11-recheck/base-relas
python3 -B tools/ps2/next11_sw_linker_relas.py \
  --build build/next11-recheck/isolated --out build/next11-recheck/new-relas
python3 -B tools/ps2/next11_sw_checkplane.py \
  --out build/next11-recheck/native --base-src build/nov15-next11/baseline-src/src \
  --baseline-engine build/nov15-next11/baseline/software/obj/engine_lto.o --ee-only
source /workspace/.onboarding/emulator-env.sh
SRB2_PCSX2_SLOTS=1 python3 -B tools/ps2/next11_sw_checkplane_run.py \
  --input build/next11-recheck/native --out build/next11-recheck/native-staged
python3 -B tools/ps2/next11_sw_checkplane_codecheck.py \
  --out build/next11-recheck/code --fixture-dir build/next11-recheck/native-staged \
  --baseline-engine build/nov15-next11/baseline/software/obj/engine_lto.o \
  --baseline-elf build/nov15-next11/baseline/software/SRB2.ELF \
  --baseline-map build/nov15-next11/baseline/software/SRB2.map \
  --candidate-engine build/next11-recheck/isolated/obj/engine_lto.o \
  --candidate-elf build/next11-recheck/isolated/SRB2.ELF \
  --candidate-map build/next11-recheck/isolated/SRB2.map \
  --baseline-linker-relas build/next11-recheck/base-relas \
  --candidate-linker-relas build/next11-recheck/new-relas --negative-controls
```

For the separate native source control, build with `--negative-ee-only` into
a fresh native directory and run it with `next11_sw_checkplane_run.py
--negative`. For combined-source callers, first build a narrow opt-out using
`next11_sw_checkplane_context.py --optout` atop that exact complete canonical
build and its matching frozen source tree, then replay that control's own
linker RELAs. Each renderer needs its own matched current-header reference,
game ELF, map and pinned build report. Hardware also passes
`--candidate-variant reference` and the real original Hardware
`--historical-engine`, `--historical-elf`, `--historical-map` and
`--historical-linker-relas`; its retained original reference side is not
reported as a new-helper performance gain.
