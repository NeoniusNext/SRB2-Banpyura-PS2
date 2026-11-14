# Exact remaining tilted Software drawers — third pass, 2026-11-14

The reference is the **immediately preceding completed pass**, frozen in
`build/nov14-next3/baseline-src/src`, with the corresponding complete Software
ELF SHA256 `30fef9b92c863a3a03a135a8198217232ea48ff6463cfd7ef22a2c5ee26851fc`.
It includes both earlier passes' changes; old Git HEAD is not the reference.
`tools/ps2/prepare_next3_baseline.py` recreates that source from the verified
`next3_baseline.patch/.json` when the original build snapshot is absent.

The only production file edited for this Software work is `src/r_draw8.c`.
The existing ordinary tilted span, NPOT descriptor cache, column drawers,
cropped UI path, headers, build flags and assets remain as they were at the
start of this pass.

## Change and exactness

The six other textured power-of-two tilted drawers now retain the texture shifts/mask
for their complete span under the existing `PS2_OPT_DRAW` gate. The four that
use changing slope lighting also retain the lighting-table pointer, colormap
offset and column cursor. The translucent and water variants retain the blend
pointer. Shared expression macros expand back into the original expressions
when optimization is disabled, avoiding a second copy of perspective setup or
pixel sampling. Every `ds_x1` result is written back, as in the preexisting
optimized ordinary tilted drawer. Floor-sprite drawers leave `ds_x1` unchanged,
as before.

This removes global parameter loads that framebuffer byte stores otherwise
force the compiler to repeat. The engine's framebuffer storage is separate
from the storage of these renderer global variables. Source and lookup-table
**contents** remain ordinary live reads; no `restrict`, composed lookup table,
immutable-data assumption or persistent cache is added. Per-call addresses
and dimensions are captured again every call. The host tests deliberately
write into source, lighting, translation, blend and background contents that
alias the framebuffer.

No floating/double expression, fixed-coordinate conversion, perspective block
size, light interpolation, texture index, transparency decision, blend order,
source read or pixel-store order was changed. Texture dimensions, framebuffer
sizes, precision policies, fallbacks, graphics, geometry, physics, features,
compatibility limits and capacities are preserved. There is no new allocation,
RAM table, VRAM use or DMA traffic.

## EE measurements and limits

EE GCC 15.2.0, ordinary release `-O2`, `-G8`, `-mno-abicalls`, `-fwrapv`;
PCSX2 2.6.3 with the standard 32 MiB configuration. Both complete extracted
functions are in one functions translation unit, separated from the driver and
external global definitions. **Neither translation unit uses LTO.** The actual
function definitions have `noinline`, `noipa` and `externally_visible`
attributes. This prevents fixture-specific buffer-address/alias proofs from
silently optimizing the reference. The lighting and conversion helpers come
from the frozen complete `r_draw.c`, without replacement math.

The real frozen complete release-LTO Software ELF was also inspected. Its two
main/remainder pixel loops load 7–8 tracked global parameters per pixel in the
lit translucent/water drawers, 6–7 in the splat, and 2–4 in the floor-sprite
drawers. The reported reference overhead therefore exists in the actual game.
Disassembly and the GP-relative symbol/loop inventory are saved in
`build/nov14-next3/sw-tilted-objects`.

The coordinator's final complete release-LTO Software ELF,
`build/ps2/SRB2.ELF`, SHA256
`20f669c6e79f14b2593e79e63323d19cad9623e08e9474b00f726d3e14ec846f`,
was checked separately. All **18 detected pixel-loop regions** across the six
candidate drawers contain **zero tracked parameter-global loads**; the saved
parameter addresses/shifts remain in registers. Its stack frames match the
object evidence below. Full linked disassembly, symbol offsets, loop ranges
and load inventories are in
`build/nov14-next3/sw-tilted-objects/elf-report.json` and adjoining `.asm` files.

The benchmark compares every framebuffer/canary byte, all lighting entries and
`ds_x1` on the EE before timing each row. It checks all 144 rows, exits 0 and
reaches `TILTED DONE`. Six drawers, texture sizes 16/256, lengths
1/8/16/32/100/320 and sparse/dense coverage are measured with 1,024 calls,
best of three paired repetitions; execution order alternates between pairs.

For 100 pixels, texture size 256 and dense coverage, cycles per call:

| Drawer | Before | After | Reduction |
| --- | ---: | ---: | ---: |
| Tilted translucent span | 7290 | 5455 | 25.17% |
| Tilted water span | 7426 | 5611 | 24.44% |
| Tilted splat | 6589 | 4942 | 25.00% |
| Tilted translucent splat | 7389 | 5665 | 23.33% |
| Tilted floor sprite | 3785 | 3296 | 12.92% |
| Tilted translucent floor sprite | 4688 | 4001 | 14.65% |

Across all equally weighted rows, the sum of costs falls **21.98%**. There are
134 faster rows and **10 slower one-pixel rows**, with increases of 3–11
cycles per call. Every sampled length above one pixel improves. Per-drawer
sums improve 24.93%, 24.28%, 24.29%, 23.00%, 13.65% and 14.75%, in the table's
order. There is no claim of universal per-call improvement, full-game FPS gain
or physical-PS2 cache timing from this microbenchmark.

Measured ELF and data:

```text
build/nov14-next3/sw-tilted-ee/TILTED.ELF
SHA256 5c6a1b62228b3682c1383984001afce7a790eebb91d813774677f727ee8163df
build/nov14-next3/sw-tilted-ee/run.log
build/nov14-next3/sw-tilted-ee/report.json
```

## Code, RAM, stack and disabled-policy control

Compiling the **complete actual** `r_draw.c` translation unit with release EE
flags, LTO disabled for inspectable object sections, passes `-Werror`:

| All relevant sections | Before | After | Change |
| --- | ---: | ---: | ---: |
| `.text*` | 70000 B | 69880 B | −120 B |
| `.rodata*` | 567 B | 567 B | 0 |
| `.data*` plus `.sdata*` | 32 B | 32 B | 0 |
| `.bss*` plus `.sbss*` | 15743 B | 15743 B | 0 |

Retaining more parameters in registers increases individual drawer stack
frames: translucent 80→112 B, water 96→128 B, splat 80→112 B,
translucent splat 80→112 B, floor sprite 48→80 B and translucent floor sprite
64→80 B. This is a disclosed 16–32 B call-stack cost, **not** a measured
increase in the game's maximum stack usage. It is not a heap or static-RAM
allocation. The object decrease is not presented as a measured final-game ELF
or peak-RAM decrease.

With `PS2_NOOPT_DRAW`, all actual `r_draw.c` `.text*` and `.rodata*` section
names, sizes and bytes are identical to the frozen reference. SHA256 of framed
section names/sizes/contents is:

```text
b9cc0617d4d509df6fa5f434a36166151079d7056ec534cdc052e820fcbdaf80
```

The six complete expanded disabled-policy function bodies also have identical
non-whitespace SHA256 `8fa27f4bb902455003050c18845835e34973c5453cfe011e26eb77b2f2f11b59`.
The complete preprocessed files differ only in another worker's new unused
`P_GetSpawnLine` declaration from `p_setup.h`; the entire object-code identity
check remains exact. Commands, sections and result hashes are saved in
`build/nov14-next3/sw-tilted-objects/report.json`.

Final production source SHA256:

```text
src/r_draw8.c
b7d50138c4421dfd661bdd034dd6ae4a4698d824917c172182d50eb78da7cad5
src/r_draw8_npo2.c (unchanged)
fd742989d9ea3da30acadc60b8eb2b2e2c8e434e201522e01d5fd12fcbd8086a
src/v_video.c (unchanged)
821a47ba9af7ee6db7ae31801b691599da234352f62b4b1587913300eaf59968
```

## Differential coverage

`tools/ps2/next3_render_tilted_hosttest.{c,py}` independently extracts all six
complete frozen/current drawers, the real lighting helper and the real slope
conversion helper. Every policy passes **139,104 cases**, comparing
**1,388,169,216 complete framebuffer/canary bytes**. The four policies are the
current float profile, double slope profile, `PS2_NOOPT_DRAW`, and full
`PS2_NOOPT`; each is compared to its own frozen precision policy. ASan+UBSan
passes the same current-profile cases without diagnostics.

Coverage includes all eleven power-of-two sizes from 2 through 2048;
framebuffer widths 1/7/16/31/319/320/512/640; nineteen positive lengths through
640, both sides of all 16-pixel block boundaries, and negative empty spans;
left/middle/right origins; signed fixed boundaries and coordinates beyond
2^32; both signs of texture/depth/light steps; clamp extremes; sparse/dense
transparency and the two-byte floor-sprite source; all background offsets;
and changing source/lookup identities. Seven alias modes cover source bytes,
shifted source overlap, colormap/lighting contents, translation contents,
blend contents, overlapping water background, and combined lookup/source
content overlap. Globals themselves remain outside the framebuffer, matching
the renderer contract. All lighting entries and the public `ds_x1` state are
compared after every call.

The eight negative controls pass: each of the four policies detects deliberate
framebuffer corruption, and each detects a changed source lookup in the
**actual extracted candidate code**. No replacement drawing algorithm or new
golden output is used. Profile/NOOPT_DRAW framebuffer-row hash is
`1f8c91807d67d5a0`; double/full-NOOPT hash is `5160fa5f23a20de2`.

Full feature-enabled engine builds, gameplay frames, heap/stack diagnostics
and final ELF measurements belong to the coordinator's integration report.

## Reproduce

```bash
source /workspace/.onboarding/environment.sh
python3 -B tools/ps2/prepare_next3_baseline.py \
  --out build/nov14-next3/reproduced-baseline
python3 -B tools/ps2/next3_render_tilted_hosttest.py \
  --out build/nov14-next3/recheck-tilted \
  --base-src build/nov14-next3/reproduced-baseline/src --negative-controls --sanitize
python3 -B tools/ps2/next3_render_tilted_objectcheck.py \
  --out build/nov14-next3/recheck-tilted-objects \
  --base-src build/nov14-next3/reproduced-baseline/src
python3 -B tools/ps2/next3_render_tilted_hosttest.py \
  --out build/nov14-next3/recheck-tilted-ee \
  --base-src build/nov14-next3/reproduced-baseline/src --ee-only
source /workspace/.onboarding/emulator-env.sh
python3 -B tools/ps2/run_pcsx2.py \
  --elf build/nov14-next3/recheck-tilted-ee/TILTED.ELF \
  --log build/nov14-next3/recheck-tilted-ee/run.log \
  --until 'TILTED DONE' --marker 'TILTBENCH ' --timeout 180
# Read-only verification of the coordinator's saved full game ELFs:
python3 -B tools/ps2/next3_render_tilted_objectcheck.py \
  --out build/nov14-next3/recheck-tilted-objects \
  --base-src build/nov14-next3/reproduced-baseline/src --elf-only \
  --baseline-elf build/nov14-next3/baseline/software/SRB2.ELF \
  --candidate-elf build/ps2/SRB2.ELF
```
