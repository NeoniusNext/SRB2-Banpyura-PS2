# PS2 software drawer and visplane continuation — 2026-10-03

Implemented exact CPU rasterizer fast paths and reduced visplane storage. These
are local drawer measurements and allocation regressions; they do not establish
an overall engine FPS or a guarantee that every map fits retail RAM. Existing
uncommitted renderer/fixed-math changes were preserved.

## Changes

- `r_draw8.c`: four NPOT column drawers use a bounded positive-step loop without
  the original per-pixel overflow test and repeated wrap loop. The ordinary
  opaque column additionally draws groups of four when their texture samples
  cannot cross a wrap boundary. Original paths remain for other inputs. Clamped
  posts, transparent texels, colormaps and translucent lookup order are retained.
- `r_draw8_npo2.c`: seven flat NPOT span drawers use a signed DDA with one unsigned
  range check per coordinate, using a preselected positive or negative wrap
  correction. This applies only to spans of at least 32 pixels, canonical initial
  coordinates, periods at most 2^30 and steps strictly inside one period. Other
  inputs retain the original loops, including negative starts landing exactly
  at a texture period. No modulo approximation or slope precision change.
- `r_plane.h,c`: PS2/profile visplanes coallocate their top/bottom strips for the
  current view width, with separate pads at both ends. Recycling retains
  capacity; a recycled undersized plane is released and replaced before joining
  an active hash list. Active plane addresses remain stable. Width validation
  bounds all allocation arithmetic. Clearing touches the current view width.
- `r_plane.c`, `r_things.c`, `r_bsp.c`: persistent PS2 visplane/drawnode banks and
  polyobject-sort scratch use `PU_STATIC` zone storage rather than consuming the
  separate libc reserve. Polyobject allocation arithmetic is checked; its old
  scratch is released before allocating a replacement. Banks survive level-tag
  resets and are never classified as evictable texture caches.
- `r_segs.c`, `r_things.c`: drawseg and sprite-clipping range capacity grows by
  50% in the PS2/profile instead of doubling. Size arithmetic is checked; pointer
  rebasing, ordering and all actual entries retain their existing semantics.
- `m_fixed.c`: repaired the existing literal newline inside `PS2_MulU32` inline
  assembly to `\n\t`; this was a preexisting integrated EE compile blocker.
- `rdraw_bench.py`: fixed a false-success path: timestamped EE UART results were
  parsed as an empty set. Timestamp prefixes are accepted, and empty, incomplete
  or mismatched row sets now fail. Expanded host stress mode hashes every draw
  before later calls can overwrite a wrong pixel.
- New `plane_bank_hosttest.c,py` extracts the actual source functions and type,
  compares reference/candidate normalized clip bytes, checks allocation canaries,
  width switches, overlap splits, freelist reuse, persistent tags and cleanup.

CPU fast paths are guarded by the existing `PS2_OPT_DRAW` policy; NOOPT takes the
original path. Dynamic visplanes and growth use PS2/profile guards. Ordinary PC
visplane layout and visible clipping results remain unchanged. No gameplay,
physics, RNG, save/demo format or graphical feature was changed in this work.

## Measured CPU results

Release EE microbench, PCSX2 through the existing locked `run_pcsx2.py` wrapper,
48 calls per row, best of three repetitions minus dispatch overhead. The base
source snapshot is `build/ps2-sw-draw-continuation/base-src`, taken before these
drawer changes. Final results combine the unchanged base run in `finalbench` and
the final drawer candidate in `unrollbench`.

| Drawer / workload | Base cycles/call | Candidate | Reduction |
| --- | ---: | ---: | ---: |
| Opaque NPOT column, h72 / about 100 pixels | 2162.1 | 1396.3 | 35.4% |
| Opaque NPOT column, h100 / about 150 pixels | 3206.8 | 2026.1 | 36.8% |
| Opaque NPOT column, h24 / about 30 pixels | 715.6 | 500.0 | 30.1% |
| Clamped NPOT column, h100 / about 150 pixels | 3957.7 | 3065.1 | 22.6% |
| Masked NPOT column, h100 / about 150 pixels | 3655.5 | 2914.6 | 20.3% |
| Translucent masked NPOT column, h100 / about 150 pixels | 4705.5 | 3964.5 | 15.7% |
| Ordinary NPOT floor span, about 320 pixels | 9569.3 | 8702.6 | 9.1% |
| NPOT water span, about 320 pixels | 12449.0 | 11903.7 | 4.4% |
| NPOT splat span, about 320 pixels | 10195.2 | 9712.7 | 4.7% |

All **207/207** EE drawer/workload framebuffer hashes match the base. Full
per-case cycles, hashes and measured ELF SHA256 values are in
`build/ps2-sw-draw-continuation/drawer-ee-report.json`. Candidate microbench ELF is
1,290,088 bytes, SHA256
`cbd1fc922e90fc74fedd676e16147cb79e5149755f8d6429a2609bdc71258270`.

NPOT floor-sprite fast paths gain only about 0.4–0.8% on long spans. Dispatch adds
approximately 1–6% to some short fallback workloads. PO2 output/code paths were
not changed. An attempted 64x64 constant-shift specialization was rejected after
its long-span measurements showed no worthwhile gain and short spans regressed.
PCSX2 does not model physical EE cache stalls; these percentages apply to the
listed instruction workloads, not whole frames or physical hardware.

## Memory and equivalence evidence

The actual visplane code was compiled with MSVC **x86 and x64, `/W3 /WX`**. The
fixture allocates 2400 planes and exercises widths
`320,320,640,319,1,160,640,640,320,512,2,640`, including overlap-created planes,
empty-plane reuse and separate top/bottom sentinel pads.

| Fixture | Original payload | Candidate payload | Savings |
| --- | ---: | ---: | ---: |
| x86, first 320-wide frame, 2400 planes | 6,412,800 B | 3,379,200 B | 3,033,600 B / 47.3% |
| x64, first 320-wide frame, 2400 planes | 6,470,400 B | 3,456,000 B | 3,014,400 B / 46.6% |

This is a deliberately heavy synthetic bank, not an observed engine map peak.
The x86 saving is **1264 bytes per 320-wide plane**. At 640-wide capacity the x86
dynamic representation costs an extra **16 bytes/plane** (x64: 24 bytes) for its
pointer/capacity metadata. Capacity remains at the high-water width after shrink.
The persistent zone allocation also changes where memory is charged, so map zone
usage alone should not be compared against an old libc-backed bank as total RAM.

Each architecture compared **21,782,400 raw clip bytes**, with zero differences.
SHA256 for reference and candidate is
`9933fbfa8595b6f3e8e666e35d66b9da5698a0296ece8426681d0ee603811552`.
Final candidate: **11,908,801 checks, zero failures** on each architecture. Wrong
pad placement and disabled growth both exit 1; the latter is detected before an
out-of-bounds write. Allocated payload is balanced at fixture cleanup.

Drawer host stress: **315/315** workload hashes match both HEAD vanilla and the
pre-edit base, with **1024 input configurations per row**. It covers span sizes
1/2/3/7/8/15/16/31/32/33/319/320, signed random starts, exact positive/negative
periods, zero/boundary steps, repeated wrap fallback, water, translation,
transparency and blending. A deliberately wrong drawer shifts **249** workload
hashes and is detected. Per-call row/column hashing prevents subsequent draws
from masking an earlier error; raw full-frame engine equivalence is a separate
integration check owned by the coordinator.

## Commands and artifacts

```powershell
$env:SRB2_PS2_OUT='D:/Ai-Project3/SRB2-PS2-Port/build/ps2-sw-draw-continuation/ee'
$env:SRB2_PS2_RELEASE='1'
python -B tools/ps2/rdraw_bench.py --out build/ps2-sw-draw-continuation/finalbench --base-src build/ps2-sw-draw-continuation/base-src --variants base,cand --ee
python -B tools/ps2/rdraw_bench.py --out build/ps2-sw-draw-continuation/unrollbench --variants cand --ee
python -B tools/ps2/rdraw_bench.py --out build/ps2-sw-draw-continuation/stress-each-call --base-src build/ps2-sw-draw-continuation/base-src --variants vanilla,base,cand --host --negative-control --stress
python -B tools/ps2/plane_bank_hosttest.py --out build/ps2-sw-draw-continuation/planes-final --negative-controls
$env:SRB2_PS2_OUT='D:/Ai-Project3/SRB2-PS2-Port/build/ps2-sw-draw-continuation/syntax-final'
python -B tools/ps2/build.py --syntax src/r_draw.c src/m_fixed.c src/r_plane.c src/r_things.c src/r_bsp.c src/r_segs.c src/ps2/ps2_mem.c
```

Final EE syntax compile: **7/7 translation units, zero diagnostics**. Final source
whitespace checks passed. No commits, staging, golden-data rewrites or direct
PCSX2 launches were performed.

## Remaining integration gates / registry handoff

- Integrated EE/host demo equality, whole-frame timing, large-map/32-MiB runs and
  live mode changes are coordinated separately. These local fixtures cannot
  guarantee general map fit or physical PS2 performance.
- Drawseg/range growth and the PS2-only drawnode/polyobject allocation migration
  rely on those integrated runs for gameplay-scene coverage; the plane-bank
  fixture directly verifies the visplane path.
- No shared-header edits beyond the explicitly delegated `r_plane.h` were needed.
  The coordinator separately owns lazy-table alignment changes in `r_draw.c`.
- Suggested registry entries, reserved by the coordinator: **PS2-30** CPU NPOT
  columns/spans; **PS2-31** dynamic visplanes, persistent zone scratch and 50%
  render-buffer growth. The coordinator writes `DEVIATIONS.md`; this agent did
  not edit the shared registry.
