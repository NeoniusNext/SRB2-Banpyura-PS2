# PS2 renderer continuation — 2026-10-02

**Result: high-impact correctness/performance fixes implemented and verified;
full lossless PC HW parity remains incomplete.** The current capability and
budget inventory is [HW_RENDERER.md](HW_RENDERER.md). This report supersedes the
older half-resolution capture/budget descriptions in HW_INTEGRATION.md, without
closing its engine-scene, switching, owner-lifetime or physical-hardware gates.

## Changes in this continuation

1. **Exact texture dimensions and RGB.** Removed automatic GS-limit/config-budget
   decimation. Arbitrary RGBA retains CT32 RGB; opaque palette index 255 is not
   remapped. RGBA/AP88 alpha-zero RGB also survives: indexed conversion is refused
   when a keyed CLUT would change RGB contributing to bilinear filtering. Exact
   dimensions up to 1024 are supported; larger dimensions require tiling and fail
   explicitly. `tex_max_bytes` now limits allocated GS blocks, including padding.
   A request larger than the entire pool fails before evicting existing records.
2. **Less upload overhead.** Removed per-upload heap staging allocation and the
   4096-byte identity/resampling maps; exact row/index addressing uses a small
   stack context. Banded IMAGE packets still copy engine data into owned DMA RAM.
3. **Lossless capture residency.** All five full-resolution slots survive texture
   pressure. Captures spill through synchronized aligned GS readback to bounded
   EE storage and restore via raw band uploads. A failed allocation/readback never
   authorizes capture discard. Resident slots can retain their backing to avoid
   repeat readbacks. Overwrite, FlushScreenTextures and Shutdown release backing.
   Wipe masks and selected resources are protected during restore. New accounting
   exposes current/peak EE backing and spill/restore counts.
4. **Exact identity presentation and final aspect.** Identity screen draws use
   local GS copies rather than bilinear triangle rasterization. This eliminated
   observed black/white 0/255 -> 1/254 stripe rounding, and replaces raster packets
   with five transfer-register writes. The implicit final path preserves its own
   source. Letterboxing captures before clear and respects the requested aspect.
5. **Selected-slot screenshots.** RGB readback uses saved GS/EE slot contents,
   top-down bilinear pixel-centre sampling, and no framebuffer redraw. Backed slots
   do not need GS restoration for screenshots. Only absent GENERIC2 may fall back
   to the completed framebuffer; GENERIC3 no longer silently aliases it. Explicit
   boolean APIs preserve the caller's destination on failure; the void callback
   invokes the registered resource failure handler (standalone fallback zeros RGB).
6. **Perspective texture correctness.** PACKED ST latches Q inside GIF but does
   not commit it to GS RGBAQ. Restored per-vertex RGBAQ commits before XYZ in
   perspective/fog/ramp packets, including lists and overlay staging. Invalidated
   the A+D colour/Q cache after packed writes. Retained one-word untextured and
   compact two-word Q=1 affine fan paths.
7. **Bounded NPOT repeats without truncation.** More than 40 repeat cuts per axis
   continue iteratively in bounded staging, rather than clamping the remainder.
   Host verification covers 98×84 periods, total projected area and period UVs.
8. **Blend/lighting fixes.** Polygon alpha 128 passes the PC masked >0.5 boundary.
   Where valid, masked alpha multiplication is folded into modulation to remove
   a second GS rounding. Palette multiplicative CLUTs quantize the final texture ×
   polygon channel factor once and leave keyed holes' destination colour intact.
   Ordinary explicit shader-lighting passes cut at actual staircase boundaries
   instead of sampled ramp bins. Lit fog ramps remain experimental. Palette/CLUT
   replacement and texture invalidation flush queued overlays; ramp creation
   protects and reacquires source records across eviction/table reallocation.
9. **Honest capability and framebuffer policy.** InitShaders and CompileShader
   return false; GLSL load attempts warn. Fixed GS passes are not advertised as
   complete built-in/custom shader support. Water ripple requests warn. Requested
   CT32 precision is never silently changed to CT16S; impossible layouts fail Init.
   Explicit CT16S use diagnoses RGB and destination-alpha loss, including dithering.
10. **Test integration repaired.** Updated the existing host suite to the current
    pass planner, packet formats, CLUT state and conversion signatures. Included
    the previously disconnected extended EE fixture in `hw_test.c`, selectable
    with `ext`. Corrected its infinite-plane oracle to the submitted finite floor
    and its zero-valued enum mistakenly disabling translucent lighting cases.
    Expanded eviction pressure so it exceeds both default framebuffer-mode pools.

Edits are confined to `src/ps2/hw/*`, the four `tools/ps2/hw*` test files,
HW_RENDERER.md and this report. Existing driver/integration work was extended.

## Verification and artifacts

The `build` parent and `D:/ps2dev` were verified with Test-Path before generated
builds. EE and emulator output is isolated in `build/ps2-renderer-check`; host
output is in `build/ps2-renderer-host-check`. PCSX2 used the existing locked wrapper
and its real-32-MiB configuration.

```powershell
python -B tools/ps2/hw_hosttest.py --out build/ps2-renderer-host-check --negative-controls
$env:SRB2_PS2_OUT='D:/Ai-Project3/SRB2-PS2-Port/build/ps2-renderer-check'
python -B tools/ps2/run_hw_test.py --tag renderer32-full --timeout 900 -- soak=600 ext
python -B tools/ps2/run_hw_test.py --no-build --tag renderer16 --timeout 240 -- quick soak=0 fb16
```

| Check | Result |
|---|---|
| x64 and x86 host, `/W3 /WX` | **17/17 groups each**, all **18** negative controls fail in their intended group(s) |
| EE compile and link, real driver | **Zero diagnostics**, `-O2`, `-Wall -Wextra -Werror`; HW_TEST.ELF **1,722,124 bytes** |
| CT32 NTSC 320×224, extended suite + 600-frame soak | **77/77 checks**, zero timeouts, zero repeat-render pixel mismatches, shutdown heap baseline restored |
| CT16S NTSC 320×224, quick compatibility suite | **48/48 checks**, zero timeouts/leaks; limitation mask **0x6000** records its intentional colour/alpha loss |
| Tilted CT32 texture, independent double perspective oracle | **0/15361 wrong texels** away from raster/texel boundaries |
| Full-resolution identity screen detail | **0 changed pixels** for one-GS-pixel black/white stripes |
| Five captures under exact 3-MiB texture pressure | **Five spills/five restores**, all selected-slot RGB and restored frame pixels correct |
| CT32 continuous/tinted wipes | **64 mask levels**, normal plus four tinted directions; all checked channels within **2** of PC equations |
| Extended blend/lighting | Eleven blend fixtures and seven actual-boundary solid/translucent lighting fixtures pass stated **2-RGB-level** tolerances |

Final ELF SHA256:
`953f20a5b107510407a5485a6e77878fd50c4160f72c8568e81078c89b096541`.

See `hwt/build.log`, `hwt/link.log`, `hwt/build-report.json`,
`hwt-renderer32-full.{log,txt,json}` and `hwt-renderer16.{log,txt,json}` under
the isolated EE directory. Host build/test/negative-control logs are under the
host directory. The passing CT32 workload has limitation mask 0; it never requests
the missing effects below. It is not a shader/engine parity result.

Measured default CT32 GS pool: **3,203,072 bytes**. Capture EE backing peaked at
**1,433,600 bytes** (CT16S: **716,800**), then returned to zero. These measurements
are not total engine RAM peaks. PCSX2 timing for this run: approximately **2900 EE
cycles/textured quad**, **1342 cycles/indexed triangle**, **2214 cycles/flat quad**
in the standalone submission fixtures. These are emulator COP0 measurements, not
physical-PS2 or integrated map frame rates.

## Interface handoff to video/build/memory owners

* Recompile all users of `ps2_hwd_dbg.h`: config appends `screen_max_bytes`; stats
  append `screen_spills`/`screen_restores`; info appends `screen_bytes` and
  `screen_peak_bytes`. Initialize configuration with GetConfig or zero the whole
  struct. `screen_max_bytes=0` selects an **8-MiB backing cap**; coordinate a smaller
  engine-aware cap using actual zone/model/batch/map peaks. `tex_max_bytes=0` uses
  pool capacity; positive values now reject excess GS footprint instead of shrinking.
* Existing `PS2HWD_SetFatalHandler`, CaptureScreen and ReadScreenRGB declarations
  are retained. The engine resource handler must not return; boolean capture/read
  results must be checked. Preserve fresh GENERIC2 capture/read ordering around
  screenshot/final drawing, and explicitly capture GENERIC3 only when its intended
  image exists. The driver does not implement palette postprocessing.
* InitShaders=false must retain the engine's nonshader lighting/fog fallbacks.
  Explicit SetShader fixtures exercise only selected GS equivalents, not shader
  compilation or global shader capability. Validate startup/switch routing.
* The build owner should track/hash all driver includes **and hw_test_ext.inc**;
  the current standalone build report hashes driver includes but omits this test
  include. The real driver must be selected for engine visual validation.

## Exact remaining blockers

1. **Single-object GS limits.** >1024-axis textures and textures/captures larger
   than the pool require lossless tiling/compositing. Full-resolution 640-wide CT32
   captures exceed every current full-height pool: NTSC **622592 bytes**, PAL
   **131072**, 480p **376832**, versus a capture of **1146880/1310720/1228800** bytes.
   EE spill cannot make one such object resident. Budgets/Init fail instead of
   silently reducing resolution or precision.
2. **Missing programmable effects.** Custom GLSL, complete built-in shader parity,
   water ripple/refraction/time effects, RGB-to-palette lookup, screen palette and
   per-fragment palette/light-table effects remain unavailable. Direct-colour and
   nonwhite-modulated tint, lit fog ramp boundaries, all combined shader/blend/alpha
   paths and shader model normal lighting are not complete.
3. **General blend/precision parity.** Partial-texel-alpha source subtraction,
   direct-colour multiplication and keyed multiply/depth combinations need more
   compositing. GS alpha/filter/blend quantization and general masked alpha-boundary
   cases are not bit-identical PC arithmetic. Passing a 2-level fixture tolerance
   does not satisfy universal no-loss output. CT16S remains lossy opt-in.
4. **Other missing capabilities.** Mip chains/mixed LOD/anisotropy, wireframe and
   corona depth visibility remain unsupported. Underwater/heat grid, sky caps and
   real model assets/materials still need independent engine reference scenes.
5. **Integration/physical evidence.** Paired PC HW map/HUD/sprite/FOF/slope/sky/model/
   fog/water/wipe frames, splitscreen, every palette/screenshot path, loaded-map
   mode switching and live-owner destruction are outstanding. PAL/480p/high modes,
   complete 32-MiB engine peaks, allocation/fault recovery, real GS/cache behavior
   and engine-scene timing require coordinated validation.
