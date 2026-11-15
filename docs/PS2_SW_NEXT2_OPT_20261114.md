# Exact sprite columns and cropped UI — next iteration, 2026-11-14

The reference is the **current previous iteration**, frozen in
`build/nov14-next/baseline-src/src`, with the corresponding complete Software ELF
in `build/nov14-next/baseline/software/SRB2.ELF`. It includes the previous exact
two-entry-per-axis NPOT slope descriptor cache. It is not the older Git HEAD.
That cache and `src/r_draw8_npo2.c` are unchanged in this work.

Production edits are confined to `src/r_draw8.c` and `src/v_video.c`, under the
existing `PS2_OPT_DRAW` gate. No assets, geometry, physics, features, capacities,
quality settings, framebuffer sizes, texture sizes or compatibility limits change.

## Runtime changes

The shade, translated and translated-translucent column drawers now retain their
source and lookup-table pointers in locals for the complete column, as the
ordinary drawers already do. A framebuffer byte store can alias any global under
C's character aliasing rules; the frozen EE code therefore reloads two, three or
four global addresses per pixel. Locals eliminate these repeated pointer loads.
Table and source **contents** still participate in ordinary byte aliasing: no
`restrict`, cached palette result, precalculated composite lookup, or assumed
immutable pixel data is introduced. The engine's destination-buffer contract
excludes writing into the storage of the global pointer variables themselves.

The optimized power-of-two translated-translucent loop uses one destination and
coordinate pair. This avoids the EE compiler's saved-register prologue in the
first local-pointer experiment and reduces code size. Its sampled indices, lookup
order and pixel-store order match the original two-pixel loop. The NPOT
normalization and legacy single-wrap step remain unchanged; the change does not
reinterpret unusual dimensions or replace their fallback behavior.

`V_DrawCroppedPatch` now selects the same four existing pixel lookup expressions
once per post, instead of calling a pixel function pointer for each pixel. Screen
base, stride, lookup pointers and post length are kept in locals, matching the
existing uncropped PS2 drawer. All crop, scale, split-player boundary, top/bottom
clip, post offset and fixed-coordinate calculations remain the same. Persistent
`v_colormap`/`v_translevel` assignments and early-return ordering are retained.
The hardware renderer and PS2 menu-recording delegation still run before the
software loop. The original path remains selected when drawing optimization is
disabled.

There are no new allocations, static lookup caches, VRAM uses, invalidation rules
or capacity thresholds. Pointer values are captured afresh on every call.

## EE measurements

EE GCC 15.2.0, release `-O2`, `-G8`, `-mno-abicalls`, `-fwrapv`; PCSX2 2.6.3,
ordinary 32 MiB EE RAM. Both complete extracted source functions reside in the
same ELF. They are compiled in a separate translation unit **without LTO**,
against external globals, to prevent fixture-specific address/alias proofs. The
timed driver is common to both. The frozen complete release-LTO game ELF was
also inspected: its actual three drawers retain the same global pointer reloads
as the separately compiled reference functions.

The initially tried same-translation-unit LTO fixture proved too much about its
own buffers and hoisted reference pointers too. Its timings are discarded; they
are not used below. These measurements isolate the edited code and are not a
claim about complete-game FPS, physical-PS2 cache timing or total GPU work.

The coordinator's final full release-LTO Software ELF, `build/ps2/SRB2.ELF`,
SHA256 `30fef9b92c863a3a03a135a8198217232ea48ff6463cfd7ef22a2c5ee26851fc`,
was inspected separately. All three candidate column hot loops retain the
source/lookup addresses in registers, with no global pointer loads inside those
loops and no stack frame. The cropped UI's per-pixel indirect call is absent;
its remaining calls are setup/math/menu-recording calls. Its final linked stack
frame is also 112 B. Linked function disassembly and
`candidate-elf-report.json` are saved in `build/nov14-next/sw-engine-final`.

Columns measure 1,024 calls, best of five paired repetitions. The suite has three
drawers, lengths 1/8/32/100/200/480 and heights 64/128/72/100. Heights are
irrelevant to the unmasked shade/translated paths but are included consistently.
The final runner exits 0, emits all **72 rows**, and reaches `COLUMNS DONE`.

| Drawer / workload | Cycles before | Cycles after | Reduction |
| --- | ---: | ---: | ---: |
| Shade, 100 pixels | 2050 | 1753 | 14.49% |
| Translated, 100 pixels | 2145 | 1650 | 23.08% |
| Translated-translucent, 100 pixels, height 128 | 3067 | 2457 | 19.89% |
| Translated-translucent, 100 pixels, height 72 | 3455 | 2762 | 20.06% |

Across all equally weighted column rows the sum of costs decreases **19.48%**:
60 rows improve, 10 match, and the two one-pixel power-of-two
translated-translucent rows cost **81 versus 77 cycles** (+4 cycles). The
one-pixel NPOT rows match at 89 cycles. This small short-call cost is retained
and disclosed; no universal per-call speedup is claimed. Per-drawer sums decrease
14.62% (shade), 23.27% (translated) and 20.04% (translated-translucent).

The UI fixture has sizes 8x8/32x64/63x127/160x100, all four pixel lookups and
dense/sparse posts. It measures 32 calls, best of five paired repetitions, and
compares every framebuffer/canary byte on the EE before timing each row. It
exits 0, emits all **32 rows**, and reaches `UI DONE`, with no pixel mismatch.
All rows improve, by **28.38–41.27%**; the equally weighted sum decreases
**37.85%**. Examples:

| Cropped UI workload | Cycles before | Cycles after | Reduction |
| --- | ---: | ---: | ---: |
| 8x8 opaque, dense posts | 3734 | 2473 | 33.77% |
| 63x127 opaque, dense posts | 296883 | 174359 | 41.27% |
| 63x127 translated-translucent, dense posts | 400926 | 254349 | 36.56% |

Final measured pairs and machine-readable results:

- `build/nov14-next/sw-columns-scalar-ee/{COLUMNS.ELF,run.log,report.json}`.
- `build/nov14-next/sw-ui-ee/{UI.ELF,run.log,report.json}`.

## Code, static RAM and stack

Actual complete translation units, compiled with the release EE flags but with
LTO disabled for inspectable sections, pass `-Werror`:

| All `.text*` sections | Before | After | Change |
| --- | ---: | ---: | ---: |
| `r_draw.c` | 70256 B | 70000 B | -256 B |
| `v_video.c` | 31208 B | 31552 B | +344 B |
| Combined | 101464 B | 101552 B | +88 B |

All `.rodata*`, `.data*`/`.sdata*` and `.bss*`/`.sbss*` totals are unchanged.
The cropped UI function's EE stack frame decreases from **144 to 112 B**; this
is a 32-byte reduction for that call, not a measured reduction in the game's
maximum stack use. The final translated-translucent drawer uses no stack frame.
Object sections and disassembly are in
`build/nov14-next/sw-engine-final`, with `object-report.json`.

With `PS2_NOOPT_DRAW`, the full preprocessed `r_draw.c` and `v_video.c` sources
match their frozen references, ignoring whitespace/blank lines (`diff -w -B`
has no differences). SHA256 of the non-whitespace preprocessed output:

```text
r_draw.c  eaae4490de2c901e61691c2731268f344a17f1680bff6594b61192e644411b8d
v_video.c 0986c772a854b8dfec2303049941c0d29a9c9e2ae4d2a263f691399e9d67bfab
```

Final production-source SHA256:

```text
src/r_draw8.c
e55b5227991ed64316d8c9ef45234f91844aeaf63b27716ce2eb7b084dd5bc25
src/v_video.c
821a47ba9af7ee6db7ae31801b691599da234352f62b4b1587913300eaf59968
src/r_draw8_npo2.c (unchanged)
fd742989d9ea3da30acadc60b8eb2b2e2c8e434e201522e01d5fd12fcbd8086a
```

## Equivalence coverage

`tools/ps2/next2_render_columns_hosttest.{c,py}` extracts all three complete
functions independently from the frozen and candidate sources. Each profile,
`PS2_NOOPT`, and `RANGECHECK` policy passes **115,800 cases**, comparing
**3,102,603,583 complete framebuffer/canary bytes**. An ASan+UBSan build passes
the same cases without diagnostics. Result hash is `633190481275b1c0`.
Coverage includes:

- Framebuffer widths 1/7/16/31/319/320/512/640, 23 lengths including empty
  ranges and odd/even counts through 480, randomized destination rows/columns.
- Every signed fixed-coordinate boundary, zero, both signs of step, overflow
  boundaries, power-of-two masking, and valid legacy NPOT wrap/normalization.
- Heights 1/2/3/5/24/63/64/72/100/128/256/1023/1024/16383/16384/32767;
  padded source storage keeps signed-coordinate tests within real allocations.
- Source, translation, lighting and blend-table identities changing on every
  call, nonidentity lookup contents, and **3,000 cases where source/table
  contents alias the framebuffer**, including writes into those contents.

`tools/ps2/next2_render_ui_hosttest.{c,py}` extracts the complete cropped drawer,
four original pixel helpers, HUD alpha arrays, actual flag/struct declarations,
and frozen fixed-division helpers. Each profile, `PS2_NOOPT`, hardware-delegate
and PS2-menu-hook policy passes **26,048 cases**, comparing
**26,289,725,440 complete framebuffer/canary bytes**. ASan+UBSan passes the same
profile cases without diagnostics. The profile/NOOPT hash is `8b1fb337de09361e`;
hardware and PS2-hook policies compare against their own matching references.
Coverage includes:

- All four pixel lookups, alpha values 0–15, blend values 0–7, all HUD alpha
  remaps at each translucency setting 0–10, and alternating colormap addresses.
- Empty, zero-length, dense and sparse multi-post columns; odd dimensions
  including 63x241 and patches through 257x1024; fractional crop origins,
  empty/negative crop extents, and nine scale factors from 1/16 through 3.
- Odd screen dimensions through 640x512, row padding, negative/offscreen
  origins, snap/scale flags, both split-player halves and their crop boundary,
  null screens, render-none, hardware delegation and menu-recording returns.
- Persistent lookup-pointer state and blend-table/menu/hardware call counts.

Both suites detect intentional output corruption and a second negative control
that mutates a lookup in the **actual extracted candidate source**, across all
regular policies. There is no replacement/golden algorithm copy in either
fixture. Complete engine builds, gameplay frames and memory checks are tracked
by the coordinator's iteration report.

## Reproduce

```bash
source /workspace/.onboarding/environment.sh
python3 -B tools/ps2/next2_render_columns_hosttest.py \
  --out build/nov14-next/recheck-columns \
  --base-src build/nov14-next/baseline-src/src --negative-controls --sanitize
python3 -B tools/ps2/next2_render_ui_hosttest.py \
  --out build/nov14-next/recheck-ui \
  --base-src build/nov14-next/baseline-src/src --negative-controls --sanitize

python3 -B tools/ps2/next2_render_columns_hosttest.py \
  --out build/nov14-next/recheck-columns-ee \
  --base-src build/nov14-next/baseline-src/src --ee-only
python3 -B tools/ps2/next2_render_ui_hosttest.py \
  --out build/nov14-next/recheck-ui-ee \
  --base-src build/nov14-next/baseline-src/src --ee-only
source /workspace/.onboarding/emulator-env.sh
python3 -B tools/ps2/run_pcsx2.py \
  --elf build/nov14-next/recheck-columns-ee/COLUMNS.ELF \
  --log build/nov14-next/recheck-columns-ee/run.log \
  --until 'COLUMNS DONE' --marker 'COLBENCH ' --timeout 180
python3 -B tools/ps2/run_pcsx2.py \
  --elf build/nov14-next/recheck-ui-ee/UI.ELF \
  --log build/nov14-next/recheck-ui-ee/run.log \
  --until 'UI DONE' --marker 'UIBENCH ' --timeout 180
```
