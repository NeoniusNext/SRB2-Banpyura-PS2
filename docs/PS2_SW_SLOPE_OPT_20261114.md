# Exact NPOT software slope setup reuse — 2026-11-14

Baseline: clean `fc7b98fe6`, with the renderer source snapshot saved in
`build/nov14-sw-opt/base-src`. The only production edit in this work is
`src/r_draw8_npo2.c`. Other agents' edits were preserved.

## Runtime change

The seven NPOT tilted drawers previously generated two libdivide descriptors
for every span. A plane commonly draws many spans with unchanged dimensions;
the EE setup includes `__clzdi2` and, for non-power-of-two dimensions,
`__udivdi3` calls. Under the existing `PS2_OPT_DRAW` gate, a shared helper now
retains two descriptors per axis. Each cache entry contains its exact dimension
key, magic word and shift flags. A miss invokes the original
`libdivide_u32_gen`, and a hit copies precisely those generated fields into the
original span-local descriptors. Two entries also handle alternating plane and
floor-sprite dimensions without repeatedly regenerating them.

The helper stores aligned magic words and copies fields explicitly. The first
experiment's assignment of libdivide's packed structures emitted ten byte
loads and ten byte stores on the EE. Explicit field copies avoid that overhead.
The two-entry cache replaces entries in alternating slots on misses; entries
never contain source/cache pointers or allocations. Changing a level, flat
source, palette, view size or framebuffer therefore needs no invalidation.
An invalid zero dimension still reaches libdivide's existing `I_Error` path.

Texture positions, wrapping, perspective interpolation, float/double precision,
lighting, translation, transparency, blending and the `ds_x1` side effect are
unchanged. The change adds no per-pixel work and does not modify the other
drawers, geometry, capacities, fallback limits, quality or gameplay.

## EE cycle evidence

EE GCC 15.2.0, release engine flags including `-O2`, `-G8` and LTO; PCSX2 2.6.3
through the existing `run_pcsx2.py` wrapper. A single ELF contains the frozen
baseline and actual candidate functions. Each row measures 1,024 draws, best
of three repetitions; numbers include the common dispatch/setup loop. The
benchmark contains no generator-count instrumentation. The runner exited 0,
emitted all 168 rows and reached `SLOPES DONE`.

Selected unchanged-dimension workloads, texture 63x47, cycles per draw:

| Drawer | Pixels | Before | After | Reduction |
| --- | ---: | ---: | ---: | ---: |
| Ordinary tilted span | 1 | 844.0 | 530.0 | 37.2% |
| Ordinary tilted span | 16 | 2122.0 | 1787.0 | 15.8% |
| Ordinary tilted span | 100 | 9206.0 | 8679.0 | 5.7% |
| Ordinary tilted span | 320 | 27583.0 | 26586.7 | 3.6% |
| Translucent tilted span | 100 | 10131.0 | 9394.0 | 7.3% |
| Tilted splat | 100 | 8824.0 | 8294.0 | 6.0% |
| Tilted floor sprite | 1 | 688.0 | 365.0 | 46.9% |
| Translucent tilted floor sprite | 16 | 1807.0 | 1481.0 | 18.0% |
| Tilted water span | 320 | 31509.3 | 29852.9 | 5.3% |

The full suite has seven drawers, dimensions 63x47 / 64x47 / 257x17, lengths
1 / 16 / 100 / 320, and both fixed dimensions and dimensions alternating every
draw. Across the 84 fixed-dimension rows, the sum of cycle costs decreases
4.24%; all improve except one 320-pixel floor-sprite row at +0.037%. Across the
84 alternating-dimension rows, every row improves, by 0.27–46.1%; their summed
cost decreases 4.06%. These equally weighted workload sums are not frame or FPS
measurements.

The first, rejected one-entry experiment slowed some one-pixel alternating
dimension calls by up to 17.4%. The second entry removes that measured case.
A stream of more than two repeatedly changing dimensions can still miss this
small cache and pay its lookup/storage cost in addition to the original
generation. Such a stream was covered for correctness by the host fixture, but
its EE timing has not been measured. No claim of universal per-call speedup is
made. Physical PS2 cache timing and complete-frame performance need separate
measurement.

Final measured ELF:
`build/nov14-sw-opt/slopes-ee-pair/SLOPES.ELF`.
Rows and normalized report:
`build/nov14-sw-opt/slopes-ee-pair/{run.log,report.json}`.
The timing fixture checks CPU cost; pixel equality is established by the host
fixtures below and the coordinator's separate integrated frame checks.

## Code/RAM evidence

Compiling the actual complete `src/r_draw.c` translation unit with the release
EE flags and LTO disabled for inspectable object sections gives:

| Sections | Before | After | Change |
| --- | ---: | ---: | ---: |
| All `.text*` | 72464 B | 70256 B | -2208 B |
| All `.rodata*` | 567 B | 567 B | 0 |
| All `.bss*` plus `.sbss*` | 15709 B | 15743 B | +34 B |

No heap, VRAM, DMA or scratchpad allocation is introduced. These are object
section sizes, not a claim about the final linked ELF or total game peak RAM.
The descriptor helper factors setup code that was duplicated in seven drawers.

With `PS2_NOOPT_DRAW`, baseline and candidate `.text*`/`.rodata*` section names,
sizes and bytes match exactly across 84 sections. SHA256 of the framed section
data is `aa558c8d4e20e99aa15c50f4bc04c268b1c39105e513b63b2d1900d3b63f0c1d`.
The hash uses each section's actual object-file offset; it does not flatten
overlapping relocatable section addresses. Object commands and size evidence
are in `build/nov14-sw-opt/{final-*.log,final-*.sections,object-report.json}`.

## Equivalence and lifecycle tests

`tools/ps2/nov14_render_slopes_hosttest.{c,py}` extracts the seven complete
functions independently from frozen and current source, along with the frozen
lighting/conversion helpers. It does not reimplement slope sampling. GCC builds
use `-Wall -Wextra -Werror`, `-fwrapv` and `-ffp-contract=off`.

Each float, double and `PS2_NOOPT` build passes **214,914 cases**, comparing all
**5,824 framebuffer/canary bytes per call**, **1,251,659,136 bytes** per policy,
with zero differences. The fixture also checks all lighting entries, `ds_x1`
and the exact number of descriptor generations against an independent cache
lifecycle model. In the mixed-dimension stream, baseline generates 429,828
descriptors and candidate generates 6,988. Deliberately corrupted pixels and
wrong generated divisors each fail on all three policies.

Cases cover:

- All seven opaque/translucent/splat/floor-sprite/water drawers; sparse and dense
  transparent samples, nontrivial lighting/translation/blend lookup tables.
- Flat dimensions 1x1, 1x3, 3x1, 3x5, 63x47, 64x47, 47x64, 257x17, 1024x3,
  16383x1, 1x16384, 32767x1, 65535x1, 1x65535, 319x241 and 512x512.
- Framebuffer widths 1 / 7 / 16 / 31 / 319 / 320 / 512 / 640; 19 span lengths
  through 640; left, middle and right starts; lengths on both sides of the
  16-pixel perspective block boundary.
- Signed fixed-coordinate boundaries, positive/negative exact periods,
  repeated wraps, values beyond 2^32, zero coordinate steps and both signs
  of depth/texture gradients.
- Repeated dimensions, alternating pairs, three-dimension replacement, cache
  sharing across drawer types and independent axis changes.

The optimized policies also compare returned descriptor fields against fresh
`libdivide_u32_gen` for **every nonzero UINT16 dimension**, 262,140 descriptor
pairs per policy. Separate subprocesses warm the cache and then test zero width
or height: reference and candidate both exit through the original libdivide
error. An AddressSanitizer plus UndefinedBehaviorSanitizer build passes the
same float cases and exhaustive descriptor check without diagnostics.

Framebuffer-row FNV64 hashes: float
`d9fdd05964f7b7f1`, double and NOOPT `6bde7dcedc4a4f84`. Different preexisting
float/double policies are compared only against their own frozen references.
The ordinary PC/double precision policy is not substituted for the PS2 policy.

Actual full `r_draw.c` EE compile passes `-Werror`; whitespace checks pass.
Integrated engine/map/rendered-frame checks and the shared deviation registry
are owned by the coordinator. No assets, golden data, build defaults, shared
headers or other agents' production files were changed here.

## Reproduce

```bash
source /workspace/.onboarding/environment.sh
python3 -B tools/ps2/nov14_render_slopes_hosttest.py \
  --out build/nov14-sw-opt/slopes-final2 \
  --base-src build/nov14-sw-opt/base-src --negative-controls --sanitize
python3 -B tools/ps2/nov14_render_slopes_hosttest.py \
  --out build/nov14-sw-opt/slopes-ee-pair \
  --base-src build/nov14-sw-opt/base-src --ee-only
source /workspace/.onboarding/emulator-env.sh
python3 -B tools/ps2/run_pcsx2.py \
  --elf build/nov14-sw-opt/slopes-ee-pair/SLOPES.ELF \
  --log build/nov14-sw-opt/slopes-ee-pair/run.log \
  --until 'SLOPES DONE' --marker 'SLOPEBENCH ' --timeout 180
```
