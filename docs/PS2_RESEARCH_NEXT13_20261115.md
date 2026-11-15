# PS2 architecture research addendum, pass 13 — 2026-11-15

This targeted continuation adds three inspected original developer papers to
the [existing architecture catalog](PS2_ARCHITECTURE_AND_OPTIMIZATION_SOURCES_20261115.md).
It maps their mechanisms to source snapshot
`353cc94103aeb38cef87d26151c779302135d0e9`. The earlier catalog and evidence
remain unchanged. This is not an exhaustive Internet survey, an accepted
optimization, or a measured RAM, VRAM or frame-rate saving.

The study used Internet search, body inspection and pinned maintainer source.
It ran no compiler, emulator or game. [Compact evidence](ps2-research-next13-20261115/evidence.json)
records checksums, page locators, source hashes and access differences; no
manuals, paper bodies, assets or BIOS files are distributed here.

## Newly inspected primary documents

| ID | Original author and checked document | Body inspected; practical addition |
| --- | --- | --- |
| N1 | Lionel Lemarié, [Texture Swizzling](https://ps2linux.no-ip.info/playstation2-linux.com/download/ezswizzle/TextureSwizzling.pdf), v1.0, 2003-06-20, PS2Linux edition, 8 pages; [original project archive](https://ps2linux.no-ip.info/playstation2-linux.com/projects/ezswizzle.html) | PDF pp1–6 and complete web-reader body. Pp2–4 describe a reversible byte permutation that uploads indexed images through CT32 while retaining their original sampling format. P5 gives compatible shapes and historical repeated-transfer measurements; incompatible cases require fallback. |
| N2 | Lionel Lemarié, SCEE Developer Support/Technology Group, [Reaching for the Limits of PS2 Performance: How Far Have We Got?](http://www.gamepilgrimage.com/sites/default/files/SystemSpecs/PS2/HowFarHaveWeGot.pdf), 2003, 34 slides | Actual PDF pp1,3,19,22–34, plus the archived slide transcript. Slide27 recommends narrow fullscreen primitives; slide19 discusses texture availability versus consumption. Slides25–26 motivate checking cache costs. These observations do not establish this port's bottleneck. |
| N3 | Mark Breugelmans, SCEE, [32bit Colour Channel Shifting using 8bit and 16bit texture formats](https://ps2linux.no-ip.info/playstation2-linux.com/download/p2lsd/32bit_colour_channel_shifting.pdf), cover date 26 October 2001–October 2004, 5 pages | All five pages. Format reinterpretation, channel masks and alternating column mappings support bit-preserving address-model experiments. The worked copies are not a general allocation policy or proof that unused-looking bits have no owner. |

These hosts archive the original authors' work; they are not current Sony
distribution endpoints. N2's HTTPS endpoint returned 503/direct and timed out
in the web reader. Its HTTP endpoint returned the inspected 34-page PDF.
N3's cover names Breugelmans while PDF metadata names Sarah Ewen; the evidence
preserves both and uses the cover for authorship.

## Application to this renderer

**Clear-strip prototype: rejected.**
[gs_fill_all](../src/ps2/hw/ps2_hw_gs.inc) emits one constant, untextured sprite
covering the viewport. Partitioning its existing integer X interval into
32-pixel strips admits a local coverage argument. It does not establish
unchanged resource lifetimes. The [GS manual](https://raw.githubusercontent.com/ninjadynamics/PS2Docs/main/GS_Users_Manual.pdf#page=46)
§3.2.9 p46 includes top/left pixel centers and excludes bottom/right centers;
§§3.2.1–3.2.2 pp37–38 define independent two-vertex sprites with flat shading
and no antialiasing. Adjacent strips therefore admit a half-open rectangle
partition with the same constant color and depth. The
[GS supplement](https://raw.githubusercontent.com/ninjadynamics/PS2Docs/main/GS_Users_Manual_Supplement.pdf)
§§1.1–1.2 pp6–8 explains why horizontal extent can incur page breaks.

This mechanism also appears in pinned PS2SDK
[draw_clear](https://github.com/ps2dev/ps2sdk/blob/b04e8973c8a9839d0653d87a8d30cc67fd3c3f05/ee/draw/src/draw.c#L149)
and [draw_rect_filled_strips](https://github.com/ps2dev/ps2sdk/blob/b04e8973c8a9839d0653d87a8d30cc67fd3c3f05/ee/draw/src/draw2d.c#L248).
Its fractional-coordinate convention differs from this driver's clear. An
isolated compact prototype retained the driver's coordinates and state and
passed extensive host coverage/state checks plus a complete actual EE build
and code binding with the existing per-unit `-O3`. A subsequent executed
literal-C counterexample rejected it: its extra 128 GIF bytes at width 320 can
force an earlier ring flush and release a previously completed DMA source.
The next cache allocation can then evict a different cached texture level.
Because this driver's decimation is non-associative, the changed source choice
produces 4,096 texels of index 255 holes where the control produces index 100.
The witness uses a constructed legal-input state and exercises both VIF modes.

The [clear investigation report](PS2_VRAM_NEXT13_20261115.md) records the
prototype, proof boundaries and rejection. Local clear coverage and register
equivalence did not preserve subsequent source-cache history. The actual
candidate also grows `.text` by 160 B and shifts linked `_end` upward 256 B.
No native fixture or completed-GS timing was run, and no clear change, memory
saving or speed improvement is adopted. The existing identity screen draw
already uses an exact local-to-local transfer.

**T8 upload permutation: unimplemented future candidate.**
[upload_ref and upload_bands](../src/ps2/hw/ps2_hw_tex.inc) currently transfer
native T8 rows. N1's alternative changes the transfer representation while
retaining the image's original format and dimensions in TEX0. Its p5 example
for 256×256 T8 reports 140 versus 179 repeated transfers per 200 scanlines,
about 1.28× throughput in that old microbenchmark; this is not a game-speed
prediction. Eligibility must prove identical touched GS byte addresses for the
existing allocation/stride, including small blocks and partial bands.

Keep original logical footprints, planner accounting, CLUT behavior, cache
keys and visible texture levels. Retain fallback without adding page padding,
duplicate residency or another allocation that changes failure/eviction
behavior. Preserve source pinning, cache writeback and PATH ordering. Runtime
permutation can cost more CPU/cache traffic than it saves; an offline exact
representation or bounded ring writer remains future investigation. The
clear counterexample also makes changed upload packet lengths and release
boundaries part of its proof obligation. No port implementation or benchmark
of this permutation was completed.

**Treat bitplanes as owned resources.** N3 supports reasoning about byte
addresses across formats. It does not authorize replacing this port's CT32
capture/wipe buffers with CT24, reducing depth precision, or overlapping
ordinary allocations. [Screen captures](../src/ps2/hw/ps2_hw_screen.inc) retain
color/alpha and the driver reads Z24 through Z32 layout. Every clear, draw,
transfer, readback and reset must respect any proposed bitplane ownership.

The prior [P4](PS2_P4_NEXT12_20261115.md) and
[visibility](PS2_VISIBILITY_NEXT12_20261115.md) counterexamples still apply:
fewer physical bytes or an invisible draw omitted today can change future
visible-texture choices. The executed clear counterexample extends that lesson
to a change confined to command encoding: unchanged logical footprints do not
establish unchanged DMA-source release, cache eviction or reconstruction.
Future candidates must preserve that complete behavior or prove an exact
lifecycle repair before their storage or timing results can justify adoption.
