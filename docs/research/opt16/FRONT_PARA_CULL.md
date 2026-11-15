# Cheaper existing sprite/shadow parallelogram cull

Baseline: `604cfd150c5240142d8cb5367950b53d987916d1`. The only production
change owned by this audit is `PS2HWD_ParaHidden` in
`src/ps2/hw/ps2_hw_fx2.inc`. It preserves the existing set of culling decisions.
It adds no distance cutoff, occluder, extent cache or entity exclusion.

The old helper transformed all four homogeneous rows and divided all four
corners before checking the shared outcode. The helper now computes the same
`w` corner expressions first and retains the quad when **any** corner reaches
the original `w < w2` eye guard. It then transforms x, y and z in their original
order, checking for a shared outside plane after each row. A rejected quad
needs neither the later rows nor perspective divisions. A retained quad uses
the original screen-coordinate expressions, corner order, 8-pixel gate and
`quad_no_centre` rule.

The outcode transformation is direct Boolean algebra: an AND of four outcodes
has a set bit exactly when all four corners satisfy that bit's comparison.
The strict comparisons and the arithmetic within each corner are unchanged.
All four eye guards run before a plane can reject, including a last-corner
crossing. The view setup and both source callers (`HWR_FX_QuadHidden` and
`HWR_PS2_SpriteHidden`) remain baseline-identical. Their model, shadow, link-draw,
rotation and interpolation handling is unchanged. Skybox and split views still
pass their current transform and viewport; no result persists between calls.
The same returns retain the prior draw, texture-classification and planner
history, avoiding the new-cull hazards recorded in the pass-12 audit.

`hw_para_cull_hosttest.py` extracts the complete baseline/current functions and
includes the actual cull type, full driver state and pixel-centre helper.
Ordinary host and ASan/UBSan runs each passed **301,751** comparisons, including
shared/mixed outcodes, all six planes, a late eye crossing, touching and adjacent
float boundaries, small pixel boxes in split viewports, arbitrary transformed
parallelograms, rotated current views and nonfinite inputs. Three executed
unsafe negatives detect a lost late eye guard, mixing different corners'
outcodes and omitting the far plane.

The separate EE probe uses the production release flags, including the driver's
per-unit `-O3` override, `-G8` and `-flto`, and the actual native headers/state
layout. It passed **72,375** comparisons. Each
timing row makes 8192 equal calls with equal compiler memory barriers. Values
below are minimum COP0 Count over seven repetitions in alternating order.

| Case | Baseline | Current | Change |
| --- | ---: | ---: | ---: |
| Large visible | 4,972,594 | 3,293,232 | −33.77% |
| Shared left plane | 3,719,218 | 1,351,728 | −63.66% |
| Shared right plane | 3,735,602 | 1,359,920 | −63.60% |
| Shared bottom plane | 3,719,218 | 1,826,864 | −50.88% |
| Shared top plane | 3,719,218 | 1,835,056 | −50.66% |
| Shared near plane | 3,727,410 | 2,302,000 | −38.24% |
| Shared far plane | 3,719,218 | 2,310,192 | −37.89% |
| First corner near eye | 352,306 | 352,304 | Equal within 2 Count |
| Last corner near eye | 786,482 | 786,480 | Equal within 2 Count |
| Small, contains centre | 7,151,666 | 5,660,720 | −20.85% |
| Small, no centre | 6,488,114 | 4,997,168 | −22.98% |
| Visible, varying w | 4,972,594 | 3,293,232 | −33.77% |

The probe's function symbols are 3064 → 2172 bytes (−892), with the same
48-byte stack frame. The compiler already pulls the baseline's eye checks
forward at `-O3`; the two eye rows therefore establish no speedup. These are
measurements of separately extracted helpers,
not the final game's LTO placement, a physical PS2 or overall FPS. This change
claims no persistent RAM or VRAM reduction. The ordinary driver translation
unit also passed the native Hardware syntax check with warnings treated as
errors. The probe's SDK link emitted the existing abicalls/non-abicalls warning;
it is retained in the build log.

The first probe accidentally omitted that per-unit `-O3` override. Its results
remain in `build/nov15-next16/para/` and are superseded by the correctly flagged
`build/nov15-next16/para-o3/` run above. The final probe also selects the same
Hardware feature flags as the ordinary game build.

An earlier explicit reuse of `HWR_FX_ThingHidden`'s clip-centre values was
rejected and removed: the production compiler already eliminated that work,
and reject cases slowed by two Count per call. Its 263,640 host comparisons,
34,264 EE comparisons, source snapshot, patch and timing log remain in
`build/nov15-next16/front/`; it is absent from production source.

The complete published summary is `front-para-results.json`; raw builds,
source-extracted headers, negative logs, disassembly and native log are in
`build/nov15-next16/para-o3/`. Reproduce after sourcing the onboarding environment:

```sh
python3 tools/ps2/hw_para_cull_hosttest.py --ee --out build/para-replay
# Acquire the shared emulator slot before launching this command.
python3 tools/ps2/run_pcsx2.py --elf build/para-replay/ee/PARACULL.ELF \
  --log build/para-replay/ee/run.log --until 'PARACULL DONE' --timeout 180
python3 tools/ps2/hw_para_cull_hosttest.py --out build/para-replay \
  --analyze-ee-log build/para-replay/ee/run.log
```
