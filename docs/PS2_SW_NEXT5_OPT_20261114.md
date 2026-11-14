# Exact NPOT perspective drawers — fifth pass, 2026-11-14

The reference is the immediately preceding completed pass, reconstructed and
verified by `tools/ps2/prepare_next5_baseline.py` in
`build/nov14-next5/baseline-src/src`. Its complete release-LTO Software ELF is
`build/nov14-next5/baseline/software/SRB2.ELF`, SHA256
`403462b11c3a09007a7c9c21dc198a113f99b103173212c8f3fbe2fdf976bc44`.
Git HEAD is not the comparison reference.

Only `src/r_draw8_npo2.c` changes for this Software work. The seven NPOT tilted
span, translucent span, splat, translucent splat, floor-sprite, translucent
floor-sprite and water drawers retain per-call metadata in locals under the
existing `PS2_OPT_DRAW` gate. All seven retain their exact `UINT16` dimensions;
the five lit drawers also retain the lighting-table pointer, colormap offset
and column cursor; the four blended drawers retain the blend-table address.
The five public `ds_x1` results are written back after drawing, as in the
preexisting optimized P2 drawers. Floor sprites leave `ds_x1` unchanged.

Framebuffer byte stores previously forced repeated loads of those renderer
globals under C's character-aliasing rules. Framebuffer storage is separate
from the storage of the globals themselves. Source, translation, lighting,
blend and background **contents** remain ordinary live reads, including when
pixel stores overwrite those contents. There is no `restrict`, composed lookup,
sample cache or immutable-data assumption.

No floating-point expression, precision policy, perspective block size,
coordinate conversion, divider generation/cache policy, sample index, lighting,
translation, blend order, transparency test or pixel-store order changes.
An inverse transformation that removes only the new metadata locals/macros and
cursor writeback reconstructs the frozen complete file byte-for-byte. All other
text, including ordinary NPOT loops and the earlier divider cache, is unchanged.
The sibling `r_draw.c`, `r_draw8.c` and `v_video.c` files match the immediate
baseline byte-for-byte. Audit and isolated diff:
`build/nov14-next5/sw-npot-source/{change-audit.json,change.diff}`.

There is no new allocation, persistent/static data, VRAM, DMA traffic, reserve,
capacity threshold or fallback. Graphics, framebuffer/texture dimensions,
geometry, physics, features and compatibility limits are unchanged. The
per-call stack and code costs are disclosed below.

## EE cost and limits

EE GCC 15.2.0, the existing release `-O2`, `-G8`, `-mno-abicalls`, `-fwrapv`;
PCSX2 2.6.3 with ordinary 32 MiB EE RAM. The actual complete frozen/current
functions are extracted into a translation unit separate from the driver and
external global definitions. Neither TU uses LTO. Each actual drawer has
`noinline`, `noipa` and `externally_visible` attributes. The lighting and slope
conversion helpers are extracted from the frozen real `r_draw.c`.

Both versions include the same preceding two-entry-per-axis divider cache.
This pass therefore does not remeasure the older uncached implementation.
The frozen complete game ELF independently confirms the repeated dimension,
lighting, blend and cursor loads inside its NPOT pixel loops. The reference
cost exists in the game, rather than arising only in the benchmark.

The EE fixture checks every framebuffer/canary byte, all slope-lighting entries
and `ds_x1` before each timing row. All **252 rows** complete, the runner exits
0 and reaches `NPOT DONE`. Seven drawers, dimensions 63×47 / 64×47 / 257×17,
lengths 1/8/16/32/100/320 and sparse/dense coverage are measured with 1,024 calls,
best of three paired repetitions, with alternating execution order.

For 100 pixels, 63×47 and dense coverage, cycles per call:

| Drawer | Before | After | Reduction |
| --- | ---: | ---: | ---: |
| Tilted span | 9287 | 7964 | 14.25% |
| Tilted translucent span | 10180 | 8694 | 14.60% |
| Tilted splat | 9716 | 8190 | 15.71% |
| Tilted translucent splat | 10607 | 8896 | 16.13% |
| Tilted floor sprite | 6793 | 6504 | 4.25% |
| Tilted translucent floor sprite | 7700 | 7207 | 6.40% |
| Tilted water span | 10332 | 9022 | 12.68% |

Across all equally weighted rows, summed cost decreases **12.8284%**: 240 rows
improve, 9 match and **3 one-pixel floor-sprite rows increase by 1–2 cycles**
(maximum 0.6006%). Every sampled length above one pixel improves in all seven
drawers; the five lit drawers improve in every sampled row. Per-drawer summed
reductions, in the table's order, are 14.4523%, 14.8321%, 15.3744%, 15.8115%,
4.2088%, 6.2803% and 14.3054%.

These are selected kernel timings, not full-game FPS, physical-PS2 cache/power
measurements or a measured reduction in frame-time variance. No universal
per-call speedup is claimed.

Measured ELF: `build/nov14-next5/sw-npot-ee/NPOT.ELF`, SHA256
`4b6e77af21d17c43ec613cceb6a26424b564a289e84c5ef521d6911bc58a1213`.
Log and all numeric pairs are in `run.log` and `report.json` in the same directory.
After strengthening the host-only alias cases, the driver was rebuilt and the
ELF relinked: both `driver.o` and the complete ELF are byte-identical to the
measured copies. `ee-alias-proof.json` records that check.

## Actual object and linked-game evidence

Compiling the complete actual frozen/current `r_draw.c` with release EE flags
and `-Werror`, without LTO for inspectable sections:

| Sections | Before | After | Change |
| --- | ---: | ---: | ---: |
| `.text*` | 68760 B | 69248 B | +488 B |
| `.rodata*` | 567 B | 567 B | 0 |
| `.data*` plus `.sdata*` | 32 B | 32 B | 0 |
| `.bss*` plus `.sbss*` | 15743 B | 15743 B | 0 |

Only the seven edited drawer text sections differ. Every other actual text
section, all data sections, the multiset of rodata contents and all BSS section
sizes match. Compiler-generated switch-table suffixes are renumbered in the
profile object; their contents match. `unchanged-sections.json` records this
check, without treating NOBITS BSS as file-backed bytes.

Object stack frames increase 144→160 B for span/translucent/splat/translucent
splat, 160→176 B for water and remain 144 B for both floor sprites. Register
pressure leaves 1–2 stack accesses in some blended pixel-loop paths; no claim
of stack-free drawers is made. The extra stack is transient, **16 B per lit
call**, not a measured increase in the game's maximum stack use. There is no
heap/static-RAM increase from the metadata locals.

The coordinator's complete release-LTO Software ELF `build/ps2/SRB2.ELF`, SHA256
`c913c71773576a6f4ca6c27386e12fe3568e3e5232ecf9dffdac2b8c5421cc32`,
was independently inspected. All **31 detected backward-branch regions with
pixel stores** across the seven drawers contain zero tracked metadata-global
loads. These regions can overlap; they are not 31 separate algorithms. The
linked per-call frames increase 176→192 B for the four lit non-water drawers,
192→208 B for water and remain 144 B for both floor sprites. Disassembly,
GP-relative symbol offsets, regions and full stack-access inventories are in
`build/nov14-next5/sw-npot-objects/{report.json,elf-report.json}` and adjoining
`.asm` files. Final complete-engine size/heap/FPS evidence belongs to integration.

With `PS2_NOOPT_DRAW`, all actual `.text*`/`.rodata*` section names, sizes and
bytes match the frozen reference. Framed code/rodata SHA256:
`b9cc0617d4d509df6fa5f434a36166151079d7056ec534cdc052e820fcbdaf80`.
The seven complete preprocessed drawer bodies also match, SHA256
`0b4d7ae9a3f68d022fd9f7fccc7c75801ba1f5d55b828b3503cd7c4889562e5c`.

Frozen production source: `src/r_draw8_npo2.c`, SHA256
`a2a3fbb79754bce848718a0cd8d7b0c60740416764808644b31544add9310b5f`.
Copy: `build/nov14-next5/sw-npot-source/src/r_draw8_npo2.c`.

## Host exactness and live aliases

`tools/ps2/next5_render_npot_hosttest.{c,py}` independently extracts the complete
actual drawers from frozen/current source. Each of float, double,
`PS2_NOOPT_DRAW` and full `PS2_NOOPT` passes **323,204 cases**, comparing
**3,141,155,584 complete framebuffer/canary bytes** per policy. ASan+UBSan passes
the same float cases without diagnostics. Both sides always use the same
preexisting precision policy. Float/NOOPT_DRAW row hash is `b29158ff56e439c0`;
double/full-NOOPT hash is `fed79f0f44cb7a83`.

Coverage includes 22 NPOT/P2 dimension pairs, 1×1 through 65535×1 / 1×65535,
257×17, 319×241, 512×512, non-square P2 and mixed P2/NPOT axes; framebuffer
widths 1/7/16/31/319/320/512/640; 20 lengths through 640 and negative empty
ranges; origins at the left/middle/right; every 16-pixel block boundary;
signed coordinate boundaries and values beyond 2^32; exact periods and repeated
wraps; both signs of texture/depth/light steps and lighting clamp extremes;
sparse/dense transparency; changing source, translation, lighting and blend
identities. All lighting entries and final `ds_x1` are compared.

Seven alias modes cover source bytes, shifted source, colormap/lighting
contents, translation contents, blend contents, overlapping water background
and combined overlap. The aliased 63×47 textures and lookup regions straddle
the destination stores; aligned two-byte floor sources remain valid allocations.
Globals themselves stay outside the framebuffer. A stale one-entry source
sample cache, injected into the **actual extracted candidate lookups**, fails
after just two pixels in alias-only runs in every policy. This proves that
intervening pixel stores affect later reads in the test, rather than merely
placing unused buffers in the same allocation.

All **12 negative controls** pass: each policy detects corrupted output, a
changed actual source read and that stale-source-cache mutation. Warm-cache
zero width/height subprocesses retain the original libdivide error path for
both versions in all four policies. No replacement drawing algorithm or new
golden output is used.

## Focused integration add-on

`tools/ps2/next5_render_npot_fixture.{py,lua}` generates
`build/nov14-next5/npot-addons/NX5_FLOOR.pk3`. Its non-solid **63×45** Doom patch
is independently decoded and checked against all 2,835 generated pixels: 2,128
opaque and 707 transparent. Masked/dense texture variants are Doom patches in
`Textures/`, whose real single-patch loader reads their exact dimensions; an
unsupported odd-sized raw flat or optional PNG decoder is not used.

The Lua follows the preceding validated static placement and iterator pattern.
It creates eight `RF_SLOPESPLAT` floor sprites, four opaque and four translucent,
with translated variants, and slopes of ±1/4. `P_CreateFloorSpriteSlope` is
registered in `lua_baselib.c`; its `o`, `xydirection` and `zdelta` setters in
`lua_maplib.c` update both fixed and double representations. `r_splats.c` selects
that explicit slope and dispatches a non-solid NPOT sprite through the two
edited tilted sprite drawers when visible. The fixture changes existing
renderable FOF textures/alpha and textures on already-sloped sector planes;
geometry/heights/collision flags remain as loaded. `sector.ffloors()` calls the
required iterator factory, and zero-valued angles use explicit assignment.

`READY` reports configured sprite/slope/plane counts, not visibility or calls
to every drawer. The add-on SHA256 is
`f6ee4484ce0eac62438ac81db80954fdc360f71e8d52424769e9b4e6b441e36c`;
its manifest records every asset hash. Paired full-engine captures and fixture
execution status belong to the coordinator's integration report.

## Reproduce

```bash
source /workspace/.onboarding/environment.sh
python3 -B tools/ps2/prepare_next5_baseline.py
python3 -B tools/ps2/next5_render_npot_hosttest.py \
  --out build/nov14-next5/recheck-npot-host \
  --base-src build/nov14-next5/baseline-src/src --negative-controls --sanitize
python3 -B tools/ps2/next5_render_npot_objectcheck.py \
  --out build/nov14-next5/recheck-npot-objects \
  --base-src build/nov14-next5/baseline-src/src
python3 -B tools/ps2/next5_render_npot_hosttest.py \
  --out build/nov14-next5/recheck-npot-ee \
  --base-src build/nov14-next5/baseline-src/src --ee-only
source /workspace/.onboarding/emulator-env.sh
python3 -B tools/ps2/run_pcsx2.py \
  --elf build/nov14-next5/recheck-npot-ee/NPOT.ELF \
  --log build/nov14-next5/recheck-npot-ee/run.log \
  --until 'NPOT DONE' --marker 'NPOTBENCH ' --timeout 180
python3 -B tools/ps2/next5_render_npot_report.py \
  --out build/nov14-next5/recheck-npot-ee
python3 -B tools/ps2/next5_render_npot_fixture.py
python3 -B tools/ps2/next5_render_npot_objectcheck.py \
  --out build/nov14-next5/recheck-npot-objects \
  --base-src build/nov14-next5/baseline-src/src --elf-only \
  --baseline-elf build/nov14-next5/baseline/software/SRB2.ELF \
  --candidate-elf build/ps2/SRB2.ELF
```
