# Exact fog and solid-color drawers — seventh pass, 2026-11-14

The reference is the immediately preceding completed pass, frozen in
`build/nov14-next7/baseline-src/src`. Its complete release-LTO Software ELF is
`build/nov14-next7/baseline/software/SRB2.ELF`, SHA256
`69141b1a0f5eb44f505b6ced3f69162fa43d214d4620d3aa48f2adce0429e31d`.
`tools/ps2/prepare_next7_baseline.py` reconstructs the frozen source. Git HEAD
is not the comparison reference.

Only `src/r_draw8.c` changes for this drawer component. The other drawer files,
including the preceding exact NPOT reciprocal/cache work, remain byte-identical
to the immediate reference. Restoring only the seven changed function bodies
and removing their two metadata macros reconstructs the complete reference
`r_draw8.c` byte-for-byte. Audit and isolated diff:
`build/nov14-next7/sw-fog-source/{change-audit.json,change.diff}`.

## Change and exactness

The four tilted fog, solid-color, translucent solid-color and water solid-color
drawers retain their lighting-table address, colormap offset and column cursor
for the complete multi-pixel span. Both blended tilted drawers retain the blend
address. The two ordinary translucent/water solid-color drawers retain their
blend address; the fog column retains its colormap address. All changes use the
existing `PS2_OPT_DRAW` gate.

Each original `R_CalcSlopeLight` call still runs first with the original public
cursor. A one-pixel or reversed tilted span then executes its exact original
single-pixel expression and returns, preserving the original do-once behavior
and cursor increment before the store. Longer spans publish the resulting
`ds_x1` before return. There is no call or callback inside these drawing regions.

The framebuffer comes from `Impl_VideoSetupBuffer`'s allocated `vid.buffer`;
`Impl_SliceScreens`, `V_Init` and `d_main.c` derive `screens[]`/`topleft` from it.
The renderer global variables occupy separate static storage. Their per-call
values can therefore be retained across framebuffer stores without changing
behavior. The lighting-array entries, planezlight entries, colormap contents,
blend contents and water background remain ordinary live reads, including when
lookup/background data overlaps destination bytes. No `restrict`, composed
lookup, sample cache or persistent pointer cache is introduced.

No floating-point expression, lighting interpolation, coordinate conversion,
source sample, transparency decision, blend order, destination bound or pixel
store order changes. Ordinary solid-color drawers retain their inclusive
`dest <= deststop` bound, including clipping of reversed-size counts. The
original zero-length tilted lighting division is not redefined. Graphics,
geometry, physics, precision, dimensions, features and compatibility limits
remain unchanged. There is no new allocation, static table, VRAM use, DMA
traffic, reserve, capacity threshold or fallback.

## Complete actual code and memory

Complete actual frozen/current `r_draw.c` translation units compile with the
existing release EE flags and `-Werror`, without LTO for inspectable sections:

| Sections | Before | After | Change |
| --- | ---: | ---: | ---: |
| `.text*` | 63,056 B | 63,392 B | +336 B |
| `.rodata*` | 567 B | 567 B | 0 |
| `.data*` plus `.sdata*` | 32 B | 32 B | 0 |
| `.bss*` plus `.sbss*` | 15,743 B | 15,743 B | 0 |

Only the seven intended drawer text sections change. Every other text section,
all data, the rodata-content multiset and BSS names/sizes match. NOBITS BSS is
compared as sizes, not as file-backed bytes. All seven object stack frames are
unchanged: 16 B for each tilted drawer and zero for the three ordinary drawers.
Every multi-pixel loop has zero tracked metadata loads, zero cursor stores and
zero stack accesses. There is no per-call heap/static-RAM or stack increase.
The 336 B text increase is a real code-size cost, not a memory-saving claim.

With `PS2_NOOPT_DRAW`, every complete actual text/rodata section name, size and
byte matches the reference. Framed digest:
`b9cc0617d4d509df6fa5f434a36166151079d7056ec534cdc052e820fcbdaf80`.
All seven disabled-policy preprocessed drawer token streams also match,
SHA256 `89da1b49ec0113086c78a609741e7b79383b4c8c4cd72b5186aeb3e0f3a290ec`.
Objects, disassembly and controls are in
`build/nov14-next7/sw-fog-single-objects`.

The frozen complete release-LTO Software ELF independently retains 4/4/5/5
tracked metadata loads and one cursor store per pixel in the four tilted
drawers, and one metadata load per pixel in each other drawer. These costs
therefore exist in the game. `next7_render_fog_codecheck.py` further compares
all seven complete fixture reference functions against the frozen actual
post-LTO `engine_lto.o`: **every instruction stream matches exactly** after
only relocation fields and symbol spelling are normalized. Registers,
constants, relative branch distances, stack instructions, relocation targets,
addends and text padding remain exact. The fixture matches Software's complete
`viddef_t` layout and five-element `screens[]` array, including GP eligibility.
The final complete release-LTO Software ELF,
`build/ps2/SRB2.ELF`, SHA256
`dbc708685926cf5f8368f3a29f5846a1f309bb25b9c5c3f79ab9dbd0cb267767`,
passes the separate linked audit: all **seven pixel loops** have zero tracked
metadata loads, zero cursor stores and zero stack accesses. All seven linked
stack frames remain 16/0/16/16/0/16/0 B in the function order used below.
Disassembly and inventories:
`build/nov14-next7/sw-fog-final-linked/elf-report.json`.

Every complete candidate drawer instruction stream in the final actual
post-LTO `engine_lto.o` also **exactly matches the measured fixture**, under the
same relocation checks. Its SHA256 is
`a8592311111063376fdc311fb6e61995b1d8fb9efb12d39713bd170b147e836d`.
Each function has 7–22 checked RELA targets and explicit addends; the parser
requires nonempty relocation data. Both versions' actual complete 376 B
`R_CalcSlopeLight` helper also matches the fixture's complete instructions and
all three exact float-literal byte values. Helper pool addresses are resolved
using the object's recorded GP and compared by literal contents, retaining the
load instructions. This establishes the actual default-LTO code underlying the
selected kernel measurements; it does not establish physical cache behavior or
whole-game FPS. Code proof: `build/nov14-next7/sw-fog-final-code/code-report.json`.
This final binding includes the separately proved composite-recovery fix in
`docs/PS2_TEXTURE_RETRY_NEXT7_20261114.md`. Complete-engine size and gameplay
frame comparisons belong to integration.

## EE measurements

The complete frozen/current drawers and actual frozen lighting helpers are
extracted into a functions translation unit separate from the driver/globals.
Neither TU uses LTO; actual drawers have `noinline`, `noipa` and
`externally_visible` attributes. This prevents fixture-specific alias proofs.
EE GCC 15.2.0, existing release `-O2`, `-G8`, `-mno-abicalls`, `-fwrapv`;
PCSX2 2.6.3 with ordinary 32 MiB EE RAM.

All **168 paired rows** compare every framebuffer/canary byte, all lighting
entries and final cursor before timing. The run exits 0 and reaches `FOG DONE`.
Seven drawers, lengths 1/2/7/8/16/32/100/320 and three lighting/background modes
use 2,048 calls, best of five paired repetitions with alternating order.

There are **159 faster rows, nine ties and no slower rows**. The equally weighted
sum of measured costs falls **28.95163%**. All **147 rows of two or more pixels**
improve, by 2.04073–36.72711%, with a summed reduction of 29.17520%. Among the
21 one-pixel rows, 12 improve and nine tie; their summed cost falls 2.66845%.
All one-pixel ties are in the three ordinary drawers. Per-drawer summed
reductions, in the table's order, are 32.42179%, 19.90556%, 32.32000%, 33.68771%,
16.44870%, 29.72324% and 8.59401%.

For 100 pixels, lighting/background mode 0, cycles per call:

| Drawer | Before | After | Reduction |
| --- | ---: | ---: | ---: |
| Tilted fog | 4149 | 2664 | 35.79% |
| Translucent solid color | 1450 | 1153 | 20.48% |
| Tilted solid color | 3852 | 2467 | 35.96% |
| Tilted translucent solid color | 4852 | 3070 | 36.73% |
| Water solid color | 1757 | 1460 | 16.90% |
| Tilted water solid color | 4861 | 3282 | 32.48% |
| Fog column | 1127 | 1028 | 8.78% |

Measured ELF: `build/nov14-next7/sw-fog-final-ee/FOG.ELF`, SHA256
`6b683d159e1c56af67725db6e3f928cf769deb129f833bdcf72ff1d80abe44c1`.
Full stream, runner status and numeric rows are in `run.log`, `runner.log` and
`report.json` in that directory.

An initial all-length local-cursor version had 12 slower one-pixel rows, costing
4–7 extra cycles/call. It was rejected in favor of the explicit original
single-pixel path. Its source, object and EE evidence remains under the
`sw-fog-source`, `sw-fog-objects` and `sw-fog-ee` directories. Intermediate
simplified-ABI fixture measurements are retained in `sw-fog-single-ee` and are
not used as final timings. The final fixture's complete native instruction
matching is described above.

Selected kernel cycles do not measure whole-game FPS, physical-PS2 cache/power
behavior, or frame-time variance. Complete-game integration belongs to the
coordinator's report.

## Host differential proof

`next7_render_fog_hosttest.{c,py}` extracts all seven complete actual frozen/current
drawers and the real frozen lighting helpers. Each of float, double,
`PS2_NOOPT_DRAW` and full `PS2_NOOPT` passes **68,886 cases**, comparing
**4,679,618,952 complete framebuffer/canary bytes**, all slope-lighting entries
and final `ds_x1`. ASan+UBSan passes the same float cases without diagnostics.
Both sides always use the same preexisting precision policy. Float/NOOPT_DRAW
row hash is `bf03e69adb10b6b8`; double/full-NOOPT hash is `f3db594d48bc9168`.

Coverage includes framebuffer widths 1/7/16/31/319/320/512/640; 22 lengths
−17/−1/0/1 through 640; left/middle/right origins; every short-loop boundary;
39 varying light start/step modes, positive/negative/tiny/zero light steps and
both clamp extremes; changing source/colormap/blend identities; nontrivial row
pitches and all three background offsets. Zero-length tilted calls are excluded
because the original helper divides by zero; reversed tilted calls retain a
valid preexisting lighting array and exercise the original do-once result.

Seven alias modes cover colormap/lighting contents, the sampled source byte,
blend contents, overlapping water background, combined lookup/source overlap,
and forward/backward shifted water background. The two ordinary bounded drawers
also test spans crossing the final row, reversed counts, and the original
inclusive destination endpoint. Aliases affect data contents; renderer global
variable storage remains separate from framebuffer storage.

All **16 negative controls** fail as expected: output corruption, a changed
actual lookup, a changed final cursor, and copying actual blend-table contents
before the first store, across all four policies. The stale-table mutation fails
on overlapping blend contents after pixel stores have changed those contents;
no replacement drawing algorithm or new golden output is used. Evidence:
`build/nov14-next7/sw-fog-final-host`.

## Focused integration add-on

`next7_render_fog_fixture.{py,lua}` creates a deterministic opaque 1×1 Doom patch,
selecting the existing solid-color flat drawers. Existing sloped base planes get
that texture; existing renderable FOFs cycle through fog, translucent solid and
ripple-water solid planes. Geometry, slope values and collision flag bits remain.
READY logs configured plane counts rather than claiming visibility/drawer calls.
The add-on can load after the preceding NPOT sprite fixture so its eight
floor-sprite objects remain while FOF textures/flags select this pass's drawers.

All used Lua getters/setters, ffloors iterator factory, dollar expressions and
zero-is-false conditions were audited against the actual engine. The real
`R_CheckSolidColorFlat` tests exact width=height=1. Lua audit and archive manifest
are in `build/nov14-next7/fog-addons`. Archive SHA256:
`24786cb9232b0e8395068c62c4e22ac2aba5e07ebb1207da44b8947886086c53`.

Frozen production `src/r_draw8.c` SHA256:
`7e1da2dc6c2f569cf3b31f8edea147b54fe81710902ea947d1feb9073016ac55`.
Copy: `build/nov14-next7/sw-fog-source/src/r_draw8.c`.

## Reproduce

```bash
source /workspace/.onboarding/environment.sh
python3 -B tools/ps2/prepare_next7_baseline.py
python3 -B tools/ps2/next7_render_fog_sourceaudit.py \
  --out build/nov14-next7/recheck-fog-source \
  --base-src build/nov14-next7/baseline-src/src
python3 -B tools/ps2/next7_render_fog_hosttest.py \
  --out build/nov14-next7/recheck-fog-host \
  --base-src build/nov14-next7/baseline-src/src --negative-controls --sanitize
python3 -B tools/ps2/next7_render_fog_objectcheck.py \
  --out build/nov14-next7/recheck-fog-objects \
  --base-src build/nov14-next7/baseline-src/src \
  --baseline-elf build/nov14-next7/baseline/software/SRB2.ELF
python3 -B tools/ps2/next7_render_fog_hosttest.py \
  --out build/nov14-next7/recheck-fog-ee \
  --base-src build/nov14-next7/baseline-src/src --ee-only
source /workspace/.onboarding/emulator-env.sh
python3 -B tools/ps2/run_pcsx2.py \
  --elf build/nov14-next7/recheck-fog-ee/FOG.ELF \
  --log build/nov14-next7/recheck-fog-ee/run.log --until 'FOG DONE' --timeout 180
python3 -B tools/ps2/next7_render_fog_report.py --out build/nov14-next7/recheck-fog-ee
# After the coordinator's final complete Software build:
python3 -B tools/ps2/next7_render_fog_objectcheck.py \
  --out build/nov14-next7/recheck-fog-objects --elf-only \
  --base-src build/nov14-next7/baseline-src/src \
  --baseline-elf build/nov14-next7/baseline/software/SRB2.ELF \
  --candidate-elf build/ps2/SRB2.ELF
python3 -B tools/ps2/next7_render_fog_codecheck.py \
  --out build/nov14-next7/recheck-fog-code \
  --fixture-object build/nov14-next7/recheck-fog-ee/functions.o \
  --baseline-engine build/nov14-next7/baseline/software/engine_lto.o \
  --candidate-engine build/ps2/obj/engine_lto.o
```
