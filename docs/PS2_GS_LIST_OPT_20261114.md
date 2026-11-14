# Indexed GS register submission, 2026-11-14

The optional hardware renderer now submits its three-register indexed triangle
lists through native GIF REGLIST. The previous PACKED path occupied one 128-bit
DMA word for each native 64-bit GS register. Geometry, texture storage, sampling,
lighting, blending, depth tests, clipping and vertex order are unchanged.

Baseline: `fc7b98fe6`. Changes are confined to `ps2_hw_draw.inc` and
`ps2_hw_model.inc`; additional files are dedicated test fixtures and this report.

## Packet and ownership changes

- A textured triangle contains native ST, RGBAQ and XYZ2/XYZF2 for each of its
  three vertices, followed by one ignored NOP. Ten 64-bit registers occupy five
  DMA quadwords instead of nine. NOP keeps every triangle and reservation aligned
  to 16 bytes; its data is explicitly initialized.
- REGLIST ignores the GIFtag PRE bit. Each list therefore begins with a separate
  native PRIM write, EOP=0, followed by the vertex tag, EOP=1. The explicit PRIM
  prefix and first reservation are allocated together as `3 + vertex_qwords`.
- The existing ring owner, `pk_alloc`, checks that entire allocation before any
  writes. Open lists close before producer-buffer flushes. No packet crosses a
  ring buffer, and no live half-quadword survives between reservations.
- Native scalar packing keeps the expressions and conversions of `put_vertex`.
  RGBAQ commits the same Q as PACKED ST followed by PACKED RGBAQ; RGB, alpha,
  palette lookup and model light-factor rounding are identical. The same
  RGBAQ state-cache invalidation remains in effect.
- Both generic indexed geometry and the optimized tiny-model path use the new
  representation. One-register flat lists retain PACKED. The generic fallback
  reserves `3 * nreg` quadwords per triangle, also covering two-register formats.
- Optimized models prepare native RGBAQ and XYZ once per transformed vertex.
  They reuse the existing `col4` slot after every float-projection cull/class
  consumer has finished. Its size, alignment and lifetime do not change. Each
  triangle corner then copies native registers and adds the triangle's fog.

For N triangles in a list, ignoring the unchanged DMA chain tag, PACKED uses
`1 + 9*N` quadwords; native uses `3 + 5*N`. The saving is `4*N - 2` quadwords,
including the explicit PRIM prefix. Even a single textured triangle saves two
quadwords. The GPU rasterizes exactly the same triangles and pixels.

## Production-source equivalence tests

```sh
source /workspace/.onboarding/environment.sh
python3 -B tools/ps2/gs_list_hosttest.py --out build/gs-list-opt-host
python3 -B tools/ps2/gs_list_hosttest.py --out build/gs-list-opt-host-sanitize --sanitize
```

The driver functions and type definitions are extracted verbatim into each
build directory; SHA256s identify the tested source. The fixture compares
decoded native register events against the unchanged PACKED encoder and the
retained pre-change model writer, rather than comparing packet bytes that are
expected to differ.

Both ordinary and AddressSanitizer/UndefinedBehaviorSanitizer runs passed:

| Coverage | Count / result |
|---|---:|
| Scalar vertices, formats 0/1/4/6 | 200,000; all GS registers identical |
| Optimized model vertices, formats 0/1/6 | 150,000; all GS registers identical |
| List triangles, including formats 2 and 5 | 761,292 |
| Actual production `emit_tris` output triangles | 214,830 |
| Deliberate Q, Z, PRIM, NOP, ring-boundary corruptions | All five detected |
| Sanitizer diagnostics in ordinary run | 0 |

Actual `emit_tris`, `xv_get`, transform, outcode and clipping functions run in the
fixture. Cases include clipping that expands one input triangle into multiple
output triangles, direct-mapped-cache aliases 0/256/512, wrap-coordinate bases,
and initial ring positions 1..11 quadwords from the end. The standalone list
cases cover two-register fmt 5; current `emit_tris` selects only formats 0/2/4/6.

The historical Windows/MSVC full-driver host fixture is stale: current PS2
texture fields, palette/VU state and engine hooks are absent. It was not reported
as passing. These dedicated tests compile the changed production functions
without altering their source or unrelated historical fixtures.

## EE / GS checks

The current standalone `hw_test.c` also lacked several new engine hooks at link
time. `gs_list_eetest.py` supplies them in a dedicated test-only file, with the
same polled kernel-clock sleep policy as the engine. It snapshots tracked
baseline HW sources without modifying the shared working tree.

```sh
SRB2_PS2_OUT=build/gs-list-base-ee python3 -B tools/ps2/gs_list_eetest.py --reference-ref fc7b98fe6
SRB2_PS2_OUT=build/gs-list-opt-ee python3 -B tools/ps2/gs_list_eetest.py
source /workspace/.onboarding/emulator-env.sh
SRB2_PS2_OUT=build/gs-list-base-ee python3 -B tools/ps2/run_hw_test.py --no-build --tag ct32 --timeout 240 -- soak=60 dump
SRB2_PS2_OUT=build/gs-list-opt-ee python3 -B tools/ps2/run_hw_test.py --no-build --tag ct32 --timeout 240 -- soak=60 dump
SRB2_PS2_OUT=build/gs-list-opt-ee python3 -B tools/ps2/run_hw_test.py --no-build --tag ct16 --timeout 240 -- fb16 quick
```

EE GCC 15.2 compiled both driver versions without diagnostics. The linker has
the existing SDK `abicalls`/`non-abicalls` warning; no other link diagnostics.
PCSX2 2.6.3, software GS, original user BIOS, 32 MiB EE RAM.

The first CT32 fixture pair completed with **55/56 checks passing in each**.
Both versions have the same pre-existing `vu0_transform` failure:
24,625 of 49,152 outcodes differ, while all projected X/Y/Z and Q values match.
This report does not hide or attribute that failure to the packet change.
The native CT16S quick suite similarly completed **48/49**, with the same failure.

All indexed/fan, indexed-cache-collision and float/tiny-model image comparisons
reported zero wrong pixels. The dedicated fixture additionally compares textured
optimized models with and without per-vertex lighting against their forced
indexed fallback. Both report zero wrong pixels. All 17 dumped CT32 framebuffer files are byte
identical between baseline and native (`build/gs-list-opt-ee/ct32-pixels.json`).
Neither CT32 run nor native CT16S reported timeouts or missing textures. Capture
spill/restore, palette changes, nearest/bilinear filtering, wipes, depth, alpha,
VRAM pressure, ring stress and shutdown checks retained their previous results.

| Initial CT32 fixture | PACKED | Native | Change |
|---|---:|---:|---:|
| 4,000 indexed triangles, EE submit cycles | 4,795,437 | 4,651,299 | -3.01% |
| Same, GIF quadwords | 36,040 | 20,044 | -44.38% |
| Same, subsequent GS wait cycles | 14,827 | 14,567 | -260 |
| Mixed 60-frame soak, elapsed cycles | 303,152,048 | 303,147,623 | Essentially unchanged |
| Mixed soak, total GIF quadwords | 1,578,995 | 1,562,965 | -16,030 |
| Mixed soak, DMA kicks | 498 | 496 | -2 |

The mixed soak is paced by vblank and includes readbacks and uploads. Its timing
does not establish an uncapped full-game FPS gain. The indexed submission
measurement establishes a local instruction/packet improvement under PCSX2;
physical EE cache and GS timing remain unmeasured.

The final optimized-model implementation was checked separately. A model with
3,000 identical indexed textured triangles isolates repeated corner submission;
the flat and vertex-lit images also match the forced indexed fallback. Commands
use the same two `HW_TEST.ELF` paths, tag `model-ct32` for baseline and
`model-ct32-final` for native, arguments `quick dump`.

| Final model fixture | PACKED | Native | Change |
|---|---:|---:|---:|
| Flat model, EE submit cycles | 647,647 | 543,572 | -16.07% |
| Vertex-lit model, EE submit cycles | 644,835 | 549,734 | -14.75% |
| Either model, GIF quadwords | 27,021 | 15,021 | -44.41% |
| Either model, DMA kicks | 5 | 3 | -2 |

This model fixture deliberately reuses three vertices heavily. It is a local
packing benchmark, not a representative full-game model/FPS measurement. The
final native CT32 quick suite passes 55/56 checks, with only the same pre-existing
VU0 outcode failure. The initial per-corner native model encoder was rejected
because it slowed this benchmark by 26.5–32.7%; the final implementation prepares
the registers once per vertex and does not retain that slower implementation.

## Resource accounting and limits

No heap buffer, static work array, texture or VRAM allocation is added. Existing
state/ring/record/scratch budgets are identical: 69,664 / 524,288 / 75,776 /
64,000 bytes in the standalone fixture. Reduced packet occupancy does not shrink
the allocated producer ring. Framebuffers, Z buffer, CLUT pool, texture pool and
screen captures keep their previous sizes and formats.

The final standalone linked image has text +1,616 bytes, data unchanged, BSS
-64 bytes from link layout. This is a small loaded-code cost, not a RAM-saving
claim. Full-engine LTO layout may differ. No VRAM reduction, raster-load reduction,
physical-console speedup, whole-game FPS increase or complete PC renderer parity
is claimed here. The coordinator handles full-engine integration validation.
