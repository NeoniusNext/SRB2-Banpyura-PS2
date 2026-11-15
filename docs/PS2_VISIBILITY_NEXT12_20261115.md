# Pass 12 Hardware visibility audit

The additional late transparent-wall cull is **rejected**. No renderer source or texture query was merged into the production tree. Existing BSP, frustum, cached-polygon, sprite and shadow culls remain unchanged. This experiment establishes no Hardware RAM/VRAM saving or FPS improvement.

The proposed guard kept every transparent node, both sorting passes and their plane-run barriers, and tested the four submitted world vertices only immediately before the wall's final texture selection. A small read-only query limited elision to initialized small P_8/chroma pairs whose cached no-index-255 state and metadata were known. The skipped path retained lighting and alpha restoration so light-table IDs were prepared in the original order. Unknown metadata, special rendering flags, invalid views, nonfinite/range uncertainty and boundary cases fell through. There was no far-distance cutoff and no entity, thinker, physics, network or audio change.

Three independent state hazards prevented a simple early cull. Removing an invisible wall before sorting merges consecutive plane runs and can reorder visible planes. Omitting the first texture selection can defer `TF_TRANSPARENT` classification or its reset, changing later wall routing and geometry-cache keys. Finally, even a fully clipped known texture contributes to the driver's working-set and eviction feedback. Avoiding that draw can change the budget used for visible textures in later frames. Solving the last issue would require a broader driver policy change; that work was not pursued.

The controlled host traces execute the original `plan_tex_touch`, `tex_drop` and `plan_frame_end` functions, the frame-end reserve block, and the `PS2HWD_PlanEnd` budget block. The texture record definition is copied from production source. External sinks and the surrounding H/TX state are modeled explicitly; these are source-level counterexamples, not native EE timings or a full allocator/raster trace.

| Feedback trace | Original draw | Omitted invisible draw |
| --- | ---: | ---: |
| Unplanned working set, blocks | 640 | 576 |
| Next reserve, blocks | 731 | 658 |
| Next planned-texture budget, blocks | 8269 | 8342 |
| Separate eviction witness: thrash blocks | 256 | 192 |
| Separate eviction witness: plan percent | 69 | 75 |
| Separate eviction witness: next budget, blocks | 7622 | 8342 |

Both ordinary and ASan/UBSan executions reproduced these differences. Removing either feedback update in an executed negative made its corresponding witness fail with exit code 1. Since future budget equality already fails, successful finite screenshot comparisons would not establish the required losslessness.

The frontend host oracle separately passed 262,144 random geometry cases, 2,067 source-extracted drawnode/material replays and 15 metadata-query cases, with the same result under ASan/UBSan. It observed 58,900 geometric rejections. Five executed unsafe negatives detected a naive classification skip, an inside/boundary rejection, an invalid-input rejection, removal of a sort barrier, and omission of lighting preparation. These tests use the actual frontend sorting, collection, lighting and texture-selection source with instrumented host sinks. They do not prove the actual driver transform error bound, target ABI, packet parity, future planner equality or performance. Their 19 modeled resource witnesses are not measured game savings.

The 602-file published source baseline is pinned by `tools/ps2/next12_baseline.json` (published revision `8729f3652629286c0f38b4c203018e3bdeaa68be`, production source `85bd9716b1ed114d46f95831c64a87623038eeaa`). The rejected snapshot changes only `hw_main.c` and `hw_cache.c`. The source checker also verified all 602 production files remained baseline-identical and detected three source-drift negatives. No EE compiler or emulator resources were used for this semantically blocked candidate.

Evidence and replay tools:

- `build/nov15-next12/visibility-audit.json`: complete inventory, ranked proposals, resource/lifetime analysis and final rejection.
- `build/nov15-next12/cull/`: isolated patch, source freeze, source check, host replay, feedback traces and rejection decision.
- `tools/ps2/next12_cull_variant.py`: prepares only the authorized isolated snapshot; refuses production-tree writes.
- `tools/ps2/next12_cull_hosttest.py` and `.c`: exact frontend host replay, independent double clipping, sanitizers and unsafe negatives.
- `tools/ps2/next12_cull_feedback.py`: executes the two planner-feedback counterexamples and their negatives.
- `tools/ps2/next12_cull_check.py`: verifies source scope, post-sort placement, original callers and production-source identity.

The archived production identity check used `next12_cull_check.py --verify-root` before any independent pass12 source changes were merged. The default check replays the isolated baseline/candidate checks without requiring future, independently changed production source to equal the old baseline.

The existing standard GS suite was not rerun for this rejected candidate. Its historical VU0 failure remains explicit: CT32 55/56, CT16S 50/51, both exit code 1, with the 17 original CT32 readbacks. This audit makes no all-green conformance claim.
