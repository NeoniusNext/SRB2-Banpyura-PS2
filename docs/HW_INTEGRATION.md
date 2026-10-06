# PS2 HW integration handoff

The GS renderer remains **experimental** until engine scenarios and the feature
matrix in [HW_RENDERER.md](HW_RENDERER.md) are verified. This worker changed only
the assigned driver/hardware/test files and these new documents. The integrated
build and `src/ps2/i_video.c`, `tools/ps2/build.py`, deviations/G1 belong to the
coordinator.

## Existing integration inspected

* `SRB2_PS2_HW=1` removes NOHW, defines HWRENDER and adds
  `tools/ps2/sources_hw.txt`. That list contains engine hardware sources and
  `ps2_hwd.c || ps2_hwd_null.c`; the null selection is logging-only.
* `i_video.c` has `Impl_HWAcquire/Release`, renderer selection, HWD initialization,
  HWR_Startup/Switch, hardware FinishUpdate and optional hwdump/hwtoggle/hwstats.
  `ps2gs_shutdown()` precedes HW acquisition; the software driver is restored on
  Init failure. This is necessary exclusive GS ownership, not standalone proof
  that the mode-switch lifecycle works.
* Existing PS2_PROFILE branches keep map textures P8 and patches AP88, reduce
  initial batching arrays, remove OpenGL-only include use and unused light-table
  freeslot initializers, and use camera-aligned shadows when Lua state is absent.
  They were retained. The new batch allocation checks remain PS2_PROFILE-only.

## Required coordinator checks/fixes

### 1. Capture the right image for screenshots and hwdump

`HWR_GetScreenshot()` reads GENERIC2 (GENERIC3 only with palette shaders).
The driver now obeys the requested slot; it no longer silently returns whatever
framebuffer is current. `Impl_FinishUpdateHW()` currently calls FinishUpdate and
GClipRect but does not itself ensure GENERIC2 was freshly captured.

Before `Impl_DumpHW`/HWR_GetScreenshot, ensure an appropriate fresh
`HWR_MakeScreenFinalTexture()` / HWD MakeScreenTexture(GENERIC2), and verify its
success. The callback is void: a missing slot currently warns and leaves the
destination untouched. `HWR_GetScreenshot` nevertheless returns its malloc
buffer, so blindly writing it can produce stale or uninitialized RGB. An explicit
platform readback status/capture contract is required for a trustworthy engine
oracle. Hardware palette rendering is unavailable; GENERIC3 cannot be assumed.

The normal final-presentation flow also needs review: on PC it captures/draws
screen textures and handles aspect. GS retains the previous picture itself,
but half-size screen textures and ignored final width/height cannot be treated
as lossless equivalents.

### 2. Switches, palettes and texture-owner lifetime

Test Software -> HW -> Software -> HW, including loaded-map and title-menu
switches, before exposing the mode as verified. Check HWR map/poly/cache flags
and allocations, GLMipmap owner lifetime, and palette reapplication. Clear/drop
driver handles **before** freeing their owners; Shutdown dereferences them to
clear `downloaded`. Existing Release calls HWD Shutdown directly; coordinate the
engine's cache/map cleanup with that lifetime rather than assuming HWR_Shutdown
resets every flag (`gl_init`, `gl_maptexturesloaded`, `gl_maploaded`).

`I_SetPalette` currently calls `ps2gs_set_palette` unconditionally. Validate
whether V_SetPalette's hardware path already invokes HWR_SetPalette in every
startup/switch/fade case. If not, route active-HW palette changes through
HWD SetTexturePalette and retain the latest palette for software reacquisition.
No palette/screenshot result from this worker's standalone harness establishes
that engine routing.

### 3. Pool/RAM failure policy and quality

Expose/record framebuffer format and configured texture limits when testing.
Default CT16S has colour loss; CT32 consumes 3.36 MiB for two FBs and Z at NTSC,
leaving only 728 KiB of pool. Two half-size CT32 captures fit, five do not.
Do not silently discard a live capture, substitute reverse subtraction for
source subtraction, or disable an effect to claim completion.

The engine can retain GENERIC1/2 alongside both wipe images. Decide and verify
the required lifetime/release points or a lossless storage/compositing design.
The driver pins captures and reports exhaustion. Test per-tag zone usage,
raw malloc/high-water driver buffers, batching growth, model storage and readback
temporaries together; the standalone 600-frame soak is not a map RAM budget.

### 4. Build and profile checks

The optional hardware build must fail on compiler/link diagnostics, link the real
driver, include all `.inc` dependencies, and rebuild after driver/header/config
changes. The inspected `build.py` already has optional sources and -lm/gsKit;
its alternative selection can still silently fall back to null if the real
source is absent. Release/test metadata should name the selected driver and
reject null selection for visual validation.

The standalones enforce empty compile **and link** diagnostics and save command,
source/ELF SHA256 metadata. The inherited integrated builder's exit policy does
not by itself enforce zero warnings. The coordinator must validate its integrated
build.log/link.log, ELF/section size and 32-MiB profile independently. This worker
ran only selected-file EE syntax plus standalone builds, not the competing
integrated engine build. Core PC paths are protected by PS2_PROFILE branches.

### 5. Engine scenario evidence

Capture paired PC HW/GS scenes with identical camera/settings for: title/HUD,
opaque/transparent walls, masked sprites, FOF stacks, slopes, sky/skybox, each
blend including subtract/multiply/fog, models, underwater/heat/water, continuous
and tinted wipes, screenshots/capture slot freshness, and repeated mode switches.
Record absent features and graphics loss explicitly. Measure COP0 phases and
displayed frames separately from submitted frames. The user requirement remains
full PC HW functionality without loss; an experimental label is not its approval.

## Suggested deviation IDs (coordinator to reserve/edit)

No deviation registry or G1 verdict was edited by this worker. These proposed
IDs align with existing inline PS2-HW comments; verify collisions before use.

| Suggested ID | Description and status to register |
|---|---|
| PS2-HW-01 | Optional native GS hwdriver, experimental; software default/reference retained |
| PS2-HW-02 | Hardware menu label and renderer switching/GS ownership; engine validation pending |
| PS2-HW-03 | CT16S/RGB5551 precision, opaque RGBA index-255 remap, size-limit decimation: known loss, not approved final behaviour |
| PS2-HW-04 | Ordered GIF ring, frame copy/clear and FINISH/vblank presentation; readback synchronization |
| PS2-HW-05 | P8/AP88 hardware cache formats and CLUT; partial AP88 CT32 path; cache equivalence pending |
| PS2-HW-06 | Smaller growable engine batching arrays and explicit allocation failure; scene peak RAM pending |
| PS2-HW-07 | Subtractive/environment/multiplicative/fog approximations: missing exact PC equations |
| PS2-HW-08 | Threshold ALPHA8 wipe: missing continuous/tinted PC wipe |
| PS2-HW-09 | Shader/palette lookup/light-table/screen palette callbacks unsupported; explicit capability refusal/diagnostics |
| PS2-HW-10 | CPU float/tiny model interpolation, transform/UV/cull; normal lighting and real models unverified |
| PS2-HW-11 | Homogeneous clipping/index cache collision fix; independent host and GS fixtures, not engine parity |
| PS2-HW-12 | Sky staging/lifetime and transform restoration; real sky colours/caps unverified |
| PS2-HW-13 | Pinned half-size screen slots and selected-slot readback; resource/loss/aspect gaps |
| PS2-HW-14 | Filter approximations (transparent nearest, no mip chains/anisotropy), wireframe/corona gaps |

## Reproduction

```powershell
python -B tools/ps2/hw_hosttest.py --negative-controls --out build/continue-hw-host-verified
$env:SRB2_PS2_OUT='D:/Ai-Project3/SRB2-PS2-Port/build/continue-hw-verified16'
python -B tools/ps2/run_hw_test.py --tag full --timeout 900 -- soak=600 dump
$env:SRB2_PS2_OUT='D:/Ai-Project3/SRB2-PS2-Port/build/continue-hw-verified32'
python -B tools/ps2/run_hw_test.py --tag full --timeout 900 -- fb32 soak=600 dump
```

Use new output directories for new runs. `run_hw_test.py` invokes
`run_pcsx2.py`, which holds the machine-wide PCSX2 lock; wait for the coordinator's
turn normally. `host:` maps to the ELF directory. No direct emulator invocation,
toolchain changes, asset recooking or golden edits are part of this procedure.
