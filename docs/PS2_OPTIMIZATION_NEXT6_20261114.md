# SRB2 PS2 exact optimizations — sixth pass, 2026-11-14

Three changes are retained: compact sprite headers, exact bounded NPOT
wrapping, and immutable per-call GS upload parameters. Four GPT-6.1 Sol
subagents at max reasoning handled memory, Software drawing, GS and audio.
The coordinator reviewed the results, built both engines and checked the
integrated output. Both new audio candidates were rejected after relevant
performance regressions; every audio source/header remains unchanged.

The five preceding passes are preserved in commit
`2a3e8838b0ecaa4700da3a22ac7ff4e5a4518bc7`. This is the source reference,
reconstructed by `tools/ps2/prepare_next6_baseline.py` with all **602 source
hashes verified**. Historical reference ELFs predate that checkpoint commit
and embed its earlier Git metadata; their source matches the checkpoint.
Final source revision is **`7de3fcfc8385547a1959ca80609fa4083249a231`**.
Exactly five of the 602 source files differ from the reference.

## Retained changes and costs

| Change | Measured result | Scope and cost |
| --- | --- | --- |
| Sprite-header cache, 16→8 B per entry | **65,536 B less resident payload** at the unchanged 8192-entry reservation | All four fixed-point values remain exact; four-field gathers add 5 fixed-point or 6 float-conversion cycles in the isolated EE probe |
| Seven NPOT perspective drawers | **7.97115% less summed EE cost** across 252 warm rows; **18.06968% less** across 21 forced-miss rows; every row improves | Same two-entry-per-axis cache and sampling; actual complete drawer object text shrinks 6192 B |
| Three CT32 row fillers | **30.1367% less summed EE cost**, all 126 rows improve | Exact upload preparation; final linked helper bodies shrink 36 B, zero added stack |

The sprite table stores the signed integral-pixel high words of existing
fixed-point values. Getters restore the full values before any old arithmetic
or float conversion. The four-pixel top adjustment retains the original wrap
at signed extremes. Initial reserve, doubling, accepted count, owner, lifetime,
allocation ordering and failures are preserved. The table still has 8192
initial slots and can grow as before. Non-PS2 layout remains unchanged.

NPOT coordinates already become signed 16-bit integers before wrapping. The
bounded reciprocal computes the same quotient throughout that existing domain;
misses use native 32-bit division, and zero dimensions keep the original error
path. Floating-point interpolation, precision, sample reads, lighting, blends,
transparency and store order remain unchanged. The GS fillers copy only their
immutable caller-owned parameters; source/palette contents remain live reads.

These are selected target-code measurements in PCSX2, not complete-game FPS,
physical-console cache/power results or measured frame-time variance. No VRAM,
DMA traffic or GPU draw-work reduction is claimed. Geometry, physics, graphics,
formats, Lua, UDMF, add-ons, network support and compatibility limits were not
reduced. Existing release flags and complete feature configuration remain.

Component reports and reproduction tools:

- [Sprite headers](PS2_SPRITECACHE_NEXT6_20261114.md): real allocator/lifecycle,
  signed-value and target-consumer proofs, including the explicit read cost.
- [NPOT drawing](PS2_SW_NEXT6_OPT_20261114.md): bounded arithmetic proof,
  cache/alias controls, target timing and final linked-loop inspection.
- [GS preparation](PS2_GS_NEXT6_20261114.md): caller/lifetime audit, exact
  pixels, timing, final linked helpers and unaffected-path checks.
- [Rejected audio](PS2_AUDIO_NEXT6_OPT_20261114.md): both rejected candidates
  and the exact production-LTO comparison that prevented a native regression.

## Complete builds and downloadable artifacts

EE GCC 15.2, release `-O2`, `-G8`, engine LTO, unchanged SDK/libraries and full
features. Both final builds link successfully; Hardware passes the existing
diagnostic gate. Incremental final rebuilds compile the changed inputs and
rerun the whole engine LTO/link step. Build input hashes match final source.

| Linked text+data+BSS | Preceding | Final | Change |
| --- | ---: | ---: | ---: |
| Software | 6,689,097 B | 6,682,985 B | **−6112 B** |
| Hardware | 7,888,892 B | 7,882,676 B | **−6216 B** |

Section totals include linker layout and build metadata, not just selected
functions. Software BSS changes +8 B and Hardware BSS −88 B; the complete
NPOT object has unchanged data/BSS. There is no new persistent scratch/cache
allocation. Section totals are not total process or peak RAM.

In the final Software ELF, all 14 inspected innermost NPOT pixel loops have
two high multiplies/results, at most two branches, no metadata/stack accesses
and no division/calls. The cache helper uses two native `DIVU` sites, no
division library calls, and an 80→16 B frame. Final lit drawer frames fall
192→144 B, floor-sprite frames 144→128 B, and water 208→192 B. The game's
maximum measured main stack remains unchanged in the stress trace.

Final Hardware CT32 helper sizes are 172→160, 160→148 and 160→148 B, all
without stack frames; context loads fall 2→0 in each pixel loop. Unaffected
`fill_idx` and `fill_ap88` retain their sizes, frames and instruction structure.

[Prebuilt Software/Hardware ELFs, SHA256SUMS and validation metadata](../prebuilt/lossless-20261114/README.md)
are included. The ELFs embed source revision `7de3fcfc8` and are copied
byte-for-byte from the final tested builds. Existing prebuilt releases remain.

## Exactness checks

- Sprite headers: baseline/current/old-switch each pass **13,044,729 checks**,
  including all 262,144 signed field values, 80 real-zone lifecycle layouts,
  growth/fault/eviction checks, ASan/UBSan and four negative controls. EE passes
  **786,432 checks** over 65536 input sets, including exact float bits; its
  wrong-height negative control is detected.
- NPOT: exhaustive **4,294,901,760 coordinate/dimension pairs**, 131,086 cache
  transitions and matching zero-dimension error exits/text. Four rendering
  policies each compare **323,204 cases / 3,141,155,584 framebuffer/canary
  bytes**, including live lookup aliases. Sanitizers and negative controls
  pass. Disabled-policy code remains identical.
- GS: host and sanitizer each pass **318,523 cases / 50,912,388 pixels**;
  EE passes **208,224 cases / 2,524,390 pixels**. Every byte pair/alpha,
  supported alignment, tail, crop and decimation case is preserved; five
  deliberate corruptions are detected.

## Integrated game comparisons

Five fresh final-engine runs complete on standard **32 MiB EE RAM**. Four
preceding-version runs are reused only after verifying their exact reference
ELFs, engine arguments, add-on/script bytes and successful captures. All
**12 whole-frame PPM pairs and every paired loader hash match exactly**;
there are no masked regions or pixel tolerances. Final runs have no warnings,
fatal/heap errors, reported audio underruns or audio hash mismatches.

| Scenario | Paired frames | Final renderer |
| --- | ---: | --- |
| MAP01 NPOT slope/floor/FOF fixture, MIDI, stereo SFX requests, spawn snapshots | 3 | Software |
| MAP11 ordinary single-player, digital music | 3 | Software |
| MAP01 same scripted scene | 3 | Native Hardware |
| MAP02 ordinary single-player, digital music | 3 | Native Hardware |

Both Hardware comparisons confirm native startup without automatic Software
fallback. `-singletics` sets the seed before map loading; captures are at level
tics 20/70/140. The established scene creates eight sloped 63×45 floor sprites,
including opaque/translucent and translated variants, and configures NPOT
slopes/FOFs. These counts are configured coverage, not a claim that every
drawer is visible. The source fixture and four stereo-request markers match.
The unchanged MIDI/Ogg paths both run. Async whole-game audio scheduling is not
claimed bit-identical; controlled audio proofs establish PCM/state equivalence.

| Loaded used-zone bytes | Preceding | Final | Change |
| --- | ---: | ---: | ---: |
| Software MAP01 scene | 6,131,984 | 6,066,448 | −65,536 |
| Software MAP11 default | 15,455,776 | 15,390,240 | −65,536 |
| Hardware MAP01 scene | 8,040,896 | 7,975,424 | −65,472 |
| Hardware MAP02 default | 8,706,976 | 8,641,152 | −65,824 |

The component reservation saves exactly 65,536 B. Small whole-zone differences
also include allocator alignment/layout. These checkpoint savings overlap
that reservation and must not be added together.

## MAP11 cache-pressure trace

The same `-singletics -keepspawn -loadprof -loadhash -zck -zheap 1 -zstack
-zquit 175 -astat` trace reaches frame 175 and `ZQUIT DONE`.

| Metric | Preceding | Final |
| --- | ---: | ---: |
| Arena capacity | 24,743,936 | 24,748,032 |
| Final used zone | 23,957,760 | 23,971,120 |
| Final free arena | 786,176 | 776,912 |
| Final largest free block | 425,088 | 376,064 |
| Peak used zone | 24,733,648 | 24,737,376 |
| Cache evictions | 11,260 | 11,040 |
| Failed allocation attempts | 1094 | 1090 |
| Main stack high-water | 132,560 | 132,560 |

Cache filling reuses available space: final used/peak bytes do not decrease,
and final free/largest blocks are smaller in this trace. The measured table
and loaded-checkpoint saving does not imply universal peak/free improvement.
Neither trace reports warnings or low-memory visual fallbacks. Failed attempts
can be recovered by eviction and are not lost-frame or texture-downgrade
counts. Wrapping 32-bit cycle totals are not converted into FPS.

## Standard GS suite

CT32 reaches **55/56**, CT16S **50/51**. All **17 complete CT32 readback PPMs**
match the preceding pass exactly. Each mode retains the same historical VU0
failure: 49,152 vertices, 24,625 outcode differences, zero maximum XYZ and
reciprocal-W difference. Nonzero full-suite returns are retained; the complete
standard suite is not declared all green. This pass does not change VU0 code.

## Reproduction and evidence

With the configured SDK and emulator from [TOOLCHAIN.md](TOOLCHAIN.md):

```sh
python3 -B tools/ps2/prepare_next6_baseline.py
python3 -B tools/ps2/build.py --jobs 2
SRB2_PS2_HW=1 SRB2_PS2_OUT=build/nov14-next6/hardware \
  python3 -B tools/ps2/build.py --jobs 2
python3 -B tools/ps2/next6_integration_check.py --out build/next6-repro \
  --baseline-sw build/nov14-next6/baseline/software/SRB2.ELF \
  --baseline-hw build/nov14-next6/baseline/hardware/SRB2.ELF
```

Supply preceding-source ELFs at the two baseline paths. They can also be built
inside the isolated reconstructed source directory with its own `build.py`.
The reconstruction guarantees source hashes, not a historical SDK or exact
Git-metadata-dependent ELF image. The integration command needs user-provided
cooked packs at `build/pak`; it generates the deterministic scene if absent.
Use a fresh output directory. `--analyze-only` verifies existing selected
ELFs, staged arguments, fixture bytes, complete captures and logs.

Component reports contain their host, sanitizer, negative-control, EE and
code-inspection commands. Raw integration evidence is under
`build/nov14-next6/runs`, `next6-integration-report.json`, `build-binding.json`
and `gs-integration-report.json`; earlier candidate evidence is preserved
separately. Published `prebuilt/lossless-20261114/validation.json` records the
final binary/source hashes, measurements and frame-pair hashes. No physical
PS2 FPS, stability or power measurement is claimed.
