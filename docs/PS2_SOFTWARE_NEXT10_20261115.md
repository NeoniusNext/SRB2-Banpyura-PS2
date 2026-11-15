# Exact visplane bounds reduction — tenth pass, 2026-11-15

The reference is `5134b87aeb382a7e156acb8a85462bd9059c096a`, frozen under
`build/nov15-next10/baseline-src/src`; its original default-LTO objects, ELFs
and maps remain under `build/nov15-next10/baseline/{software,hardware}`.
The original `r_plane.c` SHA256 is
`bff7bf0c8dcf9d0dc3aecb08699836da18890bc81c3df43b7150df2c74e4290d`.
The retained helper in `e87421615d0271dcf9074656c513f42d0ea1028d` has SHA256
`d5974328b4f1105b5da62c4f42cf11838138b43d08457fcb9fc4f8d6f08a0f9e`.

## Change and exactness

Only `R_PlaneBounds` changes. Under `PS2_OPT_REND` on EE, the per-column
minimum and maximum use `pminw` and `pmaxw`. Each input is a zero-extended
`UINT16`, including `0xffff`, and therefore lies in `[0,65535]`. Signed word
min/max selects exactly the original scalar comparison's result. Initial
loads, loop index, inclusive endpoint and final stores remain unchanged.
Zero, one-column and reversed ranges retain the initial reads and two final
stores. There is no new resource or accepted-input bound.

The actual helper initializes both accumulators and every next value with
`lhu`: the second word of each register's low 64 bits is zero. Word min/max
keeps that second word zero while selecting a nonnegative low word. Induction
therefore preserves the scalar low-64 representation throughout the loop.
The upper 64 bits of the EE's 128-bit registers are never consumed. The
early-clobber constraints prevent one output from overwriting an input
needed by the second instruction.

Explicit `$12` and `$13` clobber declarations preserve the original helper's
compiler clobber set. They add no instructions. The narrow
`PS2_NOOPT_PLANEBOUNDS_MMI` opt-out, `PS2_NOOPT_REND`, full `PS2_NOOPT` and
non-MIPS builds select the original C comparisons. Restoring only this helper
reconstructs the complete frozen `r_plane.c` byte for byte.

The helper reads the current `UINT16` strips and writes only `high` and `low`
after all reads. Strip contents are never cached or copied by production
code. No floating expression, precision, geometry, texture sampling, lighting,
pixel blending, resource limit, allocation, stack reservation or feature
changes. The preceding slope-light loop and every drawer remain in place.

## Portable proof

`next10_sw_bounds.py` extracts the complete frozen and production functions.
The functions TU is separate from its driver, uses the actual `r_plane.h`
definition, and disables LTO and interprocedural specialization. Its `src`
headers are frozen and individually hashed.

Each of profile, narrow opt-out, renderer opt-out, full opt-out and ASan+UBSan
passes **78,600 cases**, comparing **213,792,000 complete metadata and guarded
strip bytes**. The tests exhaust all 65,536 halfword values against their
complements, include 12,000 additional random spans, both array ends, lengths
from −17 through 640, constant/ascending/descending/alternating/random contents
and disjoint/shared/forward-shifted/backward-shifted strip contents. Every
field, padding byte, strip and guard is compared; no image tolerance is used.
All **12 executed actual-source extent/sample/output mutations** are detected.
Evidence: `build/nov15-next10/sw/final-host`.

An exploratory extra case read `INT32` metadata through a `UINT16 *`. It was
removed from the final proof because that effective-type alias is outside
portable C. The four ordinary strip alias modes above are valid typed reads.
The exploratory timing grid itself used ordinary disjoint strips.

## Code and target validation

Final warm/cache-invalidated measurements and both default-LTO bindings pass
for the frozen final `e87421615` builds below. These are component gates; the
coordinator reports whole-game integration and the final retention decision.
The final clobber-preserving fixture is exactly the same complete **96-byte**
MMI body measured in exploration.

All eight complete real `r_plane.c` objects compile with the release EE flags
and `-Werror`, using matching headers in both measured versions. Only the bounds
text section changes; every other complete instruction and ordered RELA stays
identical. The compiler renames one `CSWTCH` table 400→398; the gate resolves
that generated name to the complete unchanged literal bytes and alignment.

| Profile section scope | Frozen helper | MMI helper |
| --- | ---: | ---: |
| All `.text*` | 48,704 B | 48,696 B |
| `.rodata*` | 260 B | 260 B |
| `.data*` / `.sdata*` | 40 B | 40 B |
| `.bss*` / `.sbss*` | 146,548 B | 146,548 B |
| Bounds helper | 104 B | 96 B |
| Bounds helper frame | 0 B | 0 B |

All three disabled policies match complete text/rodata and preprocessed helper
tokens exactly. Helper token SHA256:
`50804d71371e073656b7f53f05e4f1c294c05f8a3be36ef09ab2f60183f2c867`.
Evidence: `build/nov15-next10/sw/final-objects`.

The final source restores the original **444-byte `drawseg_t` ABI**. The
earlier 420-byte layout and its caller proofs remain rejected-context evidence.
The unchanged `visplane_t` and complete 96-byte timed helper still bind exactly
to both final default-LTO objects; no timing result is inferred from a changed
function body.

The gate separates the historical 104-byte helper pin (`--historical-*`) from
the full caller reference (`--baseline-*`). Software uses the existing
104-byte bounds/old-cache control under the 444-byte ABI. That reference also
opts out the independent cache walk, so it is **not** described as a
single-change causal control. Hardware uses its original 104-byte/444-byte ABI
reference. Both comparisons directly require complete caller code, ordered
RELAs, data and frame identity; no identity is inferred across layouts.

The gate checks complete instructions, register choices, scheduling, relative
branches, padding, ordered explicit RELAs and frame records. Engine-defined
addresses use their exact map input contributions, avoiding collisions with
similarly named SDK local symbols. GP section-symbol addends account for the
partial link's original GP bias. Merged strings must match their complete
literal bytes at a unique address satisfying their original alignment. The
actual pinned link command supplies `memcpy` wrapping. No linked address field
is merely masked away.

Both renderer gates bind all **7,608 caller bytes**, all **143 ordered RELAs**,
the **400-byte caller frame**, both complete merged strings and all referenced
BSS capacities in `R_DrawMasked`, including its three helper calls. All 37
referenced-data records match the appropriate 444-byte ABI reference. The
helper has no code RELAs or literal loads and a zero-byte frame.

The Software control starts with 189 canonical `e7131dc7f` IR inputs compiled
with the 444-byte-ABI opt-out,
retains 187, recompiles `r_plane` with the narrow bounds opt-out, and imports the
separately checked old-cache `w_wad.o`. Its report verifies the imported
object against the exact same canonical IR/engine/ELF graph. The frozen
source tree and original generated config are pinned; the config is archived
before use. Path normalization removes only the exact repository prefix
from LTO diagnostics; complete raw streams remain.

A Software reference cannot silently stand in for Hardware data: an attempted
cross-mode comparison correctly failed because `vid` is 56 bytes in Software
and 68 in Hardware. The final Hardware reference preserves its complete
68-byte data record. Earlier 420-context reports and their explicitly labeled
after-control config recovery remain separate; they are not final references.

The measured object also binds directly to both complete **104/96-byte bodies
and exact symbol sizes in the staged executed `BOUNDS.ELF`**. The staged-run
manifest pins all ELF/object/source/header bytes before and after execution,
the original compiled inputs, runner arguments, runner return zero, emulator
binaries, complete config snapshots and raw logs. The game code gate checks
that manifest and reparses the complete native log without modifying it.
Thus object→timed ELF and object→default-LTO→resolved game ELF are all checked.

All **15 actual binary controls per renderer**, 30 total, are rejected:
helper register/MMI/frame, caller register/frame, caller RELA target/addend/
type/location, linked helper/caller register, linked resolved call pointer,
linked literal byte, and executed helper register/symbol size. These mutate
actual object or ELF files and exercise the corresponding gate.

| Final `e87421615` artifact | SHA256 |
| --- | --- |
| Software engine LTO object | `417eca1b22fc1254134cb1eeac94f7edcaee35d25a2f1d474488836b6f1e4545` |
| Software ELF | `ab5de93e035b0942a1024e3dd2ac6235ef6285a7a6406e77259946f852faaefe` |
| Hardware engine LTO object | `3d35737dc290160370300313dc8cba5136f317ba7c729a97c01a7de85aa0f484` |
| Hardware ELF | `68a4468999ee769077cbe6e866bb4b577a514e72e4d04872baef618258daa5c1` |

Final evidence: `sw/final-release-code/{software,hardware}`, the Software
`sw/all-topology-optout` reference, the original Hardware reference and
`sw/final-ee-staged`, all under `build/nov15-next10`. Earlier `shipping-code`
and `current-context-*` directories preserve the rejected 420-layout phase.

## Final EE measurements

The corrected native corpus passes **1,448 cases** and compares **3,892,224
complete metadata/strip bytes** before timing. Every timing row additionally
compares the complete result. The final run reaches `BOUNDS DONE`; the runner
returns zero. The actual EE minimum→maximum source mutation is separately
detected on a two-column ascending case, expected `1,644`, actual `2,644`.
All **14 executed parser mutations** reject missing/duplicate/malformed rows,
wrong counts, wrong modes, zero timings and false completion.

All **196 paired rows** pass: the same fourteen lengths and seven content
modes in each of warm and cache-invalidated modes, best of five paired
repetitions with alternating order. Warm rows use 2048 calls; the other mode
uses 32 individual calls, each preceded by `FlushCache(0)` and `FlushCache(2)`
outside the timestamps. Interrupts are masked only between timestamps and
their prior state is restored.

| Final mode | Faster / equal / slower | Summed per-call cost reduction |
| --- | ---: | ---: |
| Warm | 77 / 21 / 0 | 7.494114084% |
| Cache invalidated | 77 / 21 / 0 | 7.514156646% |
| Equally weighted combination | 154 / 42 / 0 | **7.504121982%** |

Every reversed, zero and one-column row ties in both modes. For 100 columns,
warm cost falls **1316.002→1217.002 CP0 Count ticks per measured call**;
cache-invalidated cost falls **1313→1214**. These include fixed driver
dispatch costs, which differ between the warm loop and individual-call mode.
The combined reduction sums per-call cost, accounting for their different
call counts.

The emulator uses standard 32 MiB RAM, cycle rate/skip zero and
`EnableEECache=false`; the cache-invalidated gate records that emulator's
observed explicit invalidation path, not physical-console cache timing.

Final staged fixture ELF SHA256:
`b2c4c77bcabecd83dda47dcae9f156abe4e5d884550ce4d005fa7d64b6d86ff7`.
Functions object SHA256:
`d5828f1b0b7d51819a32e45f4f31bda90ae3df9d46de8995929510f1544f43f8`.
Run-manifest SHA256:
`68d36bccb445fdd559ef425dc7115c489d05b14afd91cb8769c54af26faa50fd`.
The original run remains in `sw/final-ee`; the staged repeat is byte-exact and
produces the identical 196-row result. The separate target mutation is in
`sw/negative-ee-staged`.

## Controlled game integration inputs

The initial unseeded pressure observations are confounded: `-singletics`
fixes the synchronized `P_Random` seed, while local `M_Random` is still seeded
from OS clocks. Different ambient thunder samples (`DSATHUN1`/`DSATHUN2`) have
different sound allocation sizes and can change later texture-cache churn.
Those observations are retained, and do not establish that a source change
caused the different cache work.

`next10_local_rng.py` creates clearly named **diagnostic copies**, without
writing production source or any original engine/ELF/map. It replaces only
the existing **212-byte `M_RandomSeedFromOS` symbol** with a 40-byte stub and
172 bytes of NOP fill; the original section's final four padding bytes remain
unchanged. The stub calls each ELF's actual, unchanged `M_RandomSeed(UINT32)`
entry and returns true. It uses a 32-byte frame, saves/restores RA at offset
24, preserves 16-byte stack alignment and reserves 16 outgoing-argument bytes.
The real 108-byte seeder symbol/112-byte owned section, its complete SFC32
algorithm and every other state consumer remain byte-identical.

Both original functions are bound to their actual engine object, resolved ELF
and map. The initializer has exactly one actual startup call. An instruction
audit proves the seeder writes GPRs `{2,3,4,5,6,7,8,10}`, a subset of those
already written by the original initializer, with no new FPU, HI/LO, GP or
callee-saved effects. Thus the replacement also preserves the original
compiler's caller-clobber assumptions. Complete ELF size, every symbol size
and address, all headers and every byte outside the initializer are unchanged.

The primary seed **`0x01b2d3f5`** and secondary **`0x9e3779b9`** were selected
before paired cache results. Both are materialized exactly with `lui`/`ori`;
the JAL target resolves to the existing seeder. EE assembly independently
checks the complete stub and its single explicit call relocation. The same
assembled templates serve Software and Hardware. Read-only `--audit-only`
checks preserve the generated copies and reject ten extent, immediate, call,
NOP-fill, size, stack, return and register mutations per copy/seed.

Final paired copies and hashes are under
`build/nov15-next10/local-rng/{baseline,final-software,baseline-hardware,final-hardware}`.
The earlier `combined` and `combined-hardware` copies preserve the rejected
420-layout candidate. Independent method/artifact audits are
`review/local-rng-patches.json` and `io/local-rng-independent-audit.json`
under the same pass directory. These fixtures control test inputs; shipping
local randomness and gameplay source remain unchanged. The coordinator
reports the paired controlled game results.

## Exploratory EE measurements

The first complete 294-row grid compares three formulations, each against
the actual frozen default-LTO 104-byte helper. Lengths are
−1/0/1/2/3/7/8/16/31/32/33/100/320/640, with seven content modes, 2048 calls
and best-of-five paired repetitions in alternating order. Interrupt state
is restored after each timestamp pair. All timed cases use ordinary valid
disjoint strips. Evidence: `build/nov15-next10/sw/bounds-ee-explore`.

| Formulation | Faster / equal / slower | Summed cost reduction |
| --- | ---: | ---: |
| Original index loop, MMI min/max | 77 / 21 / 0 | 7.514140305% |
| Scalar count-down | 84 / 7 / 7 | 14.767417144% |
| MMI count-down | 91 / 0 / 7 | 22.281557449% |

Both count-down formulations are rejected: each slows every two-column
mode, by two and one CP0 Count ticks per call respectively. The same-loop
MMI proposal ties every reversed, zero and one-column row. Its measured
100-column cost falls 1313.002→1214.002 ticks per call. No aggregate result
overrides a regressed short row.

These are selected kernel costs under PCSX2 2.6.3, not weighted game FPS,
physical-console cache behavior, power or frame-time variance. Complete
shipping game-image pairs, allocator/cache pressure and final artifact hashes
belong to the coordinator's integration report.

## Reproduction

With the configured SDK from [TOOLCHAIN.md](TOOLCHAIN.md):

```sh
source /workspace/.onboarding/environment.sh
python3 -B tools/ps2/next10_sw_bounds.py \
  --out build/nov15-next10/recheck-sw-host \
  --base-src build/nov15-next10/baseline-src/src \
  --variants reference,production --negative-controls --sanitize
python3 -B tools/ps2/next10_sw_bounds_objectcheck.py \
  --out build/nov15-next10/recheck-sw-objects \
  --base-src build/nov15-next10/baseline-src/src
python3 -B tools/ps2/next10_sw_bounds.py \
  --out build/nov15-next10/recheck-sw-ee \
  --base-src build/nov15-next10/baseline-src/src \
  --variants reference,production --ee-only --cold
source /workspace/.onboarding/emulator-env.sh
python3 -B tools/ps2/next10_sw_bounds_run.py \
  --input build/nov15-next10/recheck-sw-ee \
  --out build/nov15-next10/recheck-sw-ee-staged
```

Use one EE compiler worker and the coordinator's serialized emulator slot.
Build a current-header narrow-opt-out control from a complete canonical build
and its matching frozen `src` tree with `next10_sw_bounds_context.py`. Then
bind the exact staged functions object with `next10_sw_bounds_codecheck.py`:
`--baseline-*` selects that control, `--candidate-*` the production files and
`--historical-*` the original renderer's frozen object/ELF/map. Add
`--negative-controls`. Use the renderer's own ELF, map and pinned build report;
never substitute a separate-TU object for the actual default-LTO game object.
