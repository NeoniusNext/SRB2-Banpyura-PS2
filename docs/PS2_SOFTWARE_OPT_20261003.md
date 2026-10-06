# Software renderer / retail RAM — 2026-10-03

Continuation of the native EE/GS software port. The starting tree already
contained unfinished port changes; this work does not reset or replace them.
The optional GS hardware renderer is not the renderer used for these runs.

## Exact CPU and GS work

- Bounded wrapping and four-pixel loops for NPOT wall columns; bounded signed
  wrapping for NPOT flat spans. EE microbench: columns save 14–36.8% of cycles,
  long basic NPOT spans 9.1%, water 4.4%, splats 4.7%. Short fallback overhead
  and the distinction between microbench and game FPS are recorded in
  [PS2_SW_DRAW_CONTINUATION.md](PS2_SW_DRAW_CONTINUATION.md).
- GIF/DMA packet reuse and unchanged-palette upload/CLUT reload suppression.
  Mean GS submit 9607 → 9217 COP0 cycles (4.06%); this is only the submission
  phase. DMA lifetime/timeout guards and staging shutdown cleanup retain
  source/VRAM correctness. 350 output/source/fit readback cases have no pixel
  differences. Details: [PS2_GS_SOFTWARE_CONTINUATION.md](PS2_GS_SOFTWARE_CONTINUATION.md).
- Native-rate audio voices use a contiguous PCM cursor while retaining the
  exact fractional cursor and EOF state. The four 64-voice native-rate cases
  save 19.6–21.9% of EE mixer cycles. Host and EE dual implementations compare
  2048 PCM/state results; the larger audio suite compares 5,944,488 stereo
  frames, 384 native-rate cases and 20,000,000 PCM conversion values.

## RAM working set

- Actual-view-width visplane clips retain all pixels, padding and high-water
  capacity. At 320 wide the x86 representation saves 1264 bytes per plane;
  the deliberately heavy 2400-plane fixture saves 3,033,600 bytes. This is
  synthetic capacity, not a measured map saving. At width 640 the extra
  metadata costs 16 bytes per plane.
- Persistent render scratch belongs to the zone rather than an unaccounted
  libc pool. Drawseg and range capacities grow by 50% with overflow checks.
- Exact two-pass blockmap lists remove temporary per-cell allocations;
  singleton tags use existing structure padding; sector-line references
  share one level allocation. Blockmap ordering and sector reference order
  remain unchanged.
- Raw flat ownership is transferred instead of duplicating the lump. Composite
  opacity uses a packed bitset. The output reuses temporary pixel storage and
  raw Doom/cooked patch posts are consumed directly with the original four
  copy/flip/blend routines, without a persistent converted-patch duplicate.
- Optional precache respects the configured render headroom. Insufficient
  headroom skips warming reconstructible caches; first-use generation remains
  available with the same content.
- PS2 C blend/TRANS tables use 64-byte alignment instead of 64 KiB. The PC
  profile retains alignment needed by its assembly renderer. All defined
  blend-table bytes match the original in forward/reverse/shuffled orders,
  after purge and after LUT reconstruction. The original undefined index-255
  row/column is excluded from comparison, without assigning new values.

## Verification reliability

The profiler sums exclusive 64-bit phase accumulators; a long window no longer
wraps its total at UINT32. Its output and informational PS2 audio messages use
the log-only output path, leaving the vanilla console HUD intact. A host
fixture validates an 8,000,000,164-cycle report, a wrapped 64-cycle interval
and counter reset.

`opt_run.py` now fails on engine OOM/heap errors even if PCSX2 exits with zero,
and fails when the requested completion marker is absent. The drawer parser
handles UART prefixes and rejects zero/incomplete comparisons. An earlier
apparent MAP11 success was an emulator process exit after a fatal OOM; it is
not counted as passing.

Integrated results and the final ELF identity are filled after the final
source freeze. All emulator invocations use `tools/ps2/run_pcsx2.py` and its
shared lock, with retail `ExtraMemory=false` (32 MiB).

## Limits

PCSX2 cycle counts measure emulated instruction cost; EE caches and physical
display timing require a real console. A finite 32 MiB machine cannot hold
arbitrary mandatory map data losslessly. Successful bounded runs certify the
specified maps and tested views, not every route, custom map, live transition
or indefinite soak. Existing profile restrictions, including Lua/UDMF and
addon support, are described by the existing port documents and were not
introduced by these optimizations. Golden inputs are not rewritten.
