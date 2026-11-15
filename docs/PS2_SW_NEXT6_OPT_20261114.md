# Exact bounded NPOT perspective wrapping — sixth pass, 2026-11-14

The reference is the immediately preceding completed pass, reconstructed by
`tools/ps2/prepare_next6_baseline.py` in
`build/nov14-next6/baseline-src/src`. Its complete Software ELF is
`build/nov14-next6/baseline/software/SRB2.ELF`, SHA256
`c913c71773576a6f4ca6c27386e12fe3568e3e5232ecf9dffdac2b8c5421cc32`.
Git HEAD is not the comparison reference.

Only `src/r_draw8_npo2.c` changes for this Software work. Seven NPOT perspective
drawers use an exact bounded reciprocal for integer texture-coordinate wrapping
under the existing `PS2_OPT_DRAW` gate. The existing two-entry-per-axis cache
keeps the same dimensions, hit order and replacement cursors. Each aligned entry
still occupies 8 B; the four entries and two cursors retain **34 B** of BSS.
The cached word now holds `ceil(2^31 / dimension)` instead of a general-purpose
libdivide descriptor. A nonzero miss uses one native 32-bit division per axis.
Zero dimensions still call the original `libdivide_u32_gen(0)` error path.

No floating-point expression, precision policy, perspective block size,
coordinate conversion, integer advance, sample index, lighting, translation,
blend order, transparency test or pixel-store order changes. Source, lighting,
translation, blend and background contents remain ordinary live reads, including
when framebuffer stores overwrite them. The previous metadata locals remain;
there is no `restrict`, sample cache or composed lookup table.

The ordinary NPOT drawers and all sibling `r_draw.c`, `r_draw8.c` and `v_video.c`
text match the immediate reference. Inverting only the cache/wrap changes
reconstructs the complete frozen `r_draw8_npo2.c` byte-for-byte. Source audit and
isolated diff: `build/nov14-next6/sw-npot-source/{change-audit.json,change.diff}`.
No allocation, reserve, capacity threshold, VRAM use or DMA traffic is added.
Texture/framebuffer dimensions, graphics, geometry, physics, features and
compatibility limits are unchanged.

## Exact integer domain

The existing `(fixed_t)u >> FRACBITS` conversion produces a signed 16-bit integer
`c`, so `n = c` for nonnegative coordinates and `n = -c - 1` otherwise always
satisfies `0 <= n <= 32767`. The existing UINT16 dimensions cover every
`1 <= d <= 65535`. Neither bound is a new restriction.

Let `B = 2^31`, `R = floor((B - 1) / d) + 1 = ceil(B / d)` and
`e = R*d - B`. Then `0 <= e < d` and
`n*e <= 32767*65534 < B`. Writing `n = q*d + r` gives
`r*B + n*e < d*B`, so `floor(n*R/B) = floor(n/d)` exactly. The EE obtains that
quotient with `high32((n << 1) * R)` and no descriptor-dependent pixel branch.
Complementing the quotient for negative coordinates reproduces the original
`c + (floor((-c-1)/d) + 1)*d`, including exact negative multiples. All helper
intermediates use defined UINT32 arithmetic; the final result is in `[0,d)`.

`next6_render_npot_reciprocalcheck.py` extracts the actual helpers and exhausts
**4,294,901,760 signed-coordinate/dimension pairs** against an independent
integer-division oracle. It also runs **131,086 cache transitions** through both
actual frozen/current cache functions, comparing both slots and the replacement
cursor for each axis. Observations are appended at helper exits without changing
their arithmetic. Coverage includes both slot hits, eviction sequences,
dimensions 1/32768/65535 and every other nonzero UINT16 dimension.

Wrong actual shift, sign, reciprocal-generation and replacement-cursor mutations
are all detected. All 16 warm-cache zero-width/height subprocesses across the
four rendering policies retain exit 2 and byte-identical original error text.

## EE measurements

EE GCC 15.2.0, existing release `-O2`, `-G8`, `-mno-abicalls`, `-fwrapv`;
PCSX2 2.6.3 with ordinary 32 MiB EE RAM. Complete frozen/current drawers and
cache helpers are extracted into a functions TU separate from the driver and
external globals. Neither TU uses LTO; drawers have `noinline`, `noipa` and
`externally_visible` attributes. Lighting and slope-conversion helpers come
from the real frozen `r_draw.c`.

Each target row first checks every framebuffer/canary byte, all slope-lighting
entries and final `ds_x1`. The runner exits 0 and reaches `NPOT DONE` after
**all 252 warm-cache and 21 cache-thrash rows**. Paired timings use the best of
three repetitions and alternate execution order.

The warm rows cover all seven drawers, 63×47 / 64×47 / 257×17 dimensions,
lengths 1/8/16/32/100/320, sparse/dense coverage and 1,024 calls per row.
Every row improves, by **2.1644–14.8148%**; the equally weighted sum falls
**7.97115%**. Per-drawer summed reductions in the table's order are 7.2755%,
6.7958%, 8.7912%, 7.8951%, 9.1662%, 9.5270% and 7.0155%.

For 100 pixels, 63×47 and dense coverage, cycles per call:

| Drawer | Before | After | Reduction |
| --- | ---: | ---: | ---: |
| Tilted span | 7964 | 7135 | 10.41% |
| Tilted translucent span | 8694 | 7866 | 9.52% |
| Tilted splat | 8190 | 7239 | 11.61% |
| Tilted translucent splat | 8895 | 7965 | 10.46% |
| Tilted floor sprite | 6510 | 5680 | 12.75% |
| Tilted translucent floor sprite | 7201 | 6282 | 12.76% |
| Tilted water span | 9022 | 8016 | 11.15% |

The additional rows cycle 63×47 / 257×17 / 319×241 dimensions for 768 calls,
after priming both cache slots with dimensions outside that cycle. Both axes
miss on every timed call. All seven drawers and lengths 1/16/100 are checked.
All **21 rows improve by 11.7306–51.4530%**, with a summed reduction of
**18.06968%**. These selected cycles do not measure every possible dimension mix.

These are selected kernel timings, not whole-game FPS, physical-PS2 cache/power
measurements or measured frame-time variance. Complete-engine size/stack and
paired gameplay-frame evidence belong to the coordinator's integration report.

Measured ELF: `build/nov14-next6/sw-npot-ee/NPOT.ELF`, SHA256
`4f93968f957633c79ee833b87ecefe3aea6b9532af6955dfd1bb581adb5e2bc7`.
The full stream, runner status and numeric pairs are in `run.log`, `runner.log`
and `report.json` in the same directory.

## Actual objects, stack and controls

Complete actual frozen/current `r_draw.c` translation units compile with release
EE flags and `-Werror`, without LTO for inspectable sections:

| Sections | Before | After | Change |
| --- | ---: | ---: | ---: |
| `.text*` | 69,248 B | 63,056 B | −6,192 B |
| `.rodata*` | 567 B | 567 B | 0 |
| `.data*` plus `.sdata*` | 32 B | 32 B | 0 |
| `.bss*` plus `.sbss*` | 15,743 B | 15,743 B | 0 |

Only the seven drawers and the replaced cache helper differ in text. All other
text sections, data sections, rodata-content multisets and BSS names/sizes match.
NOBITS BSS is compared by size, not mistaken for file-backed bytes.
`sw-npot-objects/unchanged-sections.json` records these checks.

The four lit non-water drawer frames fall 160→144 B, both floor-sprite frames
144→128 B, and the water frame 176→160 B. The cache helper falls **80→16 B**,
and its text falls 640→384 B. The selected drawer-to-cache call chain therefore
uses 80 B less stack; this is not a measurement of the game's maximum stack.
Per-block floating-point setup still has stack accesses; no stack-free drawer
claim is made.

The candidate's actual cache helper contains exactly **two native `DIVU` sites**
and only an `I_Error` call, with no division/CLZ library call. The frozen
complete Software ELF independently retains the old 80 B helper frame, two
`__clzdi2` and two `__udivdi3` call sites. Thus the old generation cost exists
in the game, rather than arising only in the isolated fixture. Actual object
and frozen linked disassembly are in `build/nov14-next6/sw-npot-objects`.

The coordinator's complete release-LTO Software ELF was independently checked,
SHA256 `69141b1a0f5eb44f505b6ced3f69162fa43d214d4620d3aa48f2adce0429e31d`.
All **14 innermost pixel loops** use two high multiplies/results, at most two
branches (loop control plus optional transparency), no metadata-global loads,
no stack accesses and no division or call. The linked cache helper has the same
two native `DIVU` sites, only the original `I_Error` call, and an 80→16 B frame.
Linked frames fall **192→144 B** for the four lit non-water drawers,
**144→128 B** for both floor sprites and **208→192 B** for water. The drawer
frames differ from the isolated non-LTO objects; both measurements are retained.
Loop boundaries, complete stack inventories and disassembly are recorded in
`sw-npot-objects/elf-report.json` and adjoining `.asm` files.

With `PS2_NOOPT_DRAW`, all actual text/rodata section names, sizes and bytes
match the reference. Framed SHA256:
`b9cc0617d4d509df6fa5f434a36166151079d7056ec534cdc052e820fcbdaf80`.
The seven complete disabled-policy preprocessed drawer bodies also match,
SHA256 `0b4d7ae9a3f68d022fd9f7fccc7c75801ba1f5d55b828b3503cd7c4889562e5c`.

An earlier measured variant converted the old general-purpose cached divider
into the bounded reciprocal. Its warm sum improved 7.6092%, but seven forced
cache-miss one-pixel rows grew 8.7–14.7 cycles (at most 1.5612%). It was rejected
in favor of the exact native-32-bit miss generator above. Its source/object/EE
evidence is retained in the `sw-npot-*-converted` directories. An initial
per-call conversion experiment was host-validated but not target-timed or
retained. No result from either experiment is mixed into the final measurements.

## Complete-drawer host proof

`tools/ps2/next6_render_npot_hosttest.{c,py}` independently extracts the complete
actual frozen/current drawers. Each of float, double, `PS2_NOOPT_DRAW` and full
`PS2_NOOPT` passes **323,204 cases**, comparing **3,141,155,584 complete
framebuffer/canary bytes** per policy, plus all lighting entries and `ds_x1`.
ASan+UBSan passes the same float cases without diagnostics. Float/NOOPT_DRAW
row hash is `b29158ff56e439c0`; double/full-NOOPT hash is `fed79f0f44cb7a83`.

Coverage retains the prior 22 NPOT/P2 dimension pairs, 1×1 through 65535×1 /
1×65535, mixed axes, framebuffer widths 1–640, empty spans, block boundaries,
signed-coordinate boundaries and values beyond 2^32, both step/depth/light signs,
lighting clamp extremes, sparse/dense coverage and changing lookup identities.
Seven live-content alias modes include source, shifted source, lighting,
translation, blend, overlapping water background and combined overlap; renderer
global-variable storage remains separate from framebuffer storage.

All **12 drawer negative controls** detect output corruption, a changed actual
source lookup and a stale sample cache under aliases in all four policies. The
four helper mutations above also fail as expected. No replacement drawing
algorithm or new golden output is used.

Frozen production `src/r_draw8_npo2.c` SHA256:
`5886e424c383b2cd47c4428fd333300e1897cca562e58fffdd4e2bf8eb66e8c3`.
Copy: `build/nov14-next6/sw-npot-source/src/r_draw8_npo2.c`.

## Reproduce

```bash
source /workspace/.onboarding/environment.sh
python3 -B tools/ps2/prepare_next6_baseline.py
python3 -B tools/ps2/next6_render_npot_sourceaudit.py \
  --out build/nov14-next6/recheck-npot-source \
  --base-src build/nov14-next6/baseline-src/src
python3 -B tools/ps2/next6_render_npot_reciprocalcheck.py \
  --out build/nov14-next6/recheck-npot-reciprocal \
  --base-src build/nov14-next6/baseline-src/src --negative-controls
python3 -B tools/ps2/next6_render_npot_hosttest.py \
  --out build/nov14-next6/recheck-npot-host \
  --base-src build/nov14-next6/baseline-src/src --negative-controls --sanitize
python3 -B tools/ps2/next6_render_npot_objectcheck.py \
  --out build/nov14-next6/recheck-npot-objects \
  --base-src build/nov14-next6/baseline-src/src
python3 -B tools/ps2/next6_render_npot_hosttest.py \
  --out build/nov14-next6/recheck-npot-ee \
  --base-src build/nov14-next6/baseline-src/src --ee-only
source /workspace/.onboarding/emulator-env.sh
python3 -B tools/ps2/run_pcsx2.py \
  --elf build/nov14-next6/recheck-npot-ee/NPOT.ELF \
  --log build/nov14-next6/recheck-npot-ee/run.log \
  --until 'NPOT DONE' --timeout 180
python3 -B tools/ps2/next6_render_npot_report.py \
  --out build/nov14-next6/recheck-npot-ee
# Read-only audit after the coordinator's full Software build:
python3 -B tools/ps2/next6_render_npot_objectcheck.py \
  --out build/nov14-next6/recheck-npot-objects \
  --base-src build/nov14-next6/baseline-src/src --elf-only \
  --baseline-elf build/nov14-next6/baseline/software/SRB2.ELF \
  --candidate-elf build/ps2/SRB2.ELF
```
