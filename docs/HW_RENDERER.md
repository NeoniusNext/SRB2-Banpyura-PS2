# PS2 GS hardware renderer — experimental

Status on 2026-10-02: **a working GS driver, not full PC HW parity**. The
requirement is a fast renderer with all PC HW features and no graphics loss.
The implementation below does **not** yet satisfy that requirement. The
primitive tests do not approve reduced precision, downsampling, missing shaders
or approximate blending as permanent deviations. Software remains the default
and the established 1:1 reference under PLAN §0a. G1 is not closed by this work.

Current evidence and commands: [renderer progress](PS2_RENDERER_PROGRESS.md).
Historical evidence: [continuation report](GATES/g1/continuation-hw.md).
Engine integration: [HW_INTEGRATION.md](HW_INTEGRATION.md).

## Implementation

`src/ps2/hw/ps2_hwd.c` fills the PS2 `struct hwdriver_s`. Its translation unit
includes `ps2_hw_priv.inc`, `ps2_hw_regs.inc`, `ps2_hw_gs.inc`,
`ps2_hw_xform.inc`, `ps2_hw_light.inc`, `ps2_hw_tex.inc`, `ps2_hw_draw.inc`,
`ps2_hw_model.inc`, `ps2_hw_screen.inc`.
The null driver is a separate **logging-only** integration aid.

* EE float matrices reproduce the PC fixed-function camera/object transforms.
  Convex fans and indexed triangle lists are clipped in homogeneous coordinates
  against near/far and a GS-safe guard band, then packed as ST/Q, RGBAQ, XYZ2.
  The GS scissor enforces the viewport. Z24 is reversed: larger values are nearer.
* Indexed vertices are cached, but each triangle snapshots its three cache
  entries before another index can alias the same slot. Lists close their GIF
  tag before a ring flush; packets never straddle producer buffers.
* Float model frames use expanded triangles; tiny frames use 16-bit indices
  and the PC 1/64 scale. CPU frame interpolation, object transforms, pivot roll,
  vertical/horizontal flip and front-face selection feed bounded 192-vertex
  staging. The camera matrix is restored. No normal-lighting implementation yet.
* Palette textures use PSMT8 and CT32 CSM1 CLUTs with the required index-bit
  3/4 swap. Four fixed CLUT images hold opaque, keyed, alpha-ramp and white-keyed
  palettes. Non-keyed AP88 with partial alpha or opaque index 255 uses CT32,
  preserving RGB and the GS 0..128 alpha scale instead of thresholding alpha.
  Arbitrary opaque RGBA also uses CT32. Opaque palette index 255 is retained;
  mixed opaque-255/transparent RGBA falls back to CT32. Alpha-zero RGB is preserved
  too: RGBA/AP88 holes use CT32 if a keyed CLUT would change their filtered colour.
  No automatic texture
  decimation remains. NPOT repeat polygons are cut into bounded chunks, including
  >40 repeats per axis; the staging limit no longer truncates remaining periods.
* VRAM allocation uses sorted, merged block ranges and LRU texture records.
  The range table can represent the worst-case alternating-block layout; the
  former 256-entry table silently lost ranges. Ordinary texture eviction clears
  the owner's `downloaded` handle. Current-frame textures are preferred over
  older ones; an ordinary texture can still be recycled within the frame because
  uploads/draws are ordered in one GIF stream. Captures spill losslessly to aligned
  EE RAM only after successful synchronized readback. GS restoration uploads the
  original pixels, without palette/alpha conversion. Impossible allocations fail
  before evicting the working set. Texture budgets include GS padding; capture
  backing has a separate bounded EE budget.
* A frame starts with the previous frame's local-to-local copy unless a complete
  colour clear makes it unnecessary. Vblank exposes only a frame-end FINISH.
  Starting a frame cancels the old pending flip; frame-end clears any FINISH
  left by a mid-frame readback. ReadScreenTexture reads the requested stored slot
  without modifying the current frame.
* Sky staging grows transactionally, is freed at Shutdown, and dome transforms
  are restored. Large valid loops are no longer silently omitted at 1024 indices.
* PACKED ST only latches Q in GIF: perspective vertices explicitly commit it with
  PACKED RGBAQ before XYZ. Untextured primitives emit flat colour once; Q=1 affine
  fans retain compact ST/XYZ packets. Packed colour/Q writes invalidate the A+D
  cache so subsequent affine draws cannot inherit stale Q or sky colours.
* Full-resolution captures and identity screen draws use exact local GS copies,
  avoiding rasterization and bilinear interpolation rounding. The implicit final
  path never clears its source; letterboxing saves it before clearing. RGB readback
  uses top-down bilinear pixel-centre sampling without altering the framebuffer.
* Explicit built-in lighting uses actual depth-staircase cuts and GS fog for
  ordinary solid/translucent surfaces. Lit fog retains experimental depth ramps.
  These passes are not GLSL: InitShaders/CompileShader refuse shader capability,
  preserving engine fallbacks. Water ripple is diagnosed as missing.

## OPT9-HF (2026-10-06): shader capability, light, water, diagnostics

Facts from runs in the emulator (details and pictures: `docs/GATES/g1/opt9-HF.md`):

* The driver now **advertises the base shaders** (`InitShaders` true unless `-hwdbg 2048`, `CompileShader` true for the built-in slots, custom GLSL still refused with one
  log line). Before, the engine saw `gl_shadersavailable = false` and used its flat per-polygon fallback colour; every GS light path (depth bands, ramp, CLUT tint,
  fog blocks, water) had never run inside the engine. `HWR_ShouldUsePaletteRendering()` is `false` on PS2 (no 3D palette lookup/light tables on the GS).
* Sector light = the GLSL equation `mix(colour, fade, floor(R_DoomColormap)/32)` reproduced exactly: polygons are cut at the eye depths where the darkness class steps
  (GS fog with a constant F per piece; FOGCOL = fade colour); colormap tint = a CLUT per tint (`CK_TINT`). Polygons that stay inside one class are not cut (THZ1: 2 of
  ~1800 polygons per frame are cut).
* Water (`PF_Ripple`, shader 4): the shader's `tex(s - sin(a)*0.025, t - cos(a)*0.025)` with `a = -pi*z/2*0.025 + leveltime*2` (**leveltime there is in SECONDS: (leveltime - 1 + rendertimefrac) / 35; this section and OPT10 turned 2 rad per tic, 35 times too fast, corrected in OPT11-FX PS2-HW-125; the band method described here was replaced by PS2-HW-120**) is made by cutting the polygon into
  16-unit depth bands (to 640 units) with the texture coordinate shifted by the middle of the band (error <= 0.5 texel on a 64 texel flat).
* A polygon whose texture coordinates span more than 4096 texels (huge floors/horizons) is cut at whole repeats (`UV_EXTENT`): the GS UV integer part is 14 bits.
* Diagnostics of the driver go to the log only (`CONS_Printf`/`CONS_Alert` are redefined to `I_OutputMsg` inside `ps2_hwd.c`): they were drawn over the picture.
* `-hwdbg` bits: 2048 engine fallback lighting (A/B), 4096 light parameters of frame 150 (`HWT lit`). `HWFX frame N: ...` lists the effects drawn in the last 300 frames.
* `-vidshot kN` / `KN` (first frame with `leveltime >= N`, `K` only in a level started after the previous shot), `wN` (N-th frame of a wipe), keys `console`, `f1`, `f2` for `-vidkeys`.

## OPT10-HF (2026-10-07): palette rendering, texture conformance test, measurement tools

Facts from runs in the emulator (details, pictures and numbers: `docs/GATES/g1/opt10-HF.md`):

* The PC OpenGL driver of SRB2 renders by default with **palette rendering** (`gr_paletterendering On`): colour = `lighttable[texel index][row]`, row = `clamp(floor(R_DoomColormap), 0, 31)`, the screen palette
  (flash, 565 crush) at the end. This is what the software renderer draws. The GLSL equation of the non palette path (`mix(colour, fade, darkness)`, OPT9) is another picture (hue shifts of the dark colormap
  rows, tinted colormaps, the DSZ1 water: mean error 14 against 5 with the palette path switched off on the PC). **PS2-HW-71:** `HWR_ShouldUsePaletteRendering()` is true on the PS2 as on the PC; the GS does the lookup with
  CLUTs (`ps2_hw_pal.inc`): `CreateLightTable`/`UpdateLightTable` keep the 32 x 256 table as palette indices, a lit polygon with a table id and a PSMT8 texture is cut where the row changes (the staircase of
  `ps2_hw_light.inc`, classes 0 and 1 are one row) and each piece is drawn with the CLUT of its row (`CK_LIGHT`: row of the table through the screen palette); `SetScreenPalette` is applied when a CLUT is built
  (flashes and the 565 look without a post process, the engine's final screen texture is the frame itself); direct colour textures, flat colours and fog blocks keep the GLSL equations. A polygon whose table id is not
  known (renderer switch before the engine rebuilt its tables) falls back to the GLSL equations. Result against the PC picture, same frame, 320x200 framebuffer: MAD 0.7-2.3 on seven maps (3.4 on the DSZ1 water).
* The 1:1 texture check `-hwtextest` (**PS2-HW-69**, `ps2_hw_tt.inc`, needs `-hwfbh 200`): every map texture (both chroma key variants) and level flat is made resident like the engine does, drawn 1:1 with a GS sprite and repeated through
  the wall/floor polygon path, read back and compared with the texels the engine handed over. `tools/ps2/hftt.py` runs it on a list of maps (`TTFAIL` lines name the texture and the first wrong pixel).
* UV of a GS sprite: the register holds 14 bits (1/16 texel), so 1024.0 wraps to 0 (**PS2-HW-70**: clamped to 1023.9375 in `sprite_tex`); the GS samples `U0 + n` at pixel `n` (no half texel), so a sub-texel start moves the picture by a texel.
* `-hwfbh N` (**PS2-HW-68**): internal frame buffer height; 200 = one pixel per engine pixel (the default 224 stretches 200 rows and the `-vidshot` readback resamples them bilinearly: soft HUD edges in pictures).
* `-vidshot iN` (N-th intermission frame), `-vidcmd 'cmd~arg;cmd2'` (console commands on the third frame); PC side `-ps2ref-shot/-ps2ref-keys/-ps2ref-cmd` (src/ps2ref.c) take the engine's own OpenGL screenshot of the same frame.
* Textures taller or wider than 1024 (THZ pipes `THPIP*` 128x1536) are stored decimated by 2 (`dx/dy`), the one remaining quality loss of the texture path (`TTDECIM` lines).
* **GIF stream validator (PS2-HW-74, `ps2_hw_val.inc`, `-hwdbg 536870912`, bit 29 of `ps2hwd_dbg_flags`)**: every ring buffer is parsed as the GIF and the GS parse it, in `pk_flush` before it is queued, and checked against the
  rules a real GS keeps and PCSX2 forgives: DMA tags and 16-byte aligned REF addresses, register field widths (TEX0 TW/TH/TBW/CLD, CLAMP, SCISSOR, FRAME, ZBUF, ALPHA, TEST.ZTE, BITBLTBUF, TRXPOS, TRXREG), IMAGE size
  against TRXREG, no register write inside a transfer, VRAM bounds by the exact GS block tables, **TEXFLUSH after local memory was written under a texture** (the real GS texture cache; PCSX2 has none), vertices inside the
  +-2000 pixel guard band, Z within the Z buffer format, Q > 0, texel span of one primitive <= UV_EXTENT. The output is `HWVAL <class> f=<frame>: ...` (three examples per class) and `HWVAL SUMMARY`. Not checked: the VU1 path
  (`-hwdbg 0x4000000`). Host test of the validator itself: `tools/ps2/hf_valtest.c` (4 clean streams, 27 seeded faults). The `-hwdbg` bits in use: 1..16384 (see above), 0x100000..0x2000000 HT, 0x4000000 VU1, 0x8000000 HWDBG_LODDBG,
  0x10000000 HWDBG_IMMDBG, **0x20000000 the validator**; a value that is a bit of another feature changes the run (the validator first took 0x8000000 and printed the plan of every frame).
* Memory (S/HT find): **PS2-HW-76 / PS2-146 (S)** the patches of the wall textures are not kept after the composition (`hw_cache.c`; they were pinned as `PU_PATCH` for the session: 4.9 MB after 7 level changes; the merge kept S's `loaded_here` version of the same fix);
  **PS2-HW-79** the batch arrays (`PU_HWRBATCH`) go back to the zone at every level change (`HWR_ReleaseBatching`; 1.45 MB after THZ2/ACZ1 against 0.42 MB). With both, the 50 map change chain (7 light maps) passes in the HW renderer.
  `-zreserve 3072` (the C heap kept outside the zone) costs the zone 1.1 MB against 1536: the maps GFZ2, THZ2, ACZ1, ERZ1, MAP08, MAP40 do not load with 3072 and load with 1536; MAP10, 11, 14, 23 do not fit even with the
  engine default of 512.
* Measurement tools of OPT10-HF: `pcshot.py` (PC reference), `hf_run.py`, `hfpanel.py`, `hfbatch.py` (`--zreserve`, `--emu`), `hfscreens.py`, `hftt.py`, `hf_perfcmp.py` (HWPROF windows of two runs), `hf_zcaller.py` (`-zcaller` log by tag and
  caller), `hf_chaincmp.py` (every picture of a map change chain has a twin), `hf_hudaddon.py` (Lua HUD test add-on), console command `hf_split 1` (splitscreen in single player, PC and PS2).

## OPT11-FX (2026-10-08): water, shadows, model light, effect matrix

Facts from runs in the emulator against the PC OpenGL engine (details, numbers, commands: `docs/GATES/g1/opt11-FX.md`):

* **Water (`PF_Ripple`) shader time** (PS2-HW-125): the PC driver hands the shader `(leveltime - 1 + rendertimefrac) / TICRATE` seconds and the shader turns 2 rad per second (2/35 rad per tic); the GS driver (OPT9 on) turned 2 rad **per tic**, 35 times too fast: the "glitching water". Now `a0 = 2 * (leveltime - 1 + frac) / 35` (`water_phase_rad`). Water blocks against the PC picture on a grid texture: 30-72 % at (0,0) -> 80-99 %.
* **Water without seams** (PS2-HW-120): the shift of the texture coordinates is computed in the vertices by eye depth and interpolated by the GS (C0 at every depth plane and at polygon borders), one triangle strip per light class, reach by texture size (64: 320 units, 256: 1280, 512: 2560), fade between 1.5 and 0.8 pixel of amplitude; polygons over 4096 texels ripple too. Host test `ripple`: mean error 0.36 px against the GLSL (OPT10 bands: 1.4-3.7 px). A/B switch `-hwwater 1` (the OPT10 sweep); cost: DEMO_001 +5.0 % of the water branch (+0.4 % wall), DEMO_004 -39 % of the water branch (-1.0 % wall).
* **Drop shadow lost the depth test on 1/3 of the maps** (PS2-HW-124): the GS interpolates the depth of a floor from vertices snapped to 1/16 pixel, a low camera sees hundreds of depth steps per pixel; the shadow 0.05 units over the floor lost. `HWR_DrawDropShadow` lifts the shadow by `d^2 / (640 h)` units (0.05 .. 6) under `PS2_PROFILE`.
* **Models** (PS2-HW-126) were drawn at full brightness (`PF_ColorMapped` is not in the surface flags of `hw_md2.c`): sector light, colormaps and `RF_FULLDARK` now work. `gr_modellighting` (directional light, off by default) is not implemented.
* **Stale light-table CLUTs** after a level restart (PS2-HW-123): `lt_clear` bumps `H.pal_gen`.
* Known differences that stay (matrix in the report): the PC snaps every blended colour to the palette (a post process the GS cannot do: MAD 2-6 in scene pictures); UI colormap fade is a translucent black quad (menu MAD 17); `PF_Decal` bias is a constant 3 depth steps; `gr_lightdithering`, wireframe, corona, custom GLSL are not implemented.
* Tools: `fx_pair.py` (one scene on both renderers), `fx_sweep.py`, `fx_shift.py` (block matching of the water), `fx_chain.py`, `fx_queue.sh`, `fx_water.sh`, `fx_watertab.py`, `fxscene.lua`/`fxflash.lua`, `make_fxmodel.py`, `-hwlt N`, `-hwwater N`.
* Trap: the PC engine reads and **saves** `/opt/srb2-assets/reference.cfg`: a run that sets `gamma 4` or `gr_models On` leaves it for the next PC runs of everybody (`pcshot.py` now resets them at the start of every run).

## OPT11 round 2, FX2 (2026-10-08): speed of the sprites, shadows, sky dome and sky box water on the EE

Details, numbers and what was not reached: `docs/GATES/g1/opt11-FX.md` section 6 (PS2-HW-240..249).

* **Sphere tests before the work** (`ps2_hw_fx2.inc` `PS2HWD_CullSetup`: the rows of the clip transform and the gradient lengths of the four side planes): a thing whose sprite cannot reach the view is not projected (`HWR_AddSprites`), a drop shadow and every quad of the sky dome are tested the same way. A sphere wholly outside one side of the view volume holds four corners outside it, which is what `PS2HWD_QuadHidden` calls hidden: the result is exact (check mode `-hwfx 2`: 0 differences on DEMO_001..004, 500 thousand things). Things with a drop shadow, models, link draws, floor and paper sprites, skins, overlays and rolled sprites are not tested by the sphere (their quad is still tested by `PS2HWD_QuadHidden` as before).
* **A/B switches on one ELF** (`-hwfx N`, `ps2_hw_fx2.h`; a set bit switches one path off): 1 thing filter, 2 check mode, 4 sort keys in the vissprite, 8 shadows, 16 sky dome, 64 interpolated state kept in the vissprite, 256 sky box water, 1024 thing visibility; `-hwfx 1373` = every path off. `-fxfrac N` draws every frame N percent between two tics (the time demo draws whole tics only).
* Result: DEMO_001 wall -2.1 %, DEMO_004 -4.0 % (same ELF, whole-tic frames, no profiler console); between two tics -3.7 % / -5.8 %. Pictures: 0 differing pixels in 40 snapshots (4 demos, whole tics and between tics, paths on / off). The goal of round 2 (0.8 M cycles for sprites + HUD + sky + water on DEMO_001, 2 M on DEMO_004) is not reached: 2.19 M and 3.64 M.
* Trap for A/B between two different ELFs: the pictures of two builds differ by up to 0.5 % of the pixels (1-2 pixel lines along wall edges) when the allocation pattern differs (the size of `gl_vissprite_t` alone does it): the batch order of polygons with one texture depends on a hash of the texture's pointer (`HWR_ProcessPolygon`). Compare pictures of one ELF.

## OPT11 round 2, GEOM2 (2026-10-08): the engine side of the walk - geometry cache of walls, determinism tools

Details, numbers, commands: `docs/GATES/g1/opt11-GEOM.md` (round 2, R2.1..R2.9; registry PS2-HW-200..215).

* **Geometry cache of the walls** (`hw_gcache.inc`, on by default): the polygons a seg makes (`HWR_ProcessSeg`) are recorded as the calls the function makes to the three sinks of the walk and made again when the key - plain words, the inputs of the function (sector versions, line and side words, texture numbers, ...) - is the same; exact, no hashing. Planes are cached with `-hwgc 1` (no gain after the block collection of PS2-HW-233: a hit costs about what the calculation does). 3D floors with an animated texture hit (the record is patched to the texture of the moment: PS2-HW-213). The arena (640 KB) doubles while it is full and the zone has room; the zone can take it back (`Z_AddReclaimHook`).
* **Determinism tools**: `-hwpolyhash` (HWPH line: `h=` all polygons, `w=` the polygons of the world only, `s=` heights/lights/flats of all sectors, `v=` the view), `tools/ps2/gm_polycmp.py A B [--world]`; `-singletics` on a map makes one tic for every frame drawn (PS2-HW-208: two runs of a map show the same frames); `-hwgc 2` calculates every hit again and compares; `build/mapcamp3.sh` (15 maps against the reference stream) in the worktree.
* **A/B switches** (`-hwgo` bits): 1 AddLine angle reuse, 4 QuadHidden two-corner exit, 8 HWR_Lighting fast path, 16 wall light memo, 32 shader table per batch, 1024 inline R_FakeFlat, 2048 light table once per view, 4096 vertex angle once per view, 8388608 no growth of the cache arena, 16384 BSP walk without walls and planes (a measurement), 32768 flat of a plane chosen at once; `-hwgc N`, `-hwgcmem KB`.
* Traps: (1) memory: the cache takes 0.8 MB (a big level: 1 MB); the level of DEMO_002 (26203 segs) is at the edge of the zone and cliffs (textures purged and read again from the PAK: 6..20 M cycles a frame in a few windows) for some sizes of any long-lived block, with the cache off as well as on - a zone/STAB matter. (2) A run of a map is only comparable with `-singletics` (frame = tic) and without screen shake (MAP12).

## Complete callback matrix

`P` = implementation exercised by standalone primitive/readback tests;
`H` = pure logic tested on x86/x64; `U` = engine scenario unverified.
Every row remains **U for full engine/PC comparison**, including rows marked P.

| `hwdriver_s` callback | GS implementation / result | Evidence or outstanding gap |
|---|---|---|
| Init | gsKit CRTC setup, own FB/Z/pool and GIF ring | P: NTSC CT16S and CT32, two Init/Shutdown cycles; PAL/480p unverified here |
| SetTexturePalette | opaque/keyed CLUT update, baked direct-colour invalidation | P: 256 cells, palette change causes 0 indexed reuploads |
| FinishUpdate | FINISH, vblank flip, optional pacing | P: 30/30 paced flips; unpaced frames can be replaced before display |
| Draw2DLine | screen-space one-pixel quad, colour/alpha | Implemented; automap/line fixture not verified |
| DrawPolygon | convex triangle fan, transform/clip/state | P/H: edges, transform, Z, texture/blend fixtures |
| DrawIndexedTriangles | cached transform, triangle-list packet runs | P/H: equals fans; colliding indices 0/256/512 verified |
| RenderSkyDome | colour vertices, fan/strip loops, bounded allocation, restored view | P: dome draws and repeat is identical; cap colours/real engine sky unverified |
| SetBlend | stores flags consumed by drawing | P/H for basic modes; see equations below |
| ClearBuffer | independent colour/depth write masks, full-target clear | P: every colour/Z pixel; viewport-specific engine behaviour unverified |
| SetTexture | existing record or converted, banded upload | P/H: indexed/keyed/CT32 exact shapes and LRU; CT16 used only for explicit framebuffer/capture mode |
| UpdateTexture | discard record and reupload | Implemented; engine update/lifetime fixture outstanding |
| DeleteTexture | free record and clear owner handle | Used by P fixtures; engine owner destruction ordering outstanding |
| ReadScreenTexture | selected GS/EE slot -> top-down bilinear RGB888 | P/H: selected slots and unchanged FB; absent GENERIC2 reads completed FB; GENERIC3 requires an explicit capture |
| GClipRect | viewport/scissor and 2D near projection | P/H: inset/edge/full viewports and clipping; engine splitscreen unverified |
| ClearMipMapCache | invalidate ordinary records, preserve screen slots | Implemented; cache/mode switch lifetime outstanding |
| SetSpecialState | nearest/bilinear; diagnostics for other requested states | H/P basic filter; models lighting, anisotropy, wireframe absent; mixed/mipmap filters approximated |
| DrawModel | CPU float/tiny interpolation, object transform, UVs, culling | P: 8 fixture cases, 0 pixel differences; H: 16 cases including roll/restoration/culling. Real MD2/MD3 loading/skins unverified |
| CreateModelVBOs | CPU path uses current mesh UVs; no GS VBO required | No geometry discarded; records current max UV metadata; hardware VBO IDs unused |
| SetTransform | float view/projection, flips/mirror/shear/roll code | P/H basic camera/roll; full engine combinations outstanding |
| GetTextureUsed | allocated pool blocks × 256, includes captures | P: reported pool usage; not total EE/GS consumption |
| Shutdown | drain DMA/GS, remove handlers/semaphores, clear handles, free staging | P: second-cycle heap returns to baseline; requires live owners when handles cleared |
| PostImgRedraw | textured 10×10 grid, black behind displaced edges | Implemented; actual underwater/heat grid not verified |
| FlushScreenTextures | release resident slots and EE backing | P: capture pressure allocations return to zero; engine lifecycle outstanding |
| DoScreenWipe | CT32 destination-alpha mask blend; tinted per-channel CLUT/add/subtract | P: 64 continuous mask levels and four tinted directions within 2 RGB levels. CT16S one-bit destination alpha is explicitly lossy |
| DrawScreenTexture | full-screen slot draw, clear, surface flags | P basic roundtrip; tint/fog/shader differences remain |
| MakeScreenTexture | GS framebuffer -> full-resolution copy, recoverable EE backing | P: all five slots under 3-MiB texture pressure; one complete capture still must fit the pool |
| DrawScreenFinalTexture | aspect/letterboxing; exact identity copy or zero-copy implicit final | P/H: picture retained, black bars, one-pixel stripes unchanged |
| InitShaders | returns false | Honest capability refusal; programmable shader path unavailable |
| LoadShader | warns for all GLSL sources, no executable created | Fixed GS pass subset does not establish built-in/custom shader equivalence |
| CompileShader | warning, returns false | Missing |
| SetShader | stores requested slot; custom slots warn | Explicit experimental built-in passes; capability refusal keeps engine fallback routing |
| UnSetShader | fixed-function state already active | No shader allocation to release |
| SetShaderInfo | stores LEVELTIME | The water ripple is made from it (OPT11-FX, PS2-HW-120, `ps2_hw_water.inc`) |
| SetPaletteLookup | no-op (PS2-HW-71): the textures are indexed, no RGB-to-index lookup is needed | OPT10: palette rendering is done with CLUTs, see the OPT10-HF section |
| CreateLightTable | keeps the 32 x 256 colours as palette indices (8 KB), returns an id (PS2-HW-71) | rows of the table are CLUT images (`CK_LIGHT`) |
| UpdateLightTable | rebuilds the indices, new CLUT generation | as CreateLightTable |
| ClearLightTables | frees the tables | engine calls it at level free |
| SetScreenPalette | stores the (flash / 565 crushed) screen palette; the CLUTs of palette textures are built from it | flashes and fades without a post process; flat colours and direct colour textures do not follow a flash |
| GetModeList (`_WINDOWS` only) | not a member of the PS2 build's struct | Platform-specific PC callback; PS2 mode enumeration belongs to i_video |

## PC HW capability matrix

This is a feature inventory, not a claim that engine geometry reaches every
implemented primitive path correctly.

| PC HW capability | Current GS coverage | Required validation / implementation |
|---|---|---|
| Solid/masked/translucent walls | Textured fan/list primitives, Z24, alpha test | Real BSP/wall joins, texture pegging, ordering and overdraw |
| Floors/ceilings, FOFs | Engine plane polygons can use same primitive path | FOF side/top/bottom, translucent stacks, horizon, fog-block scenarios |
| Slopes | Arbitrary 3D vertices and perspective texture coordinates | Slope seams, stacked slopes, near-plane/intersection cases in engine |
| Sprites/rotated/flipped sprites, HUD patches | Palette/AP88 and polygon paths | Camera-facing geometry, sprite clipping/translucency/colormaps, HUD scaling |
| Sky dome/skybox | Dome path and ordinary polygon path | Real cap/strip textures, skybox viewpoint and depth handling |
| Sector lighting/tint/fade | CPU engine fallback; explicit GS depth bands/fog and palette tint CLUTs | P: seven banded fixtures within 2 RGB levels; direct-colour/modulated tint and full engine references outstanding |
| Fog | Flat four-pass SRC_ALPHA/SRC_COLOR blend; explicit lit fog ramps | P flat blend; sampled lit fog and full fade/tint equivalence outstanding |
| Models | Geometry/interpolation/culling implemented | Normal/directional lighting, all skin/material and frame layouts in engine |
| Dynamic/static lights/coronas | ALAM lighting is not enabled; corona draw rejects with warning | Resolve PC optional feature configuration, depth visibility and light geometry |
| Translucent/additive/reverse-subtract blending | Native GS equations | Tested independently, GS rounding differs from PC; textured/partial-alpha coverage needs expansion |
| Subtractive/multiplicative/environment blending | Native source-minus-destination after polygon alpha scaling; palette channel multiply; two-pass environment | P flat/opaque palette within 2 RGB levels. Partial-alpha subtract, direct-colour multiply, keyed multiply/depth and lit combinations remain gaps |
| Alpha/keyed holes | Keyed index 255, CT32 AP88, masked source-alpha scaling | GS alpha quantization/general partial-alpha threshold parity remain gaps; polygon alpha 128 correctly passes >0.5 |
| Water shader/ripple | PF_Ripple: texture shift in the vertices by eye depth (C0, no seams, reach by texture size), one triangle strip per light class (OPT11-FX) | P/H: host test `ripple` mean error 0.37 px against the GLSL, PS2/PC scene pairs (`docs/GATES/g1/opt11-FX.md`); the shader has no refraction of what lies below |
| Underwater/heat screen distortion | CPU grid redraw (`PostImgRedraw`) | Compared with the PC in water pits of GFZ1/GFZ2 (OPT11-FX): same wobble and colours (the wobble moves a column by at most 0.9 px); the underlay of the grid is black here, the PC code draws it with `white` and whatever texture state is current: edges only, not measured |
| Built-in/custom shaders | Unsupported, Init/Compile false | Fixed-function or multipass equivalents for built-ins; custom programmable code has no interpreter |
| Palette rendering/light LUT/colormap postprocessing | Unsupported shader callbacks warn | Complete colour lookup and light-table effects |
| Near/far/frustum/scissor clipping | CPU homogeneous clip, GS guard band/scissor | H/P independent reference; full map occlusion and splitscreen not verified |
| Framebuffer/depth readback | Aligned local-to-host transfer, bounded synchronization | P NTSC CT16S/CT32; real cache, error injection and physical hardware outstanding |
| Capture/screenshot | Selected GS/EE slot RGB888; explicit boolean status API | Engine GENERIC2/3 freshness, callback failure routing and exact PC readback references |
| Screen textures/presentation | Full-size captures, GS/EE residency, exact identity copies, aspect/letterbox | P five default-mode slots; larger-than-pool high-resolution captures need tiled storage/compositing |
| Wipes (normal/tinted/continuous) | CT32 continuous destination-alpha and per-channel tinted compositions | Engine flags/masks/reference frames outstanding; CT16S cannot preserve continuous mask alpha |
| Texture filtering/wrap/mipmaps/anisotropy | Repeat/clamp, nearest/bilinear; TF_TRANSPARENT nearest matches PC driver policy | Mip chains, mixed LOD, anisotropy and shader-filtered alpha reference frames outstanding |
| Texture colour/resolution | PSMT8 exact palette indices or CT32 RGB; exact supported dimensions | No RGB555 conversion/remap/decimation. >1024 axes, larger-than-pool textures and insufficient budgets fail explicitly; lossless tiling required |
| Wireframe | Not implemented; diagnostic | Line/edge emission and state semantics |

### Blend equations

`Cs`, `Cd`, `As` are source RGB, destination RGB, source alpha. GS divides
alpha by 128; PC uses normalized alpha. Clamping and rounding differ.

| Mode | PC intent | GS now |
|---|---|---|
| Ordinary opaque | Cs | Cs |
| Masked | Cs×As, alpha > 0.5 | Source-alpha scaling folded into modulation where valid; polygon alpha 128 boundary corrected; general partial-alpha quantization remains |
| Translucent | Cs×As + Cd×(1−As) | `(Cs−Cd)×As + Cd` |
| Additive | Cs×As + Cd | Same equation |
| Reverse subtract | Cd − Cs×As | Same equation |
| Subtractive | Cs×As − Cd | Pre-scale polygon colour, native Cs−Cd; partial texel alpha factor missing |
| Environment | Cs + Cd×(1−As) | Destination attenuation followed by source addition |
| Multiplicative | Cs×Cd | Palette per-channel CLUT factors, quantized once after modulation; direct-colour fallback warns |
| Fog block | Cs×As + Cd×Cs | Three destination-channel scale passes plus source addition; lit ramp experimental |

GS ALPHA exposes one **scalar** factor As/Ad/FIX, not a per-channel destination
colour factor. Palette multiply and flat fog therefore require channel passes.
General direct-colour multiply and source-minus-destination with partial texel
alpha still need compositing. Primitive checks use stated GS rounding tolerances,
not bit-identical PC framebuffer output.

## Real memory budgets

All sizes below include GS page padding. GS VRAM is exactly **4,194,304 bytes**.
FB CT16S pages are 64×64; CT32 and Z24 allocation pages are 64×32. Z24 consumes
a 32-bit-layout allocation. The four fixed and 120 dynamic CLUT images reserve
16 pages (**131072 bytes**). Internal size is selected from engine resolution
and CRTC magnification; default 320×200 engine coordinates use **320×224 NTSC**.
CT32 is the default. CT16S is explicitly lossy opt-in. No automatic CT32-to-CT16S
fallback remains; a layout that cannot fit returns Init failure.

| Mode | Each CT16S FB | Z24 | CT16S texture pool | Each CT32 FB | CT32 texture pool |
|---|---:|---:|---:|---:|---:|
| NTSC 320×224 (current measured default) | 163840 | 286720 | 3448832 (13472 blocks) | 286720 | 3203072 (12512 blocks) |
| NTSC 640×448 (calculated current layout) | 573440 | 1146880 | 1769472 | 1146880 | 622592 |
| PAL 640×512 (calculated, not run here) | 655360 | 1310720 | 1441792 | 1310720 | 131072 |
| 480p 640×480 (calculated, not run here) | 655360 | 1228800 | 1523712 | 1228800 | 376832 |

Two FBs + Z + 131072 + pool = 4194304. Default captures are full 320×224,
not decimated 640×448 images. Each GS capture occupies one FB-sized allocation.
All five CT32 slots fit by themselves, and can spill to EE RAM under texture
pressure: measured peak backing **1433600 bytes**, with five spills and five
restores. CT16S backing stores visible raw rows (not GS page padding): measured
peak **716800 bytes**. Resident captures may retain backing until overwritten
or flushed, avoiding repeated readback on subsequent evictions.

`screen_max_bytes=0` selects an 8-MiB EE backing cap; a positive value sets a hard
budget. Readback/allocation failure does not authorize capture eviction. Ordinary
textures are evicted first; a currently needed wipe mask/ramp/selected texture
cannot be sacrificed to make a capture appear successful. `tex_max_bytes=0` uses
the pool limit; a positive limit counts allocated GS blocks, not source bytes.

At full 640-wide CT32 NTSC/PAL/480p, **one full-resolution capture exceeds the
pool**. EE spilling alone does not solve that single-object limit. Lossless tiled
capture/upload/compositing or a coordinated framebuffer/Z layout redesign is
still required for those modes. Large textures (>1024 per axis or larger than
pool) also fail explicitly rather than silently shrinking.

Measured EE allocations/state after the standalone workload:

| Component | Bytes | Scope |
|---|---:|---|
| Driver H state | 69392 | Includes worst-case free ranges, CLUT cache and capture pointers; not all translation-unit statics |
| GIF producer ring | 524288 | 4 × 131072, memalign(64) |
| Texture record capacity | 30720 | 512 EE records × 60 bytes; grows on demand |
| RGBA index conversion scratch | 64000 | High-water allocation in this workload; not a fixed upper bound |
| Sky staging | 840 | Tiny test dome; grows with engine dome |
| Frame readback temporary, default CT16S / CT32 | 143360 / 286720 | Plus caller's RGB/u32 destination; scales with internal stride/rows |
| Depth readback temporary, default | 286720 | Plus caller destination |
| Capture EE backing, default CT16S / CT32 high-water | 716800 / 1433600 | Pressure fixture; configurable cap, released at Flush/Shutdown |
| Model staging | 4608 | 192 × (20-byte vertex + 4-byte index), CPU-only static |
| Polygon clip buffers | 16320 | Two × 204 × 40-byte clip vertices, CPU-only static |
| Indexed transform cache | 13312 | 256 × 52-byte entries, CPU-only static |
| Cut staging | 79200 | 9 × 220 × 40-byte vertices; bounded regardless of repeat count |
| Overlay queue | 65536 | Experimental lit fog/ramp support; ordinary lighting now uses exact-boundary bands |

Other state/CLUT/hash arrays, gsKit 8-KiB queues, libc/stack, hardware-engine
poly pool, caches and models also consume EE RAM. `GetInfo` deliberately labels
H-state versus dynamic buffers; these numbers are **not** a total 32-MiB engine
peak measurement. The default standalone retains three 286720-byte readback
arrays (860160 bytes); its eviction fixture also intentionally retains 240
128×128 RGBA source textures (15728640 bytes). This is not engine ownership.

Initial engine batching uses 8192 final vertices, 24576 indices, 8192 unsorted
vertices, 2048 polygons and 2048 polygon indices: **434176 bytes +
2048×sizeof(PolygonArrayEntry)**. Arrays double without dropping polygons;
growth briefly retains the old and new buffers. PS2 allocation failures now
reach I_Error before memcpy through NULL. Map-scale high-water RAM and OOM
behaviour remain to be measured by the integrated build.

## DMA/cache and resource ownership

1. Ring base addresses are `memalign(64)`, GIF words aligned 16. Upload rows/CLUTs
   and CPU vertex staging are copied into these owned packets; engine pointers
   are never submitted by DMA reference. CPU-only scratch need not be DMA aligned.
2. `pk_flush` calls `SyncDCache(start,end)` on the written packet range before
   enqueueing the physical address. IRQ-protected queue transitions transfer
   ownership to GIF DMA; the next producer buffer is reused only after the FIFO
   completion count proves it is free.
3. DMAC completion means RAM packets can be reused, not that rasterization is
   finished. GS FINISH is required before framebuffer readback/reuse. Polling
   backs up the GIF interrupt path; loss of an interrupt cannot authorize reuse.
4. A DMA/FINISH timeout logs an error and stops rendering instead of returning
   a busy buffer or unresolved framebuffer to the producer. Fault-injection
   recovery is not implemented/tested. This is intentionally fail-stop.
5. ReadVram rejects NULL/unaligned destinations and unsupported formats/layouts.
   Local-to-host buffers are aligned 64 and synchronized before and after DMA.
   ReadFrame/ReadDepth use aligned temporaries; their caller output is CPU-only.
6. Engine GLMipmap owners must stay alive until DeleteTexture/ClearMipMapCache/
   Shutdown clears their handles. The GS stream is single-producer; software and
   HW drivers must not own GIF/CRTC handlers concurrently.

PCSX2 success does not validate physical PS2 cache coherence, GIF stalls or
EE cache-miss timings. No toolchain/assets/golden changes are needed by this driver.

## Diagnostics and completion criteria

Unsupported effects, missing texture/capture data and quality approximations
increment `unsupported_calls` / `unsupported_mask` and issue a once-per-category
warning. Test runs with a nonzero limitation mask are **not parity successes**.
Current result counts and masks are recorded in `PS2_RENDERER_PROGRESS.md`.
A zero mask means only that this workload did not request a diagnosed limitation;
it does not cover absent shaders, other blend/alpha combinations or engine scenes.

Completion requires engine scenarios for walls/FOFs/slopes/sprites/sky,
colormaps/fog/blends, models, water/heat, every wipe/capture slot and mode switches;
paired PC HW reference frames using the same camera/settings; all unacceptable
matrix gaps implemented; actual 32-MiB peak accounting; and engine-scene timing.
An integrated boot or a colourful primitive dump is not that evidence.
