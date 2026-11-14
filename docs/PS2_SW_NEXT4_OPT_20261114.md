# Exact ordinary floor sprites and splats — fourth pass, 2026-11-14

The reference is the immediately preceding completed pass, frozen in
`build/nov14-next4/baseline-src/src`, with complete release-LTO Software ELF
SHA256 `20f669c6e79f14b2593e79e63323d19cad9623e08e9474b00f726d3e14ec846f`.
`tools/ps2/prepare_next4_baseline.py` reconstructs that source from the frozen
patch and checksum manifest. Git HEAD is not the comparison reference.

The only production file changed for this Software work is `src/r_draw8.c`.
Only four ordinary power-of-two drawers and their shared expression macros
differ from this pass's frozen source. All other source text in the file is
byte-identical, including every preceding optimization. The isolated diff is
`build/nov14-next4/sw-sprites-source/change.diff`.

## Change and exactness

`R_DrawSplat_8`, `R_DrawTranslucentSplat_8`, `R_DrawFloorSprite_8` and
`R_DrawTranslucentFloorSprite_8` now retain their texture shifts/mask for the
whole span. Both translucent drawers also retain the blend-table address.
The existing `PS2_OPT_DRAW` gate controls these locals. Framebuffer bytes do
not overlap the storage of renderer global variables, so their values stay
constant throughout a drawer call. Source and lookup-table **contents** remain
ordinary live reads, including when those contents overlap framebuffer bytes.
There is no `restrict`, composed lookup, immutable-data assumption or cache.

After each existing integer coordinate advance, an empty GCC register-operand
assembly expression keeps the coordinates as a sequential pair. It emits no
instruction, does not access memory, and changes no register value at runtime.
This is the technique already used by the optimized ordinary span, translucent
span and water span before this pass. Without it, EE GCC retains many simultaneous
`x + k*step`/`y + k*step` partial sums for each eight-pixel group and spills them.

These are separate changes, measured separately below. No floating-point
expression or precision policy is touched. Both coordinate additions still
occur in the same order for each pixel, with the existing `-fwrapv` behavior.
The eight-pixel grouping, remainder condition, source type and sample index,
opacity/transparent-pixel tests, translation/lighting/blend order and pixel
stores are preserved. In particular, the splat's existing `0x3FFFFF` mask remains
in its grouped loop and its existing inclusive destination bound remains in
the remainder loop. The public `ds_x1` remains unchanged, as before.

There is no new allocation, heap/static table, VRAM use, DMA traffic, capacity
limit or fallback. Texture/framebuffer sizes, graphics, geometry, physics,
features, add-ons and compatibility limits are unchanged.

## EE measurements and separate contributions

EE GCC 15.2.0, ordinary release `-O2`, `-G8`, `-mno-abicalls`, `-fwrapv`;
PCSX2 2.6.3 with the ordinary 32 MiB profile. The complete extracted frozen and
current definitions are compiled in a translation unit separate from the driver
and global definitions, **without LTO**. Actual definitions use `noinline`,
`noipa` and `externally_visible`, preventing fixture-specific address/alias
proofs or identical-function merging. Two additional definitions isolate
parameter hoists alone and sequential advancement alone. Their framebuffer
results are checked against the reference on both host and EE.

The frozen complete game ELF was inspected independently: its main loops retain
12 tracked metadata-global loads per eight-pixel group in opaque drawers and
20 in translucent drawers. The remainder loops retain 2–4 such loads per pixel.
The reference's cost therefore exists in the game, rather than arising only in
the benchmark. Full disassembly, GP offsets and loop load inventories are in
`build/nov14-next4/sw-sprites-objects/report.json` and adjoining `.asm` files.

The coordinator's final complete release-LTO Software ELF,
`build/ps2/SRB2.ELF`, SHA256
`403462b11c3a09007a7c9c21dc198a113f99b103173212c8f3fbe2fdf976bc44`,
was inspected independently. All **eight detected pixel-loop regions** across
the four drawers contain **zero tracked parameter-global loads** and no stack
accesses. Across each complete candidate function there are also zero `lw`/`sw`
accesses through `sp`, versus 6–10 such loads and 7–11 such stores in the frozen
functions. Only saved-register `ld`/`sd` traffic remains. Stack frames in this
final linked game are exactly 48/48/32/48 B, confirming the 80 B reductions below.
Linked disassembly, loop ranges and complete stack-access inventories are saved
in `build/nov14-next4/sw-sprites-objects/elf-report.json` and adjoining `.asm` files.

All **112 EE rows** compare every framebuffer/canary byte for all three candidate
variants before timing. The run exits 0 and reaches `SPRITES DONE`. It covers four
drawers, texture sizes 16/256, lengths 1/7/8/16/32/100/320 and sparse/dense
coverage. Each row times 1,024 calls, best of three repetitions, with execution
order rotated among all four variants.

For 100 pixels, texture size 256 and dense coverage, cycles per call:

| Drawer | Before | After | Reduction |
| --- | ---: | ---: | ---: |
| Splat | 2184 | 1831 | 16.16% |
| Translucent splat | 2962 | 2423 | 18.20% |
| Floor sprite | 2675 | 2329 | 12.93% |
| Translucent floor sprite | 3585 | 3034 | 15.37% |

Across all equally weighted rows, the sum of combined costs falls **14.7131%**.
Every row improves, by **8.9752–28.6164%**. Per-drawer sums fall 14.8914%,
16.9265%, 12.2056% and 14.7508%, respectively. These are selected kernel
measurements, not whole-game FPS, physical-PS2 cache timing, power measurements
or a measured reduction in frame-time variance.

| Variant relative to frozen reference | Sum reduction | Faster / equal / slower |
| --- | ---: | ---: |
| Parameter hoists alone | 10.8334% | 94 / 0 / 18 |
| Sequential advance alone | 3.4485% | 112 / 0 / 0 |
| Both, retained production candidate | 14.7131% | 112 / 0 / 0 |

The isolated effects are not additive. Hoists alone increase three drawer stack
frames by 16 B; the combined candidate avoids that cost. No isolated ablation
replaces the combined production code.

Measured ELF and evidence:

```text
build/nov14-next4/sw-sprites-ee/SPRITES.ELF
SHA256 bde09b6ea0bc1a492da61c18178aff774c1b39094f8aa9feaf6564c0f2a86c94
build/nov14-next4/sw-sprites-ee/run.log
build/nov14-next4/sw-sprites-ee/report.json
build/nov14-next4/sw-sprites-ee/ablation-objects.json
```

## Code, RAM, stack and disabled control

The complete actual `r_draw.c` translation unit compiles with release EE flags
and `-Werror`, with LTO disabled for inspectable sections:

| All relevant sections | Before | After | Change |
| --- | ---: | ---: | ---: |
| `.text*` | 69880 B | 68760 B | −1120 B |
| `.rodata*` | 567 B | 567 B | 0 |
| `.data*` plus `.sdata*` | 32 B | 32 B | 0 |
| `.bss*` plus `.sbss*` | 15743 B | 15743 B | 0 |

All four actual drawer stack frames decrease by **80 B**: splat 128→48 B,
translucent splat 128→48 B, floor sprite 112→32 B and translucent floor sprite
128→48 B. This is per-call stack-frame evidence, not a measured decrease in
the complete game's maximum stack use. The object code decrease is not a
claimed final linked-game RAM decrease; integration measures the whole ELF.

With `PS2_NOOPT_DRAW`, every actual `.text*`/`.rodata*` section name, size and
byte matches the frozen reference. The framed code/rodata SHA256 is
`b9cc0617d4d509df6fa5f434a36166151079d7056ec534cdc052e820fcbdaf80`.
The four complete preprocessed drawer bodies also have identical non-whitespace
SHA256 `f39578cedb41c9cb4424f8075ee57a6c00ceff3002c33a9971e91e2fc0ce888b`.
Complete preprocessed files include other workers' changed declarations; object
code identity is exact. Commands and sections are saved in
`build/nov14-next4/sw-sprites-objects/report.json`.

Frozen final production source:

```text
src/r_draw8.c
SHA256 4936b879437709b5f6b8ab56426a6c03eccda8b492c616bbed00451a8179d2f1
build/nov14-next4/sw-sprites-source/src/r_draw8.c
```

## Differential coverage and integration fixture

`tools/ps2/next4_render_sprites_hosttest.{c,py}` extracts complete actual drawers
from the frozen and current sources. Each of the profile, `PS2_NOOPT_DRAW` and
full `PS2_NOOPT` policies passes **752,832 cases**, comparing **17,381,983,104
complete framebuffer/canary bytes** across current, hoists-only and advance-only
variants. Reference-versus-current alone accounts for 5,793,994,368 of those
bytes. ASan+UBSan passes the same profile cases without diagnostics. All policy
hashes are `f96e718eb0d91467`.

Coverage includes all eleven P2 sizes 2–2048; framebuffer widths
1/7/16/31/319/320/512/640; 24 lengths from empty spans through 640, both sides
of eight-pixel boundaries; left/middle/right origins; both signed-coordinate
overflow boundaries and every combination of twelve X starts/steps; both signs
of Y step; sparse and dense visibility; nonidentity and changing source,
translation, lighting and blend identities; source/framebuffer overlap, shifted
source overlap, colormap-content overlap, translation-content overlap,
blend-content overlap and combined overlap. Alias cases draw within the source
and lookup regions, so those contents can be overwritten. Renderer globals
themselves remain outside the framebuffer. Extra cases retain the inclusive
remainder bound at and beyond the last screen byte. Every case checks unchanged
`ds_x1` as well as complete output storage.

All six negative controls pass: each of the three policies detects deliberate
framebuffer corruption and a changed source read in the actual extracted
candidate code. No replacement drawing algorithm or new golden output is used.

`tools/ps2/next4_render_sprites_fixture.{lua,py}` creates the deterministic
`NX4_FLOOR.pk3` integration add-on. Its 64×64 non-solid Doom patch is decoded and
checked against every generated input pixel before packaging. The Lua follows
the existing `fxscene.lua` placement pattern and creates eight static ordinary
floor sprites: four opaque, four translucent, with normal and translated
variants. It also configures eligible existing horizontal renderable FOF planes
with a matching masked flat and alternating opaque/translucent alpha. Its
`READY` counts describe configured planes, not measured drawer calls. The paired
run driver supplies identical seeds/tics and validates `READY` plus capture
completion; complete engine/gameplay results belong to the coordinator report.

The first baseline fixture smoke exposed a test-add-on error: the nested loop
used `sector.ffloors` instead of calling its iterator factory. That diagnostic
run has no `READY` marker and is excluded from comparison evidence. The corrected
fixture uses `sector.ffloors()`, matching `sector_get`/`sector_iterate` and the
existing `luatests/lm_world.lua`. The FOF API audit confirms `t_slope`/`b_slope`
are readable (nil when absent), while `toppic`, `bottompic`, `fofflags`, `blend`
and `alpha` each have explicit read/write cases in `lua_maplib.c`. It writes no
slope/height field. Explicit angle assignment also avoids SRB2 Lua's numeric-zero
false value in the `and/or` shorthand, and `destscale` holds sprite scale fixed.
Corrected add-on SHA256:
`089fc51c320385e1921145e4e4d13538303a2aa75afb240b2f30a0d70f2db176`;
the generator also saves `NX4_FLOOR.expected.json` with every asset/input hash.

## Reproduce

```bash
source /workspace/.onboarding/environment.sh
python3 -B tools/ps2/prepare_next4_baseline.py \
  --out build/nov14-next4/reproduced-baseline
python3 -B tools/ps2/next4_render_sprites_hosttest.py \
  --out build/nov14-next4/recheck-sprites \
  --base-src build/nov14-next4/reproduced-baseline/src --negative-controls --sanitize
python3 -B tools/ps2/next4_render_sprites_objectcheck.py \
  --out build/nov14-next4/recheck-sprites-objects \
  --base-src build/nov14-next4/reproduced-baseline/src
python3 -B tools/ps2/next4_render_sprites_hosttest.py \
  --out build/nov14-next4/recheck-sprites-ee \
  --base-src build/nov14-next4/reproduced-baseline/src --ee-only
source /workspace/.onboarding/emulator-env.sh
python3 -B tools/ps2/run_pcsx2.py \
  --elf build/nov14-next4/recheck-sprites-ee/SPRITES.ELF \
  --log build/nov14-next4/recheck-sprites-ee/run.log \
  --until 'SPRITES DONE' --marker 'SPRITEBENCH ' --timeout 180
python3 -B tools/ps2/next4_render_sprites_report.py \
  --out build/nov14-next4/recheck-sprites-ee
python3 -B tools/ps2/next4_render_sprites_fixture.py
# Read-only independent final full-game ELF inspection:
python3 -B tools/ps2/next4_render_sprites_objectcheck.py \
  --out build/nov14-next4/recheck-sprites-objects \
  --base-src build/nov14-next4/reproduced-baseline/src --elf-only \
  --baseline-elf build/nov14-next4/baseline/software/SRB2.ELF \
  --candidate-elf build/ps2/SRB2.ELF
```
