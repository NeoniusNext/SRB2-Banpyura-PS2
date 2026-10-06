# GS hardware renderer: final report (agent 1) — WORK IN PROGRESS

This file is updated after every stage so that the work can be continued if the session is cut. Numbers below are
copied from real runs (log paths given); anything not run is marked **not run**. Design reference: `docs/HW_RENDERER.md`.

## Stage log

### Stage 1a — layout, format, texture exactness (done, standalone tests only)

Changed: `src/ps2/hw/ps2_hw_{priv,gs,tex,draw}.inc`, `ps2_hwd.c`, new `ps2_hw_{light,screen}.inc`.

* Frame buffers are **CT32 by default** (RGB555 removed from the default path; CT16S stays only as an explicit opt-in
  `fb32 = 0` and as the automatic fallback when CT32 would leave < 48 pages for textures, which warns and sets limitation bit
  `HW_SCREEN_LOSS`).
* The frame buffer is the **internal picture** (docs/VIDEO_MODES.md): `vid.width x vid.height` rounded up to
  a size the CRTC magnifies by a whole number (320x200 on NTSC -> 320x224, CRTC x8 horizontally, FRAME read mode,
  output format 3). 4 MiB VRAM at 320x224 CT32: 2 x 280 KiB buffers + 280 KiB Z + 124 KiB CLUT area + **3128 KiB texture
  pool** (was 728 KiB at 640x448 CT32).
* Textures keep their exact size (<= 1024 texels per side) as PSMT8 + CLUT or CT32 with the GS alpha scale; no power-of-two
  stretching, no RGB5551, no 256 KiB budget decimation. Repeats of non power-of-two textures are made by cutting the polygon
  at whole texture widths (code in place, test: not yet).
* Screen captures are full size (internal resolution) GS local-to-local copies.

Evidence: `build/agent-hw-dev/hwt-q3.txt`: `H0 COMPLETE checks=41 failures=0` (quick mode, CT32 320x224).

(Further stages are appended below as they are completed.)
