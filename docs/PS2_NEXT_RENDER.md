# Exact renderer CPU and drawseg RAM continuation — 2026-10-03

This change preserves prior uncommitted renderer work. Local drawer benchmarks
and extracted lifecycle fixtures establish the results below; integrated demo,
map, physical hardware and whole-frame FPS gates are owned by the coordinator.

## Runtime changes

- `src/r_draw8.c`: complete clamped texture posts reuse the existing opaque or
  translucent column drawer. The gate excludes short calls, zero texture height,
  incomplete posts and unsafe NPOT signed/overflow steps. Other calls retain
  their old clipping loop. No texture sample, colormap or blend order changes.
- `src/r_draw8_npo2.c`: the seven existing bounded NPOT fast span loops calculate
  the original inclusive framebuffer destination bound once, before drawing.
  The pixel coordinate DDA and all fallback paths retain their old semantics.
- `src/r_defs.h`, `src/r_segs.c`, `src/r_segs.h`: PS2/profile drawsegs hold a
  frontscale pointer and width rather than 640 inline fixed-point entries.
  Allocate only when FOF planes need the slot, grow to the current `viewwidth`,
  retain the high-water width and all previously written bytes, zero extensions
  via existing `Z_Realloc`. Row allocations have `PU_STATIC`, no owner pointing
  into the moving drawseg array, and remain valid across array reallocation and
  level-tag resets. The firstseg offset-zero rebasing bug is also corrected for
  PS2/profile growth: presence is kept separately from its index.
- `src/r_things.c`: ensure the current frontscale width when a polyobject plane's
  drawseg is bound to a node, without adding work to each sprite comparison.
  Polyobject planes can reference a slot that never owned FOF planes;
  its initial values must be zero, or retain prior values if the slot was used.
- `src/r_segs.c`: after the complete `R_RenderSegLoop`, PS2 releases the frame
  pin on completed opaque mid/top/bottom texture roots through the memory
  agent's `R_ReleaseTextureCache` API. This makes them eligible for pressure
  eviction, without immediately freeing them. All opaque column calls have
  returned at this point; drawsegs keep IDs/column indexes rather than cache
  aliases, and deferred masked/thick-side passes fetch their columns again.
  Three possible release calls per wall range add overhead that needs the
  coordinator's whole-frame measurement; the drawer microbench does not include
  this memory-pressure integration.
- `src/r_plane.c`: release a sky column root after the whole sky plane, and
  release the flat root after every completed non-fog plane via
  `R_ReleaseFlatCache(R_GetTextureNumForFlat(...))`. Release happens after all
  synchronous spans/columns have returned. Fog planes do not acquire or release
  flat sources, skipped/NULL-source paths return before release, and water's
  separate background screen remains intact. Sources remain cached until
  pressure eviction and every later plane fetches/touches its own root again.

Drawer gates use existing `PS2_OPT_DRAW`; memory representation uses
`PS2_PROFILE`. The ordinary PC representation and drawer paths remain unchanged.
No gameplay, physics, RNG, demo/save format or graphical feature was modified.

## Release EE drawer results

Existing `rdraw_bench.py`, `SRB2_PS2_RELEASE=1`, EE GCC, PCSX2 through the locked
`run_pcsx2.py` wrapper. Each row measures 48 draws, best of three repetitions,
subtracting dispatch overhead. Baseline is the pre-edit snapshot at
`build/ps2-next-render/base-src`, including all earlier renderer improvements.

| Workload | Base cycles/call | Candidate | Reduction |
| --- | ---: | ---: | ---: |
| Opaque clamped PO2 column, h128 / about 100 pixels | 1667.1 | 1337.7 | 19.8% |
| Opaque clamped NPOT column, h72 / about 100 pixels | 2069.1 | 1435.3 | 30.6% |
| Opaque clamped NPOT column, h100 / about 150 pixels | 3065.1 | 2065.1 | 32.6% |
| Translucent clamped PO2 column, h256 / about 190 pixels | 4344.5 | 3798.3 | 12.6% |
| Translucent clamped NPOT column, h100 / about 150 pixels | 4101.4 | 3690.7 | 10.0% |
| Ordinary NPOT floor span, about 320 pixels | 8702.6 | 8387.4 | 3.6% |
| Translucent NPOT span, about 320 pixels | 11883.7 | 10611.8 | 10.7% |
| NPOT water span, about 320 pixels | 11903.7 | 10949.0 | 8.0% |
| NPOT splat span, about 320 pixels | 9712.7 | 8825.5 | 9.1% |
| NPOT floor sprite, about 320 pixels | 12059.9 | 10541.2 | 12.6% |

All **207/207** framebuffer workload hashes match. Individual rows, hashes and
ELF SHA256 values are in `build/ps2-next-render/drawer-ee-report.json`.
Candidate microbenchmark ELF is 1,290,216 B, **128 B larger** than baseline;
SHA256 `c54502ee09dd8429692d8e5fb45706d5ad514d600ab88a48d37b6b9d7e609120`.

Dispatch has a small cost on fallback sprite posts: about 4–6 cycles/call,
0.4–2.1% in the measured post scenarios. Some unchanged short/aligned drawers
also move by a few cycles when the compiled code layout changes. The accepted
fast paths reduce the listed long workloads; these numbers are not a whole
engine FPS prediction. PCSX2 does not model physical EE cache stalls.

## Equivalence and allocation fixtures

All new fixtures compile the actual source/type with MSVC x86 and x64 `/W3 /WX`.

- Existing host stress: **315/315** workload hashes equal HEAD vanilla and the
  pre-edit baseline, 1024 input configurations per row. Broken drawer changes
  **249** row hashes and is detected.
- `next_render_columns_hosttest`: **94,080 cases** per architecture; compare all
  64,064 destination/canary bytes each call. Complete/truncated/zero/negative
  post lengths, short and long calls, zero/bounded/multiple-wrap/negative steps,
  negative/period starts, heights 0 through 16,384 and translucent lookup paths.
  Both architectures pass with zero byte differences. Broken-pixel mode exits 1.
- `next_render_spans_hosttest`: **6300 cases** per architecture, all seven NPOT
  drawers, sizes 1/7/8/9/31/32/33/319/320/321, final framebuffer rows and columns,
  positive/negative/zero steps and the original inclusive endpoint. Compare
  65,024 destination/canary bytes per call; zero differences. Broken mode exits 1.
- `next_render_drawseg_hosttest`: **13,644,453 checks** per architecture, zero
  failures. Extracts the real drawseg type, frontscale allocator and growth
  block. Forced moving reallocs, firstseg slot zero, portal cursor rebasing,
  32 frames with widths 320/640/1/319/160/512/640/320, retained sparse columns,
  zero extensions and non-owner polyobject plane readers. Wrong slot-zero
  rebasing and resetting old scale bytes each exit 1 on both architectures.
- After the flat/sky release insertion, rerun the existing plane-bank fixture:
  x86/x64 **11,908,801 checks**, zero failures, **21,782,400** clip bytes each
  equal the reference, SHA256
  `9933fbfa8595b6f3e8e666e35d66b9da5698a0296ece8426681d0ee603811552`.
  Both overlap-pad and skipped-growth negative controls exit 1. This exercises
  plane storage, not the PS2-only cache release callbacks; those require the
  coordinator's integrated release-pressure/golden runs.

| x86 synthetic bank | Old payload | Candidate | Savings |
| --- | ---: | ---: | ---: |
| Drawseg structure per slot | 2996 B | 444 B | 2552 B / 85.2% |
| First frame, 288 capacity, 13 rows at width 320 | 862,848 B | 144,512 B | 718,336 B / 83.3% |
| Final retained bank, 972 capacity and many widths/slots | 2,912,112 B | 1,356,980 B | 1,555,132 B / 53.4% |

These bank allocations are synthetic. Persistent scale row allocations are
counted in candidate payload, and would add individual zone header/padding in
the engine. If every slot eventually needs a 640-column row, sidecar metadata
adds overhead rather than saving memory. Thus the actual map peak still needs
an integrated retail-RAM run. The shrink/growth fixture preserves old bytes even
outside the current width so mode changes cannot discard previous values.

## Rejected compiler experiment

The frozen drawer sources were built and run again with EE GCC `-O2` and `-O3`,
using existing `rdraw_bench.py`, release flags and the locked emulator wrapper.
`SRB2_PS2_OFLAGS` is honored by its imported `build.py` flags. Output is at
`build/ps2-next-render/oflags/{o2,o3}` and `oflags/report.json`.

All **207/207** hashes match across optimization levels, but `-O3` is rejected:
25 rows improve, 141 remain within 0.1%, and 41 regress. The equally weighted
sum of per-row cycle costs rises from **393,034.7 to 395,046.4**, **0.51%**;
this is a workload summary, not an engine FPS result.

| Workload | `-O2` cycles | `-O3` cycles | Change |
| --- | ---: | ---: | ---: |
| Ordinary PO2 span, about 5 pixels | 164.3 | 203.1 | +23.6% |
| NPOT water span, about 320 pixels | 10949.0 | 11280.2 | +3.0% |
| NPOT splat, about 320 pixels | 8825.5 | 9083.3 | +2.9% |
| PO2 water span, about 100 pixels | 2353.5 | 2333.5 | -0.8% |

Separately compiling the actual engine `src/r_draw.c` at the two levels gives
`.text` **71,259 → 101,955 B**, **+30,696 B / +43.1%**, with `.bss` unchanged
at 6833 B. The microbenchmark ELF grows from 1,290,216 to 1,297,384 B. Larger
code also raises physical instruction-cache concerns that PCSX2 does not model.
No production compiler setting or `opt_units.txt` entry was changed. The
existing `-O2` is retained; no integration golden run is needed for a rejected
flag experiment that changed no runtime source or default flags.

## Commands and handoff

```powershell
$env:SRB2_PS2_RELEASE='1'
$env:SRB2_PS2_OUT='D:/Ai-Project3/SRB2-PS2-Port/build/ps2-next-render/ee'
python -B tools/ps2/rdraw_bench.py --out build/ps2-next-render/trial1 --base-src build/ps2-next-render/base-src --variants vanilla,base,cand --host --stress --negative-control
python -B tools/ps2/rdraw_bench.py --out build/ps2-next-render/trial1-ee --base-src build/ps2-next-render/base-src --variants base,cand --ee
python -B tools/ps2/next_render_columns_hosttest.py --out build/ps2-next-render/columns --base-src build/ps2-next-render/base-src --negative-control
python -B tools/ps2/next_render_spans_hosttest.py --out build/ps2-next-render/spans-final --base-src build/ps2-next-render/base-src --negative-control
python -B tools/ps2/next_render_drawseg_hosttest.py --out build/ps2-next-render/drawsegs-poly-final --negative-controls
python -B tools/ps2/plane_bank_hosttest.py --out build/ps2-next-render/planes-release --negative-controls
$env:SRB2_PS2_OUT='D:/Ai-Project3/SRB2-PS2-Port/build/ps2-next-render/syntax'
python -B tools/ps2/build.py --syntax src/r_draw.c src/r_segs.c src/r_bsp.c src/r_things.c src/r_plane.c
```

EE syntax: **5/5 translation units**, zero diagnostics. Source whitespace check
passes. No commits, staging, golden-data changes or direct emulator launches.
The shared deviation registry is owned by the coordinator; suggested additions
describe exact full-post/span fast paths and lazy width-aware drawseg frontscale
  storage with slot-zero rebasing, and completed wall/flat/sky cache eligibility.
  Integrated frame/tick equality, whole-frame
cycles, MAP11/other large maps on 32 MiB, view changes in the running game and
physical hardware performance remain separate gates.
