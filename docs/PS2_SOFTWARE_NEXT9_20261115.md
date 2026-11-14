# Exact slope-light clamp scheduling — ninth pass, 2026-11-15

The reference is `19cf5df2a82b04e37fabe76a4441f3d5062ebaf1`, frozen under
`build/nov15-next9/baseline-src/src`. Its original Software and Hardware
default-LTO objects, ELFs and maps are preserved under
`build/nov15-next9/baseline/{software,hardware}`. The final shipping source is
commit `2d0e8bf28f31a37c2d9ce2af9342f5f35284a69f`; `src/r_draw.c` SHA256 is
`5e17ca3a08749c7fa69b3459d01a8ad97ada908e1ad4d133e755703095875bc8`.

## Change and exactness

Only `R_CalcTiltedLighting` changes, under the existing `PS2_OPT_DRAW` policy.
Its existing width becomes a count-down loop. Negative light still follows a
separate branch; nonnegative light uses scalar `slt`/`movn` to select its upper
clamp. This is ordinary scalar EE code; no MMI instruction is retained.

The original `(end-start)/(ds_x2-ds_x1+1)`, signed `start += step`, arithmetic
shift and destination order remain. For existing valid clipped columns, the
width lies in `[1,640]`, so decrementing that width visits precisely the same
array entries. This is an existing renderer bound, not a new accepted-input
limit. The post-shift value is in `[-32768,32767]`. Negative values produce
zero; otherwise signed `47 < light` followed by conditional selection produces
exactly the original `light >= 48 ? 47 : light`. The early-clobber constraint
keeps the comparison temporary separate from both inputs.

The asm also declares GPR9 clobbered, retaining the original helper's compiler
clobber set. This declaration adds **zero instructions**. An initial version
without it let GCC change caller register allocation. The final declaration
restores every caller's complete original instructions and relocations while
retaining the identical measured optimized helper body.

Every floating-point expression, conversion, lighting scale, division and
precision policy in `R_CalcSlopeLight` remains byte-for-byte original source.
The complete lighting array remains 2560 B. The helper has no calls, callbacks,
texture samples, blends or framebuffer accesses. All fourteen calling drawer
bodies retain their complete original instructions, including transparent
pixel branches and live lookup/background reads. Consequently arbitrary
source contents, including fully transparent inputs, cannot change the cost
or behavior of the modified helper.

`r_draw8.c`, `r_draw8_npo2.c`, `r_plane.c` and `r_segs.c` remain byte-identical to
the reference. Restoring only `R_CalcTiltedLighting` reconstructs the entire
reference `r_draw.c` byte-for-byte. Original source is selected for ordinary
non-profile builds, `PS2_NOOPT_DRAW` and full `PS2_NOOPT`. There is no sampling,
texture-format, precision, geometry, physics, feature, resource-limit,
allocation, stack-reservation, VRAM or DMA change.

## Actual default-LTO and linked-code proof

The final measured fixture extracts the complete actual frozen/current
integer and floating helpers into a TU separate from external globals and
the driver. Public entry points are `noinline`, `noipa` and
`externally_visible`; neither fixture TU uses LTO. Only the selected reference
and production implementations enter the final functions TU. This avoids
compiler identical-code folding between duplicate exploratory helpers.

`next9_sw_lighting_codecheck.py` requires complete helper instruction,
register, scheduling, branch-distance, padding and ordered-RELA identity with
the actual full default engine LTO object. Only relocation fields and symbol
spelling normalize. Local pooled float relocations resolve to their exact four
literal bytes, retaining the original load instruction. All **15 RELAs** and
three distinct float values are checked in both versions.

The separate linked gate reconstructs every filled address field from the
actual ELF symbols, `_gp`, explicit RELA addends and the map's checked engine
`.sdata` contribution. All **1272 mapped pool bytes** match their object bytes.
Every complete linked helper body and padding byte must equal the fully
resolved object; pointer fields are not merely masked away.

The checker discovers all **14 actual helper callers**, requires the same
complete call graph, and compares all **25,032 caller text bytes**, ordered
RELAs, registers, branches, constants, float literals and padding exactly.
Each caller is then independently bound to fully resolved bytes in both real
ELFs. Thus the preserved caller code is checked directly, rather than inferred
from source or from a register-renaming argument.

Both final shipping metadata builds from source commit
`2d0e8bf28f31a37c2d9ce2af9342f5f35284a69f` pass this complete gate in
`build/nov15-next9/sw/shipping-release/{software,hardware}`. Each uses its own
original frozen object/ELF/map and the same exact final 210-row fixture.

| Final artifact | SHA256 |
| --- | --- |
| Software engine LTO object | `755d8ff55f2a83ec725f25dc784214d914b9a2565600088951759193b69e09a3` |
| Software ELF | `9ccdd6c2b5b3a3dfc8de212742b4acd48a021174203872288a71b65d8919e5d8` |
| Hardware engine LTO object | `2f8258434a6edd50798163d516b7705d20d251a031ed27e7e54ce344e7a4d19a` |
| Hardware ELF | `f945cb854e179b78c9dd5dc4c91e411e338ddde80458993254927074a43d11bb` |

The Hardware generic `.sdata` contribution has **2968 B** and includes **22
R_MIPS_32 data-pointer RELAs**, alongside float constants. The gate resolves
every original zero pointer field using its exact symbol and explicit addend
before comparing all mapped bytes. Their ordered records remain in the report.
The Software contribution remains 1272 B with no such data relocations.

All **nine actual binary negative controls** are rejected: changed instruction
register, RELA target, addend, type or location, changed object literal byte,
changed linked register or resolved pointer, and changed mapped linked literal
byte. These mutate actual object/ELF files and exercise the corresponding gate.
Both final builds reject these nine controls; Hardware additionally rejects
changed mapped-pool data-RELA target and addend, **20 final controls in total**.

Complete real `r_draw.c` objects compile with release EE flags and `-Werror`:

| Section scope | Reference | Final |
| --- | ---: | ---: |
| All `.text*` | 63,392 B | 63,368 B |
| `.rodata*` | 567 B | 567 B |
| `.data*` / `.sdata*` | 32 B | 32 B |
| `.bss*` / `.sbss*` | 15,743 B | 15,743 B |
| Slope-light helper | 376 B | 352 B |
| Helper stack frame | 0 B | 0 B |

Only the helper's text section changes. Every other complete text section and
ordered relocation stream is identical; data, rodata contents and BSS
names/sizes match. Both disabled policies match complete code/rodata and
preprocessed helper tokens. The `PS2_NOOPT_DRAW` framed digest remains
`b9cc0617d4d509df6fa5f434a36166151079d7056ec534cdc052e820fcbdaf80`.
Evidence: `build/nov15-next9/sw/final-objects`.

## Exactness checks

Each of float, `PS2_NOOPT_SLOPE`/double, `PS2_NOOPT_DRAW`, full `PS2_NOOPT` and
ASan+UBSan passes **298,391 direct helper cases**, comparing **763,880,960
complete lighting-array bytes**. Hash: `a3975cddc6bff609`. The actual production
function is extracted verbatim. Coverage exhausts all **65,536 possible shifted
integer results** with four fractional bit patterns, signed endpoints including
`INT32_MIN/MAX`, wrapped endpoint subtraction under the existing `-fwrapv`,
positive/negative/zero steps, lengths 1–640, both array ends and reversed spans.
Thirty thousand additional random endpoint cases use explicitly sequenced RNG
draws. Only original signed-division overflow inputs and zero divisors are
excluded from the C proof.

All **12 executed actual-helper source mutations** are detected: altered shift,
upper clamp and loop extent, across four policies. Evidence:
`build/nov15-next9/sw/final-host`.

The unchanged real fog/solid drawer fixture calls each version's own actual
lighting helpers. Each of the same four policies and ASan+UBSan passes
**68,886 cases** and compares **4,679,618,952 complete framebuffer/canary bytes**,
the complete lighting array and final cursor. Seven live-content alias modes
cover colormap, source, blend table, combined content and overlapping water
background, including forward/backward offsets. Every lookup content remains
a live read. Float/NOOPT_DRAW hash is `bf03e69adb10b6b8`; double/full-NOOPT hash
is `f3db594d48bc9168`. All **16 executed pixel/lookup/cursor/stale-table controls**
are detected. Evidence: `build/nov15-next9/sw/final-pixels`.

## EE measurements

EE GCC 15.2, existing full-feature release `-O2`, `-G8` and `-fwrapv`; PCSX2
2.6.3 with ordinary 32 MiB EE RAM. The final extracted production fixture
reaches `LIGHT DONE`, runner exit zero. Before timing, **6443 native integer
cases** compare **16,494,080 complete array bytes**, followed by full array
comparison for every timing row. Native integer hash: `00a9a8cf376d16cd`.

All **210 paired rows** pass: lengths −1/0/1/2/3/7/8/16/31/32/33/100/320/640,
fifteen lighting modes, 2048 calls, best of five paired repetitions with
alternating order. Modes include entirely negative/ordinary/upper-clamped light,
both crossing directions and conversion extremes. Interrupts are masked only
between timestamps and their prior state is restored afterward.

There are **132 faster rows, 78 exact ties and zero slower rows**. Equally
weighted summed measured cost falls **17.01355787%**. Every entirely negative,
native zero-length and reversed row ties exactly. Faster rows improve
**1.52667–28.19693%**. CP0 Count ticks per measured call for 100 entries:

| Lighting mode | Reference | Final | Reduction |
| --- | ---: | ---: | ---: |
| Entirely negative | 1019.004 | 1019.004 | 0% |
| Ordinary in-range | 1419.004 | 1119.004 | 21.14% |
| Entirely upper-clamped | 1519.004 | 1119.004 | 26.33% |

The source-level zero-length divisor is preexisting undefined C behavior;
the host C proof excludes it. Both checked complete default-LTO binaries
branch out before that division for zero/reversed spans. The fifteen native
zero rows explicitly preserve their observed no-write behavior and cost.
No new source error policy or supported zero-divisor behavior is asserted.

Final timed fixture: `build/nov15-next9/sw/final-ee-zero`. ELF SHA256:
`02803e12165e879ebb0558803fe67a5ecee8d0a72176896ea57c95f29ec75287`.
Functions object SHA256:
`eabacbeb43e48f6045e48f96c565e3cb374685f6564cd8ecaaf81332f9719463`.
The complete functions bind to the actual default-LTO game code as described
above. These include fixed benchmark dispatch costs; the percentages describe
selected warm helper calls, not weighted game FPS, physical-console cache
behavior, power or frame-time variance.

## Rejected formulations

Fully negative and short rows gate adoption, even when aggregate cost improves.
The 195-row exploration grids exclude the later fifteen native zero rows:

| Variant | Faster / equal / slower | Summed cost reduction |
| --- | ---: | ---: |
| Two C ternary clamps | 108 / 17 / 70 | −7.71% |
| Unsigned range guard | 126 / 15 / 54 | −4.64% |
| Nonnegative upper move, original index loop | 110 / 35 / 50 | 4.26% |
| Count-down plus C clamps | 129 / 16 / 50 | −0.18% |
| Count-down plus MMI min/max | 130 / 15 / 50 | 7.36% |

All are rejected. The MMI variant's negative/conversion-extreme row slows up
to **21.77%**. A count-down plus empty-asm/C-upper-clamp alternative has no
regressions but improves the selected sum only **6.55%**, so it is not retained.
These are exploration results, not claims that rejected candidate bodies were
default-LTO game code. Their reference body is actual-code bound; only the
retained production body receives the final full-LTO and linked-byte gate.
Raw extracted sources, host tests and target logs remain under
`sw/{clamp-variants,clamp-count,clamp-last}`.

## Reproduction

With the configured SDK and emulator from [TOOLCHAIN.md](TOOLCHAIN.md):

```bash
source /workspace/.onboarding/environment.sh
python3 -B tools/ps2/prepare_next9_baseline.py
python3 -B tools/ps2/next9_sw_lighting.py \
  --out build/nov15-next9/recheck-light-host \
  --base-src build/nov15-next9/baseline-src/src --negative-controls --sanitize
python3 -B tools/ps2/next9_sw_pixels.py \
  --out build/nov15-next9/recheck-light-pixels \
  --base-src build/nov15-next9/baseline-src/src --negative-controls --sanitize
python3 -B tools/ps2/next9_sw_objectcheck.py \
  --out build/nov15-next9/recheck-light-objects \
  --base-src build/nov15-next9/baseline-src/src
python3 -B tools/ps2/next9_sw_lighting.py \
  --out build/nov15-next9/recheck-light-ee \
  --base-src build/nov15-next9/baseline-src/src --ee-only
source /workspace/.onboarding/emulator-env.sh
python3 -B tools/ps2/run_pcsx2.py \
  --elf build/nov15-next9/recheck-light-ee/LIGHT.ELF \
  --log build/nov15-next9/recheck-light-ee/run.log --until 'LIGHT DONE' --timeout 180
python3 -B tools/ps2/next9_sw_lighting_report.py \
  --out build/nov15-next9/recheck-light-ee
python3 -B tools/ps2/next9_sw_lighting_codecheck.py \
  --out build/nov15-next9/recheck-light-code \
  --fixture-object build/nov15-next9/recheck-light-ee/functions.o \
  --baseline-engine build/nov15-next9/baseline/software/engine_lto.o \
  --baseline-elf build/nov15-next9/baseline/software/SRB2.ELF \
  --baseline-map build/nov15-next9/baseline/software/SRB2.map \
  --candidate-engine build/ps2/obj/engine_lto.o \
  --candidate-elf build/ps2/SRB2.ELF --candidate-map build/ps2/SRB2.map \
  --candidate-variant production --negative-controls
```

Bind the Hardware build's same software helpers independently by selecting its
own frozen and final engine objects, ELFs and maps. Complete game images,
native Hardware startup and memory-pressure integration belong to the
coordinator's final pass report.
