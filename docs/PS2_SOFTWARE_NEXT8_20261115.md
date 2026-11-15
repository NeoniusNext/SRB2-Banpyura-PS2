# Exact affine NPOT blending — eighth pass, 2026-11-15

Reference source is frozen `bd1160aaf3466ee8bcbb28a92f2116c1b8b64b2a`,
reconstructed under `build/nov15-next8/baseline-src/src`. Reference default
Software LTO code is `build/nov15-next8/baseline/software/engine_lto.o`, SHA256
`a8592311111063376fdc311fb6e61995b1d8fb9efb12d39713bd170b147e836d`.
The earlier reciprocal wrapping, P2 drawers, translated/shaded columns and
seven fog/solid-color improvements remain intact.

## Scope and exactness

Only `R_DrawTranslucentSpan_NPO2_8` and `R_DrawWaterSpan_NPO2_8` change in
`src/r_draw8_npo2.c`. They retain the blend-table address only inside the
existing affine NPOT bounded-step branch. That branch already
requires at least 32 pixels, positive fixed dimensions no larger than
`0x40000000`, canonical starting coordinates and steps strictly within one
flat dimension. All of those conditions, coordinate expressions, corrections,
source indices, pixel expression order, inclusive framebuffer endpoint and
cursor behavior remain unchanged. Short spans and every original fallback
remain in place. There is no new accepted-resource bound or failure policy.

`Impl_VideoSetupBuffer` obtains the framebuffer from a 64-byte-aligned dynamic
allocation; `Impl_SliceScreens`, `V_Init` and `d_main.c` derive screens and
`topleft` from that allocation. Renderer pointer globals occupy separate
static storage. These drawers have no call or callback in the drawing branch.
Retaining the address therefore cannot change when framebuffer writes modify
source, translation, colormap, blend-table or background *contents*. Every
such content remains an ordinary live read. No `restrict`, composed lookup,
precomputed texel, persistent pointer cache, precision or sampling change is
introduced. Independent review found no correctness blocker.

The translucent splat and floor-sprite drawers are restored byte-for-byte to
the reference after their measured regressions. The final production file is
frozen at SHA256
`c8c613ba2533551d6032322cb58a54c65d9db4164406a184f1a1be2e1f49a054`.
Restoring the two changed function bodies reconstructs the complete reference
file byte-for-byte. No other drawer or plane/segment source changes.

## Host proof

Each of profile, `PS2_NOOPT_DRAW`, full `PS2_NOOPT` and ASan+UBSan passes
**502,396 cases** and compares **35,959,998,492 complete framebuffer/canary
bytes**. Every policy produces hash `2781fe042759f197`. Full comparison covers
all bytes, including the original inclusive endpoint and unchanged guards;
there is no tolerance or masked image region. Evidence is in
`build/nov15-next8/sw/host-final-frozen`.

Coverage includes framebuffer widths 1/7/16/31/319/320/512/640; 22 lengths
from −17/−1/0 through 640; eight flat-dimension pairs from 1×1 to 2047×2047;
121 coordinate/step modes including signed extremes, exact negative multiples,
fractional starts, both step signs, repeated wraps, and steps at/outside the
existing fast-path limits. Source/table identities alternate. Seven content
alias modes cover source, colormap, translation, blend table, combined data,
and forward/backward shifted backgrounds. Separate cases cross the final row
and use reversed counts, retaining original clipping behavior.

All **nine negative-control runs** fail as expected: deliberate output
corruption, an altered actual source sample and copying live blend contents
before the drawing loop, across all three policies. An adversarial table alias
writes entry 64 before the next pixel reads it, explicitly rejecting a stale
snapshot of table contents. Only its metadata address may be retained.

An early alias fixture forced row zero while leaving a −1-row water background
offset, producing invalid pre-buffer reads. It was corrected before the
accepted runs above; the complete fresh ASan+UBSan run has no diagnostics.
The malformed fixture is not correctness evidence for either implementation.

## Actual code and memory

The fixture extracts complete actual frozen/current drawer bodies, using real
`UINT16` dimension globals, the complete Software `viddef_t` layout and the
five-element `screens[]` array. Driver/globals and functions compile as separate
translation units without LTO; functions are `noinline`, `noipa` and
`externally_visible`. This prevents fixture-specific alias proofs.

`next8_sw_codecheck.py` requires complete instruction and relocation-stream
identity between the measured function and actual default engine LTO code.
Only relocation fields and symbol spelling normalize. Register allocation,
scheduling, literal constants, relative branches, stack instructions, alignment
padding, relocation target and explicit RELA addend must match exactly. All
four reference drawers pass against both corresponding frozen full default-LTO
objects, with 19/19/20/22 checked relocation entries respectively. The final
Software and Hardware builds from source commit
`956162266634e1fbd422250bb945bd518a5acbfb` also match all four complete timed
fixture functions, including the two restored controls.

| Final build artifact | SHA256 |
| --- | --- |
| Software engine LTO object | `97da81b839687c8272825659d81116090b38dbdc7f46ae519a16283eb812ffc4` |
| Software ELF | `6ef23d16bc59fd917e7b736e529029097033d69895af7c4d30a0c50569e2a01b` |
| Hardware engine LTO object | `731008d48dfb8081463893520025323fb3f7de66ea13afc62ad952b9c4b01857` |
| Hardware ELF | `048c19da28915c9c67a41b257e03b352725229d84d605485b50ec5376721b25f` |

Evidence is scoped separately under
`build/nov15-next8/sw/final-release/software/{code,native}` and
`build/nov15-next8/sw/final-release/hardware/{code,native}`. The corresponding
frozen reference object/ELF for each renderer comes from
`build/nov15-next8/baseline/{software,hardware}`. `binding-summary.json` in the
final-release directory records all reference/final hashes, the source commit,
the exact timed fixture and report paths.

Frozen extracted-body, driver and stub hashes are checked alongside complete
instructions and RELAs. The checker then resolves every actual MIPS RELA
against each final ELF symbol address and `_gp`, requiring **every linked
body/padding byte** to equal the resolved LTO object. Both references and both
final release ELFs pass; filled address fields are verified, not merely
masked away. Each final build rejects five separate static binary controls:
changed linked register/pointer fields and changed object register, RELA
target and RELA addend, **ten rejected controls in total**. They mutate actual
binary bytes; the nine executed pixel negatives are reported separately
above. Both final linked builds independently pass the loop/frame audit.
The Hardware binding concerns these same software drawers included in that
build; the measurements below describe those drawer kernels.

The final complete `r_draw.c` controls retain **63,392 B text, 567 B rodata,
32 B data and 15,743 B BSS**. The two changed functions remain **992 B and
1048 B**, with unchanged **16-byte frames** and save/restore work. Every other
text section and all data, rodata contents and BSS names/sizes match. The
disabled-policy complete code and extracted preprocessed tokens match the
reference; framed object-code digest is `b9cc0617d4d509df6fa5f434a36166151079d7056ec534cdc052e820fcbdaf80`.
Object evidence: `build/nov15-next8/sw/objects-final`.

The native audit constructs real natural loops using instruction control flow,
MIPS delay slots and dominance. A backward jump into earlier hot code is not
misreported as a repeated pixel loop. Each selected actual drawer has two
pixel loops: the bounded branch and original fallback. Its bounded loop
removes one blend-pointer global load per pixel, retaining fallback loads and
zero stack accesses inside either loop.
The complete instructions and relocations outside the bounded setup/loop
window match exactly: 436+444 B for ordinary translucent spans and 476+456 B
for water. This explicitly preserves the original zero/short-span and
fallback code, including cold stack work. No heap/static RAM or stack
reservation increases.

## EE measurements

EE GCC 15.2, existing release `-O2`, `-G8`, full feature configuration; PCSX2
2.6.3 with ordinary 32 MiB EE RAM. The fixture reaches `AFFINE DONE` and the
runner exits zero. All **288 paired rows** compare complete framebuffer/canary
bytes and cursor before timing: four complete drawers, twelve lengths
0/1/2/3/7/8/16/31/32/33/100/320, and six coordinate/source modes. Modes include
both bounded-step signs, both original fallback signs and fully transparent /
fully opaque splat/floor-sprite controls. Each row uses 2048 calls, best of
five paired repetitions with alternating order. EE interrupts are masked
only between timestamps; their previous enabled state is restored afterward,
excluding asynchronous SDK interrupt service from kernel timing.

There are **32 faster rows, 256 exact ties and zero slower rows**. The two
changed drawers have 144 rows: **32 faster, 112 exact ties, zero slower**, with
an equally weighted summed cost reduction of **2.67214%**. All zero/short-span,
original-fallback and unchanged splat/floor-sprite control rows tie exactly.
The **32 eligible bounded rows** improve **4.84615–5.63940%**, with a summed
reduction of **5.43444%**. Their per-drawer summed reductions are 5.52031% for
ordinary translucent spans and 5.35121% for water.

For 100 pixels, bounded positive mode, cycles per measured call:

| Drawer | Reference | Final | Reduction |
| --- | ---: | ---: | ---: |
| Ordinary translucent NPOT | 3631 | 3432 | 5.48% |
| Water NPOT | 3748 | 3549 | 5.31% |

Measured ELF SHA256:
`6af7f59a26a09fef1ab53a3747f3e3f8f74de1e9d47b51fb8f6da0f3d8f19d3e`.
Functions object SHA256:
`0d6734403ac7bc654828e28f632b0550d692660b9c68f3d3fc7e6a30a35c12eb`.
Full rows, copied source inputs, hashes and logs are in
`build/nov15-next8/sw/ee-final`; machine-readable summary is `report.json`.
Its complete selected functions bind to the actual default full-LTO drawers
and resolved linked bytes in both final release builds described above. These component costs do
not establish a weighted game FPS or physical-console improvement.

## Rejected formulations

An unconditional per-call blend-address snapshot passed all exactness checks
but produced **33 slower rows** among 192 measured EE rows. Ordinary spans and
splats cost five extra cycles for zero pixels, three for one pixel and one for
two pixels in most tested modes. Floor sprites and water also regressed on
zero-length calls. It is rejected. Its source, exact-code check and complete
measurements remain under `build/nov15-next8/sw/{all-length.c,ee,code}`.

The bounded-only splat version still saves an additional register on every
call: short/fallback calls cost three extra cycles, and fully transparent
bounded calls cost five extra cycles. The floor-sprite version selects
`mtlo/madd/mflo`; fully transparent bounded calls cost **n+1 extra cycles**.
Both are rejected. Their complete default-LTO functions match the measured
fixture, so these are actual-code costs. Source and 288-row evidence remain
under `build/nov15-next8/sw/{bounded-four.c,ee-bounded-modes}`; actual-code
binding is in `code-provisional-lto` and `native-provisional-lto`.

## Reproduction

With the configured SDK and emulator from `docs/TOOLCHAIN.md`:

```bash
source /workspace/.onboarding/environment.sh
python3 -B tools/ps2/prepare_next8_baseline.py
python3 -B tools/ps2/next8_sw_hosttest.py \
  --out build/nov15-next8/recheck-sw-host \
  --base-src build/nov15-next8/baseline-src/src --negative-controls --sanitize
python3 -B tools/ps2/next8_sw_objectcheck.py \
  --out build/nov15-next8/recheck-sw-objects \
  --base-src build/nov15-next8/baseline-src/src --compile \
  --baseline-engine build/nov15-next8/baseline/software/engine_lto.o
python3 -B tools/ps2/next8_sw_hosttest.py \
  --out build/nov15-next8/recheck-sw-ee \
  --base-src build/nov15-next8/baseline-src/src --ee-only
python3 -B tools/ps2/next8_sw_codecheck.py \
  --out build/nov15-next8/recheck-sw-code \
  --fixture-object build/nov15-next8/recheck-sw-ee/functions.o \
  --baseline-engine build/nov15-next8/baseline/software/engine_lto.o \
  --candidate-engine build/ps2/obj/engine_lto.o \
  --baseline-elf build/nov15-next8/baseline/software/SRB2.ELF \
  --candidate-elf build/ps2/SRB2.ELF --negative-controls
# Bind the same timed fixture to the Hardware build's software drawers:
python3 -B tools/ps2/next8_sw_codecheck.py \
  --out build/nov15-next8/recheck-sw-code-hardware \
  --fixture-object build/nov15-next8/recheck-sw-ee/functions.o \
  --baseline-engine build/nov15-next8/baseline/hardware/engine_lto.o \
  --candidate-engine build/nov15-next8/hardware/obj/engine_lto.o \
  --baseline-elf build/nov15-next8/baseline/hardware/SRB2.ELF \
  --candidate-elf build/nov15-next8/hardware/SRB2.ELF --negative-controls
source /workspace/.onboarding/emulator-env.sh
python3 -B tools/ps2/run_pcsx2.py \
  --elf build/nov15-next8/recheck-sw-ee/AFFINE.ELF \
  --log build/nov15-next8/recheck-sw-ee/run.log --until 'AFFINE DONE' --timeout 180 \
  > build/nov15-next8/recheck-sw-ee/runner.log 2>&1
python3 -B tools/ps2/next8_sw_report.py --out build/nov15-next8/recheck-sw-ee \
  --selected R_DrawTranslucentSpan_NPO2_8 R_DrawWaterSpan_NPO2_8
```

Selected EE kernel cycles do not establish whole-game FPS, physical-console
cache/power behavior or frame-time variance. Whole-game exact images, native
Hardware startup and the unchanged MAP11 pressure trace belong to integration.
