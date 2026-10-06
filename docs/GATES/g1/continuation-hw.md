# HW continuation — standalone evidence, 2026-10-02

**Result: experimental GS implementation improved; full PC HW parity and engine
scenario validation remain incomplete.** This does not change G1 or open G2.
Full callback/capability matrix and budgets: [HW_RENDERER.md](../../HW_RENDERER.md).
Coordinator handoff and proposed PS2-HW IDs: [HW_INTEGRATION.md](../../HW_INTEGRATION.md).

## Work completed

Inspected AGENT_BRIEF, PLAN §0a, G1, all existing driver `.inc` files, standalone
tests, hardware-engine changes and coordinator-owned integration source.

Implemented CPU model rendering (previously empty): float/tiny frames, interpolation,
UVs, object scale/rotations/pivot roll/flips, culling, bounded triangle batches,
camera restoration. Added host and GS independent triangle comparisons.

Fixed indexed transform-cache aliasing; transactional sky staging/free/restore;
AP88 partial-alpha/opaque-index-255 conversion; selected-slot readback; pinned
screen-capture eviction policy; lossless free-range accounting; NULL/allocation
checks; GS frame FINISH/pending-flip ownership; range-specific SyncDCache before
GIF DMA; fail-stop ownership timeouts; and PS2 batch malloc checks.

Unsupported/approximate paths now warn and count limitation bits. Init prints
EXPERIMENTAL. Build/test wrappers save command and SHA256 metadata, reject link
diagnostics/inconsistent completion markers, and host --out resolves relative
paths correctly. Only assigned source/test files and these three new docs changed.

## Actual commands and results

Parent `build` was verified with Test-Path before generating unique output
directories. All emulator runs used `run_hw_test.py` -> `run_pcsx2.py` shared lock.

```powershell
python -B tools/ps2/hw_hosttest.py --negative-controls --out build/continue-hw-host-verified
$env:SRB2_PS2_OUT='D:/Ai-Project3/SRB2-PS2-Port/build/continue-hw-verified16'
python -B tools/ps2/run_hw_test.py --tag full --timeout 900 -- soak=600 dump
$env:SRB2_PS2_OUT='D:/Ai-Project3/SRB2-PS2-Port/build/continue-hw-verified32'
python -B tools/ps2/run_hw_test.py --tag full --timeout 900 -- fb32 soak=600 dump
$env:SRB2_PS2_OUT='D:/Ai-Project3/SRB2-PS2-Port/build/continue-hw-syntax'
$env:SRB2_PS2_HW='1'
python -B tools/ps2/build.py --syntax src/hardware/hw_batching.c src/hardware/hw_clip.c src/hardware/hw_light.c src/hardware/hw_main.c src/ps2/hw/ps2_hwd.c
```

| Check | Actual result | Evidence |
|---|---|---|
| Host x64/x86 | Each `HT COMPLETE groups=14 failed=0`; exit 0 | `build/continue-hw-host-verified/test-{x64,x86}.log` |
| Host negative controls | 15/15 red as expected; exits 1; only named/dependent groups fail | `test-x64-neg1.log` .. `test-x64-neg15.log` in same directory |
| EE owned-file syntax | 5/5, failed 0, 0 diagnostics | `build/continue-hw-syntax/build.log` (empty) |
| Standalone compile/link | Both ELFs **1674080 bytes**, 0 compile/link diagnostics | `build/continue-hw-verified{16,32}/hwt/build-report.json`, build.log/link.log |
| GS CT16S | `H0 COMPLETE checks=43 failures=0`; wrapper exit 0 | `build/continue-hw-verified16/hwt-full.{log,txt,json}` |
| GS CT32 | `H0 COMPLETE checks=43 failures=0`; wrapper exit 0 | `build/continue-hw-verified32/hwt-full.{log,txt,json}` |
| Soak | Each **600 frames**, **24** repeat-render comparisons, **0** mismatching pixels, **0** timeouts | full logs above; 600 frames is not 600 seconds |
| Ring stress | **8000** quads/cells, **0** wrong, **10** DMA kicks, **72035** qwords | full logs |
| Indexed cache collision | **0/286720** differing pixels; **51462** nonblack pixels | full logs |
| Float/tiny models | 8 cases, **0** differing pixels; **143360** drawn samples | full logs; does not load a real MD2/MD3 file |
| AP88 | **0/256** wrong cells including all alpha bytes/opaque index 255 | full logs; CT16S allows one replicated RGB555 step, CT32 ±1 |
| Selected-slot screenshot | **0** wrong saved-slot RGB; **0/286720** current-frame changes | full logs |
| Sky restoration | **0** differences on repeat with same camera | full logs; initial sky smoke check is only nonblack coverage |
| Mixed-shape pool | **0/699** wrong samples over 60 textures; 801 boundary samples excluded | full logs; not a full texture-pixel PC oracle |
| Shutdown cycle | heap before second Init / after second Shutdown = **3442776 / 3442776** bytes | full logs; inherited label says "before the first Init" but heap0 is sampled inside cycle 1 |

Both verified-mode builds have ELF SHA256
`f6c36ff4c58c0d96246c793c12d6477d532501458508ab343b537f7b54ba8728`;
the framebuffer choice is a runtime argument. Per-source hashes and exact
compiler/link commands are in each `hwt/build-report.json`.

Host test coverage includes 300 random matrix products, 3000 clipped polygons,
52783 triangles in 60 batches, 196 texture shapes, 7098 allocations/6673 frees,
700416 converted texels and 52 upload fixtures (4491264 payload bytes).
Clipping uses area/edge tolerances, not pixel-perfect comparison for all boundaries.

### COP0 Count measurements (emulator, not PS2 hardware speed)

| Workload | CT16S | CT32 |
|---|---:|---:|
| 8000 flat quads, EE calls | 13972782 (1746/quad) | 13970409 (1746/quad) |
| 2000 textured 3D quads, EE calls | 4173519 (2086/quad) | 4173813 (2086/quad) |
| 4000 indexed triangles, EE calls | 4877554 (1219/triangle) | 4871281 (1217/triangle) |
| 20 full-screen textured quads, EE submit | 42626 | 42626 |
| GS wait after that submit | 18593 | 18574 |
| Slowest soak submit+finish | 4755567 | 4835043 |

The tests label Count increments "EE cycles". These are emulator observations,
not calibrated hardware cycle/cache/fill-rate figures. The 20-layer GS wait is
not a realistic physical GS throughput benchmark. No full-engine FPS, CPU
phase profile, 25/35-FPS promise or G5 conclusion follows from them.

Unpaced soak CT16S: dropped=414, flips=210; CT32: dropped=416, flips=208.
600 submits are not 600 displayed frames. The paced check has 30/30 flips and
one preexisting pending frame counted as dropped.

### GS negative controls

Run against the earlier final16 snapshot (1673184-byte ELF, before additional
filter/uniform diagnostic strings; raster/texture/model implementation identical):

```powershell
$env:SRB2_PS2_OUT='D:/Ai-Project3/SRB2-PS2-Port/build/continue-hw-final16'
python -B tools/ps2/run_hw_test.py --no-build --tag neg-z --timeout 600 -- quick negctl=1
python -B tools/ps2/run_hw_test.py --no-build --tag neg-alpha --timeout 600 -- quick negctl=2
python -B tools/ps2/run_hw_test.py --no-build --tag neg-blend --timeout 600 -- quick negctl=4
python -B tools/ps2/run_hw_test.py --no-build --tag neg-zwrite --timeout 600 -- quick negctl=8
python -B tools/ps2/run_hw_test.py --no-build --tag neg-clut --timeout 600 -- quick negctl=16
```

All reached COMPLETE with wrapper exit 0; test-script nonzero exits are expected:

| Disabled feature | Script exit / failures | Observed sensitivity |
|---|---:|---|
| Z test | 1 | depth-resolution wrong winner 1/2; depth-order negative assertion observes 53520 wrong pixels |
| Alpha test | 1 | keyed holes: 6348 colour + 6348 depth mismatches |
| Blending | 6 | Four normal blend checks red; AP88 239/256 wrong; mixed-pool CT32 230 wrong |
| Z writes | 2 | keyed depth 6348 wrong; centre Z=0; wrong winner 1/2 |
| CLUT index swap | 7 | 16432/32768 wrong nearest samples; 128/256 wrong palette cells; palette/pool/eviction red |

Logs/JSON remain `build/continue-hw-final16/hwt-neg-*.{log,txt,json}`.
Several original negative-aware checks report PASS when they observe their
expected corruption; adjacent normal checks still turn red. This is intentional.

## Initial failures and oracle corrections

* The baseline host CLI failed because relative --out was reused after cwd moved
  into that directory. Resolving the path fixed compilation.
* Baseline PCSX2 `build/continue-hw-gs/hwt-baseline.txt` had 36 checks, one FAIL:
  mixed pool **835/1500** wrong samples. The oracle used `fu*width` while the
  actual screen samples used an inset position in a 30-pixel quad. It also let
  `id*7` overflow the 8-bit blue field into alpha. Correct screen-centre/source
  mapping and byte masking resolved the failure. This was not evidence of a
  fixed GS block-layout bug. Boundary exclusions are reported, not hidden.
* Exposing the former free-list loss as a fatal error made an intermediate host
  test abort. The range capacity was then raised to its actual worst-case bound
  and accounting strengthened to require free+used exactly equals pool size.
* Intermediate AP88 CT16S test had one false failure with tolerance 8: replicated
  RGB555 steps can be 9. CT32 independently passed all 256 cells at tolerance 1.
  The corrected CT16S oracle permits one true RGB555 step, not arbitrary colour loss.

## Budgets, limitations, integration requirements

Measured NTSC pools: CT16S **1892352 bytes**, CT32 **745472 bytes**. Peak selected
driver allocations at the workload checkpoint: H state **67304**, DMA ring
**524288**, record capacity **26624**, scratch **64000**, sky **840** bytes.
These omit other statics, readback temporaries and engine/zone allocations.
The actual layout/formulas and simultaneous capture failure are documented in
HW_RENDERER; there is no measured integrated 32-MiB peak in this report.

The full tests report nonzero limitation masks (**0x6800**, 28/27 calls). Known
losses/gaps include RGB555 framebuffer/texture precision, texture decimation,
opaque RGBA index-255 remap, transparent filter fallback, half-size captures,
threshold wipes, source subtract/multiplicative/environment/fog approximations,
normal model lighting, wireframe/coronas, shaders and palette/light-table effects.
PostImgRedraw is implemented but not exercised with an engine underwater/heat grid.

Required coordinator fixes/checks: fresh GENERIC2 capture and reliable screenshot
error reporting; GS/texture-owner lifetime and engine cache flags across switches;
palette routing; explicit capture lifetime/budget policy; real-driver selection
and zero-diagnostic integrated build; and full engine scenario/readback/timing
validation. See HW_INTEGRATION for concrete source paths and suggested IDs.

Not tested: integrated gameplay/title renderer scenes, PC HW frame pairs, real
models/content, PAL/480p, dithering enabled, actual DMA cache on physical PS2,
DMA/FINISH timeout injection/recovery, integrated heap peak, engine performance.
The coordinator's concurrent integrated build is independent evidence.

No commits or registry/G1 edits; no toolchain, assets or golden modifications.
