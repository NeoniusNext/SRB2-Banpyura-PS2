# GS presentation continuation — 2026-10-03

This work changes the GS presentation backend of the **software renderer**, not the optional hardware renderer. Indexed rendering, physics, game logic, source/output resolutions, filtering and VRAM layouts remain unchanged.

## Changes

- `src/ps2/ps2_gs.c`: retain the GIF/DMA packet between ordinary frames, patching only the source REF address, back-buffer FRAME and TEX0 CLD. Rebuild on source/output/fit/filter changes or when a black-bar clear is needed. The existing 2 KiB chain is reused; no frame copy or second packet is allocated.
- Include the optional palette transfer at the packet head and start the DMA after it when the CLUT is unchanged. TEX0 CLD=0 retains the internal GS CLUT in that case. Compare palette entries after RGB masking/swizzling so identical palette updates do not upload or reload it.
- Retire a buffer's black clear only after submitting its packet. Previously, a vblank flip during construction could cause a retry to lose the pending clear.
- Reject source changes and submissions when the preceding DMA/FINISH wait times out. Check DMA completion before copying/reallocating the staging buffer. Release the fallback staging buffer on normal GS shutdown; retain it if DMA timed out and may still reference it.
- `src/ps2/ps2_gs.h`: expose packet rebuild and palette upload counts for verification.
- `tools/ps2/video_test.c`: repair missing engine diagnostic stand-ins, assert unchanged palette/packet reuse, assert staging release/restart with exact palette readback, and fix a tautological refused-output assertion.

## Executed checks

Builds use `SRB2_PS2_RELEASE=1`, `SRB2_PS2_OUT=build/ps2-gs-sw-continuation/<base|opt>` and `python tools/ps2/build_video_test.py`; compiler output is empty. PCSX2 runs exclusively through `tools/ps2/run_pcsx2.py`, default 32 MB configuration and shared lock.

| Check | Measured result |
|---|---|
| Baseline NTSC `ntsc frames=600 matrix=0`, `base/run.log` | `failures=0`, mean submit 9,607 COP0 cycles |
| Optimized same scenario before extra restart check, `opt/run.log` | `failures=0`, mean submit 9,217 cycles: **4.06% less** |
| Final NTSC including staging restart, `opt/final.log` | `failures=0`, 600 frames, mean submit 9,220 cycles, 10 full-screen soak readbacks with 0 mismatches, 0 DMA/FINISH timeouts |
| Cached palette/packet assertions | 8 repeated palette updates/frames: uploads 3 → 3, packet builds 3 → 3; cached output has 0 pixel differences |
| Staging shutdown/restart | **64,016 heap bytes released**, restart output/palette readback has 0 differences |
| Final frame packet counters after restart | 683 frames, 3 full packet builds, 24 CLUT uploads |
| Mode changes | 44 changes through engine API, all screen slices aligned; heap usage 1,944,104 → 1,944,104 bytes |
| Auto-region PAL soak + complete output/source/fit matrix, `opt/matrix-auto.log` | `failures=0`, **350 cells, 0 actual mismatching pixels**, 25 BIOS-supported outputs; 576p correctly refused by BIOS 2.00 |
| Host `python tools/ps2/video_hosttest.py --negative-controls` | `failures=0`; all five negative controls detect their intended defects |
| GS object size (`size`, base → opt) | text 9,033 → 9,441 bytes; bss 3,260 → 3,288 bytes; data remains 16 bytes |

The matrix command is `python tools/ps2/run_pcsx2.py --elf build/ps2-gs-sw-continuation/opt/VIDEO_TEST.ELF --log build/ps2-gs-sw-continuation/opt/matrix-auto.log --args='frames=600' --timeout 300 --until 'V0 COMPLETE'`. Do not force `ntsc`/another output when testing output menu changes: command-line output selection intentionally overrides the menu. An earlier `opt/matrix.log` run forced NTSC and consequently reported four output-change failures; the corrected auto-region run passes all outputs.

The complete matrix was run before moving the preceding DMA check ahead of staging copy; the final NTSC run exercises that final reorder and the new shutdown/restart checks. Rendering/packet contents are the same in both runs.

## Limits

This is about 387–390 saved COP0 cycles per GS submission, **not** a 4% improvement of the entire game frame. CPU software rasterization and map memory remain separate bottlenecks. The indexed texture and framebuffer allocation sizes are unchanged; persistent static RAM increases by 28 bytes, while fallback staging releases up to the current source size (64,016 measured allocated bytes at 320×200). Ordinary aligned engine screens never need staging.

PCSX2 does not model EE cache costs or prove physical console display timing (especially VGA modes). Cache writeback remains `FlushCache(0)`; no cache-coherency optimization was asserted from emulator-only results. Hardware-induced DMA/GS timeout recovery itself is not fault-injected. The tests validate pixels, palette changes, buffer lifecycle and emulated instruction timing, not a universal large-map/32 MB guarantee.

## Proposed deviation entry (coordinator owns shared registry)

`PS2-40 | Только PS2 (src/ps2/ps2_gs.c/.h, software GS presentation) | GIF/DMA-пакет сохраняется между кадрами, обновляются REF/FRAME/TEX0; неизменная палитра не передаётся и CLUT GS не перезагружается. Очистка границ снимается только после реальной отправки пакета; fallback staging освобождается после завершения DMA при shutdown; предыдущий timeout останавливает смену source/следующую отправку. Разрешения, nearest/bilinear, индексные пиксели и VRAM layout сохранены. PCSX2: NTSC/PAL по 600 кадров, 350 readback-комбинаций — 0 px различий/таймаутов; submit 9607→9217 тактов (-4,06%), staging release64016 Б; отчёт docs/PS2_GS_SOFTWARE_CONTINUATION.md.`

No changes to shared engine headers or build scripts are required. The coordinator should add the PS2-40 registry row and include the backend in the final integrated build.
