# OPT16 frame-planner scratch proof

The frame planner's cost table shrinks from `u32[272][8]` to
`u32[272][PLAN_LEVEL_MAX + 1]`, where `PLAN_LEVEL_MAX` is five. The fill loop
computes `lx..cap`, removing the unused `cap + 1` cost. This saves **2,176 bytes
of static EE memory**: 8,704 becomes 6,528 bytes. Costs remain `u32`; the change
does not narrow them.

## Why the removed column and costs are unused

`plan_lmax` starts at five and can only decrease. Initial `lx` and `cap` are
clamped to that maximum. Hidden and full-size-kept entries set both to zero.
The extra relaxation for a large image assigns `cap = lx + 2` only when that
value is at most the image's maximum. Carried immediate entries use the same
bounds. Therefore every entry begins with `0 <= lx <= cap <= 5`.

The only reads of `blk_l` occur after the coarsening loop rejects entries with
`lx >= cap`. Its two indices are `lx` and `lx + 1`; both lie in the initialized
range from the original `lx` through `cap`. Incrementing the chosen entry's
`lx` preserves `lx <= cap`. Consequently six columns suffice, and computing
the cost at `cap + 1` cannot affect the chosen level, score or working-set sum.
The later fidelity pass may refine levels, but computes costs directly with
`plan_blocks` and never reads the scratch table again.

## Executable proof

Run from the repository root:

```sh
python3 tools/ps2/plan_scratch_opt16_hosttest.py
```

The driver reads the reference directly from Git commit
`604cfd150c5240142d8cb5367950b53d987916d1` and the candidate from the working
tree by default. `--source-root` selects a frozen source root containing
`src/`. The final integrated rerun uses the exact frozen ELF source:

```sh
python3 tools/ps2/plan_scratch_opt16_hosttest.py \
  --source-root build/nov16-hwopt/candidate-v7/source \
  --out build/nov16-hwopt/final-planner-v7
```

The final frozen-v7 run passes; its three input files are byte-identical to
the working tree recorded in the report. The durable JSON names the frozen
source root and records both sets of full-file hashes. The later MMI helpers
and texture-allocation retry changes lie outside the extracted planner
dependencies; the reviewed P4 footprint case remains unchanged.

It extracts the complete actual `PS2HWD_PlanEnd` and its actual
`plan_lmax`, `plan_blocks`, histogram quantile, LOD choice, GS footprint and
footprint-planning functions into separate translation units. It first checks
that the candidate function differs only in the reviewed table declaration
and fill limit, that `plan_lmax` differs only in using the five-valued constant,
and that the other extracted arithmetic functions and planner constants match.
The final P4 integration adds one explicitly reviewed exception: the exact
`#ifndef PS2_NOOPT_P4_BAND`/`case PSM_T4` block in `tex_footprint`. The guard
constructs the expected function by inserting that literal block before the
old T8 case; it still compares the complete function and all GS block tables.
Every original T8/CT path must remain equal. The candidate is compiled with
the P4 case enabled, and the extracted planner continues to call the actual
footprint function with unchanged `PSM_T8` (`0x13`).

Host wrappers provide texture fields and renderer state. Read-only observers
record initial levels/caps, table size and cost-call counts. There is no second
implementation of planner decisions. Baseline and candidate retain separate
static scratch across cases, exercising cold and reused table contents.
The standalone `plan_lmax` boundary oracle tests divisibility and minimum-axis
size independently.

Both the optimized host run and the AddressSanitizer/UndefinedBehaviorSanitizer
run passed:

| Check | Result |
| --- | ---: |
| Differential frames | 703 |
| Assertions per successful run | 907,039 |
| Texture records compared per run | 191,216 |
| Assembled entry counts exercised | Every value 0 through 272 |
| Initial coarsening caps exercised | Every value 0 through 5 |
| Width/height maximum-level probes | Every side 1 through 16,384 in both axes |
| Over-budget frames | 538 |
| Removed footprint calls | 71,708 |
| Largest individual cost | 1,048,576 blocks |
| Compiled baseline/candidate table symbols | 8,704 / 6,528 bytes |

The differential checks compare every texture's planned level, immediate hint
and hint frame, plus exact/final working-set totals, budget initialization,
planner-active state, measuring counter, diagnostic entry count, assembled
entry count and initial level/cap bounds. The work counter verifies exactly
one removed footprint calculation per filled table row in every over-budget
case. `nm -S` independently checks the compiled `blk_l` symbol sizes.

Fixtures cover full plan and immediate capacity, duplicate carried keys and
duplicates of planned keys, no-area entries, unmeasurable immediate images,
full-size keep ages 0/149/150, fidelity on/off, large-first fidelity behavior,
histogram-tail boundaries, inactive planning, debug output, odd dimensions,
asymmetric dimensions, the five-level boundary, frame-counter wrap, low/high
pool budgets and reserves below/equal/above the nominal budget. The 420 varied
frames use a fixed random seed.

The 16,384-by-16,384 fixture expects two exact costs to total 2,097,152 blocks,
then two level-two costs to total 131,072 blocks. This would reject a `u16`
cost table, including after coarsening.

## Executed negative controls

Both controls mutate disposable extracted candidate headers and must fail:

1. Changing the fill to `l < cap` omits a required endpoint. The first cold
   level-four/cap-five fixture reports candidate working-set size zero versus
   baseline one block.
2. Restoring `l <= cap + 1` while keeping six columns reaches the same boundary
   and produces UBSan's `index 6 out of bounds for type 'u32 [6]'` error.

The driver reports success only when both ordinary runs pass and both controls
produce their expected executed failures.

Two additional source-guard controls alter the old T8 path and the reviewed P4
case in disposable strings. Both must be rejected by the complete-function
guard. These check the review restriction; they are separate from the two
executed C controls above.

The machine-readable snapshot is
[`PLAN_SCRATCH_HOST_RESULTS_20261116.json`](PLAN_SCRATCH_HOST_RESULTS_20261116.json).
It records source/function/fixture hashes, compiler identity, outcomes and
scratch symbols. Generated sources, commands and full logs are under
`build/nov16-hwopt/final-planner-v7/`.

This establishes host planner parity and scratch safety for this change. The
removed call counts are work counts, not EE cycle measurements. Whole-game
build, replay timing and visual validation belong to the integration report.
