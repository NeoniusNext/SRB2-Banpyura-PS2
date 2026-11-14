# SRB2 PS2 exact optimizations — eighth pass, 2026-11-15

This pass retains a smaller sprite-frame layout, two affine NPOT blend
improvements and two CT32 upload callbacks. Four GPT-6.1 Sol subagents at max
reasoning investigated Software, GS, memory and audio; a fifth independently
reviewed correctness and evidence. The coordinator integrated and published
the changes. All preceding optimizations remain intact.

The reference is `bd1160aaf3466ee8bcbb28a92f2116c1b8b64b2a`.
Production source is commit `956162266` (`Compact PS2 sprite frames and
streamline exact render kernels`). The frozen manifest contains all **602
source hashes**; exactly **three source files** change. Audio is restored
byte for byte to the reference after both MIDI candidates regressed.

## Retained changes and measured scope

| Change | Measured result | Scope |
| --- | --- | --- |
| PS2 sprite frames, 108→104 B | **13,440 B less payload** for the stock loader replay; **14,032 B less actual EE zone use**; separate **256 B less static scratch BSS** | 3360 replay frames in 587 distinct roots; all 16 rotations, full flip mask, IDs, growth and lifetimes retained |
| Two affine NPOT blend drawers | **5.43444% lower summed cost** for 32 eligible bounded rows; **2.67214%** across all 144 selected rows | 32 faster, 112 exact ties, zero slower; zero/short/fallback code remains identical |
| AP_88 and RGBA CT32 callbacks | **20.59470% lower summed actual-LTO callback cost** over 84 rows | 76 faster, eight exact one-pixel ties, zero slower; unchanged 308 B bodies and zero-byte frames |

The memory values overlap and must not be added together. The loader replay
uses the stock constructors and pack order, but its payload/zone measurements
are not a promise of identical whole-game memory differences on every map.
The separate static scratch saving is visible in linked BSS.

CPU figures are warm kernel measurements on emulated EE, bound to complete
default-LTO game instructions and relocations. They are not weighted game
FPS or physical-console measurements. No reduction in VRAM capacity,
DMA/GIF traffic or GPU draw work is claimed. No precision, texture size,
sampling, filtering, geometry, physics, features, voices, rates, formats,
Lua/UDMF/add-on/network support or resource limits are reduced.

## Why only these variants remain

The first NPOT candidate captured a blend-table address for every call. It
slowed short calls by up to five cycles. Restricting captures to the existing
bounded path retained gains for ordinary translucent spans and water, while
splat gained unconditional register-save work and floor-sprite MADD selection
slowed fully transparent input. Both latter functions were restored.

The analogous IA88 CT32 induction was exact but cost one extra cycle on
one-pixel rows. Four simple expression variants produced the same code;
IA88 was restored. No new dispatch was introduced.

Two MIDI storage candidates were exact but were rejected using actual
default-LTO synthesis, despite RAM savings. The larger candidate narrowed
note/envelope storage and slowed 29 of 48 MIDI rows, worst 1.5878%. The
channel/track-only candidate still slowed 19 rows, worst 1.2721%, despite
improving the aggregate. **No eighth-pass audio change or saving is retained.**
The full source restoration and final code graph are checked separately.

## Component proofs

- [Sprite frames](PS2_MEMORY_NEXT8_20261115.md): actual target-header ABI,
  unchanged non-PS2 interface, full object opt-out restoration, loader/copy/
  growth/rotation ownership checks in the real arena. Host/sanitizer passes
  69 cases and 6,162,003 checks with four executed mutations. All three EE
  variants pass 69 cases and **1,285,276 literal bytes** against the frozen
  host oracle. Target checks are 6,160,477 baseline/opt-out and 6,160,471
  current because physical heap walks differ. Every measured request/used/
  peak row is nonincreasing. Static fixture backing has exterior canaries;
  production allocator code and allocation policy are unchanged.
- [NPOT drawers](PS2_SOFTWARE_NEXT8_20261115.md): each of three policies and
  ASan/UBSan passes **502,396 cases and 35,959,998,492 compared bytes**, including
  live source/blend/translation/background content aliases. Nine executed
  pixel mutations and five binary/relocation mutations are detected. All
  288 final target rows pass, with 32 faster, 256 equal and zero slower.
  Complete instruction/RELA and fully resolved linked bytes match; exact
  cold/fallback windows, text sizes, data/BSS and 16-byte frames are preserved.
  The timer masks interrupts only inside measurement windows and restores
  their prior state; production interrupt behavior is unchanged.
- [CT32 callbacks](PS2_GS_NEXT8_20261115.md): host/sanitizer each passes
  **204,912 row cases and 30,192,631 pixels**, plus 524,320 unsigned-column
  identities and six executed mutations. Native exact-code fixture passes
  138,514 cases and 1,645,624 pixels. Complete original sections and ordered
  RELAs are embedded and bound to both real game ELFs and the fixture. Entire
  IA88/PSMT8 streams and all three native PPACB sites remain unchanged.
- [Audio rejection evidence](PS2_AUDIO_NEXT8_20261115.md) records full PCM/private-state/read exactness,
  real codecs, failure counts and the two 223-row production-code gates.
  Codec/core/thread/ring regressions and negative controls remain available
  in the component report; exact sound alone did not satisfy adoption.

## Final builds and game integration

Both final full-feature release builds link with zero reported compile/link
diagnostics and embed source revision `956162266`. Final input hashes match
the three retained files and the restored audio source. All final Software
and Hardware code/relocation/linked-byte gates pass against the timed code;
restored audio's complete 42-section/215-relocation graphs match each
renderer’s original reference (seven/eight referenced data sections).

| Linked text+data+BSS | Reference | Final | Difference |
| --- | ---: | ---: | ---: |
| Software | 6,686,225 B | 6,685,969 B | −256 B |
| Hardware | 7,886,004 B | 7,885,748 B | −256 B |

Text/data totals are exactly unchanged for both renderers; each BSS shrinks
256 B. ELF file sizes remain 10,324,660 / 11,261,672 B. Linked section totals
are not total process RAM. The earlier full Software candidate compilation
also emitted three existing unused-symbol warnings in two unchanged files;
those are preserved in raw logs, not suppressed or described as new warnings.

[Final ELFs, hashes and validation metadata](../prebuilt/lossless-next8-20261115/README.md)
are copied from the tested binaries without alteration. Previous releases
remain available.

The main integration selects **12 runs on standard 32 MiB EE RAM**: six fresh
final runs, one fresh reference pressure trace and five verified historical
reference runs from the preceding final build. Exact binary, engine-argument,
stock-asset and scripted-fixture identity is checked before accepting reuse.
All **13 complete PPM pairs** and paired loader hashes match exactly, including
the tic175 pressure image with no earlier screenshot-buffer allocations.
No final run has warnings, plain-column substitutions, heap/fatal errors,
reported audio underruns or decoded/consumed hash errors. Both Hardware pairs
confirm native startup without Software fallback.

The paired scenes are Software MAP01 with the existing NPOT/fog/water/MIDI/
stereo fixture, Software MAP11 ordinary play, native Hardware MAP01 with the
same fixture, native Hardware MAP02 ordinary play, and Software MAP11 with
spawn snapshots and capture only at tic175. These are output/loader checks,
not a claim that every optimized drawer is visible in those screenshots.

| Map-loaded used zone | Reference | Final | Difference |
| --- | ---: | ---: | ---: |
| Software MAP01 combined | 6,073,616 B | 6,059,584 B | −14,032 B |
| Software MAP11 default | 15,330,208 B | 15,316,208 B | −14,000 B |
| Hardware MAP01 combined | 7,982,640 B | 7,968,560 B | −14,080 B |
| Hardware MAP02 default | 8,581,184 B | 8,567,248 B | −13,936 B |

These checkpoints overlap the frame-table saving and must not be added to it.
Whole-zone rounding and heap placement account for their small differences.
The separate unchanged-argument MAP11 `-keepspawn -zheap 1 -zstack -zquit 175`
trace completes with zero warnings:

| Metric | Reference | Final |
| --- | ---: | ---: |
| Arena capacity | 24,743,936 | 24,743,936 |
| Final used zone | 23,905,872 | 23,634,736 |
| Final free arena | 838,064 | 1,109,200 |
| Final largest free block | 530,544 | 478,720 |
| Peak used zone | 24,732,368 | 24,734,944 |
| Evictions | 10,703 | 9,801 |
| Failed allocation attempts | 1,168 | 1,020 |
| Main stack high-water | 132,560 | 132,560 |

Final use is 271,136 B lower, but **peak use is 2576 B higher** and the largest
free block is 51,824 B smaller. Allocation layout changes cache residency and
rebuild work: the final-use difference is not an additional fixed RAM saving,
and no universal peak/free-space improvement is claimed. Failed attempts
include recovered allocations and are not lost frames or downgraded textures.
The established 384 KiB stack reservation and measured 132,560 B high-water
are unchanged. CP0 counters may wrap and are not converted into whole-game FPS.

Two additional fresh no-capture MAP11 traces extend the same arguments to
**350 frames**. Both complete with equal loader hashes and zero warnings,
heap/fatal errors, underruns or audio hash errors. This brings the selected
integration evidence to **14 runs**; the image comparison remains 13 pairs.

| 350-frame metric | Reference | Final |
| --- | ---: | ---: |
| Arena capacity | 24,743,936 | 24,743,936 |
| Final used zone | 24,312,528 | 24,559,152 |
| Final free arena | 431,408 | 184,784 |
| Final largest free block | 195,696 | 32,832 |
| Peak used zone | 24,732,368 | 24,734,944 |
| Evictions | 21,981 | 19,606 |
| Failed allocation attempts | 2,885 | 2,513 |
| Main stack high-water | 132,560 | 132,560 |

At this later checkpoint the final version holds **246,624 B more zone
memory**, within the same pre-reserved arena, while making 2375 fewer cache
evictions and 372 fewer failed attempts. This reverses the final-use ordering
at frame175 and confirms why cache occupancy is reported separately from
fixed metadata savings. No universal reduction in total/peak RAM or game FPS
is claimed; the tested output and texture quality remain unchanged.


The real-driver standard suite preserves its exact historical result:
**CT32 55/56 and CT16S 50/51**, solely the same VU0 outcode mismatch (24,625
of 49,152 vertices, zero XYZ/reciprocal-W error). Its nonzero suite returns
remain visible. All **17 complete CT32 readback images** match the preceding
published hashes. The standalone suite is not declared entirely successful.


## Reproduction

Use the configured SDK/emulator from [TOOLCHAIN.md](TOOLCHAIN.md), full release
features, existing `-O2`, `-G8` and engine LTO. Supply user-owned cooked packs
at `build/pak`; no BIOS or game assets are included.

```sh
python3 -B tools/ps2/prepare_next8_baseline.py
python3 -B tools/ps2/build.py --jobs 1
SRB2_PS2_HW=1 SRB2_PS2_OUT=build/nov15-next8/hardware \
  python3 -B tools/ps2/build.py --jobs 1
python3 -B tools/ps2/next8_integration_check.py --out build/next8-repro \
  --baseline-sw build/nov15-next8/baseline/software/SRB2.ELF \
  --baseline-hw build/nov15-next8/baseline/hardware/SRB2.ELF
python3 -B tools/ps2/next8_gs_integration.py --out build/next8-gs-repro
python3 -B tools/ps2/next8_pressure_check.py --out build/next8-pressure-repro --frames 350
```

Baseline reconstruction verifies all 602 hashes without modifying the live
checkout. Build the reference ELFs in that isolated source if the saved ones
are unavailable; historical SDK/Git metadata and exact binary identity are
not promised by source reconstruction. Original reference LTO objects were
preserved before this pass, so production-code comparisons did not rely on
recompiling the historical baseline.

The game checker generates fixtures if absent and normally runs twelve
scenarios, requiring fresh output directories. `--reuse-baseline` validates
already staged base runs against the selected exact ELF, arguments, fixture
bytes and stock assets; `--analyze-only` rechecks all output. It rejects
**every warning**, including texture substitutions in the separate pressure
run. Component reports provide their own target and sanitizer commands.

The GS checker builds the existing `gs_list_eetest.py` standalone fixture
with its test-only engine hooks. The underlying suite's nonzero VU0 result
is retained, compared exactly with the preceding published signature and
never declared an all-green suite. Compiler diagnostics fail its build; the
existing SDK assembler/C ABI link warning remains explicitly distinguished.

Raw freezes, failed variants, code extracts and full logs remain under
`build/nov15-next8`. Published validation metadata pins final source, tested
ELFs, proof tools, frame images and scoped measurements. Physical PS2 FPS,
frame-time variance and power have not been measured.
