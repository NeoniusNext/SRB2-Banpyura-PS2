# Exact half-rate mixing, fourth iteration — 2026-11-14

Reference: the complete preceding source in `build/nov14-next4/baseline-src`,
reconstructed and verified with `tools/ps2/prepare_next4_baseline.py`. Its
`ps2_audio.c` SHA256 is
`f2b0bc02cb63d8a43d54d725a372f51161bb3e6abf984a0c5b513964dc774211`.
The comparison uses the actual preceding functions, with no normalization.
Git HEAD is older and is not this pass's reference.

Production changes are confined to `src/ps2/ps2_audio.c`. The surrounding
`i_sound.c`, music/MIDI decoders and audio thread are byte-identical to that
reference. The final mixer SHA256 is
`b5c9b225f88e0875346924b47632f786f8d5d2ed880ca4ece2606d0f22fdc70a`.

## Work removed and exact bounds

A voice with fixed-point step exactly 32768 and a fraction below 32768 reads
each source frame twice. The new EE MMI kernel loads two complete source
frames and adds their exact repeated contributions to four output frames.
It covers 8/16-bit mono/stereo and every source alignment. Its loads consume
exactly 2/4/4/8 source bytes respectively; they do not fetch a later source
frame. Sixteen-bit products retain the existing correction for C division
toward zero. Mixing order and final clipping are unchanged.

The helper first bounds the output count against the remaining source frames,
then keeps the original scalar fraction/cursor update for the last zero to
three outputs. Complete pairs advance one source frame and leave the fraction
unchanged. Source EOF clears the same voice at the same render boundary.
High-half or noncanonical fractions, other steps, and zero-gain voices retain
their preceding paths. No source is resampled, rephased or converted for this
optimization. This applies to existing 11025-Hz samples at ordinary pitch and
other source/pitch pairs yielding the same exact step. Twelve of the 223 stock
raw PCM effects have that ordinary-pitch rate (ten 8-bit and two 16-bit mono).

The voice helper receives the immutable sample descriptor instead of eleven
individual arguments. The gain calculation now tests its exact unsigned
numerators against the original denominator before dividing: `n / d == 0`
exactly when `n < d`. Multiplication width, unsigned wrapping, denominator and
the resulting integer gains are unchanged. This explicitly keeps gain-division
work outside the already-silent path.

No allocation, persistent buffer, voice field, cache capacity, input read,
pinning/release rule, thread, ring, output rate, PCM precision, pitch/pan/gain
definition, codec support, feature or compatibility limit is changed.

## Paired verification

```sh
source /workspace/.onboarding/environment.sh
python3 -B tools/ps2/prepare_next4_baseline.py
python3 -B tools/ps2/audio_half_next4_check.py \
  --baseline build/nov14-next4/baseline-src \
  --out build/audio-half-next4-20261114-final --ee
python3 -B tools/ps2/audio_half_next4_check.py \
  --baseline build/nov14-next4/baseline-src \
  --out build/audio-half-next4-20261114-final-sanitize --sanitize
python3 -B /workspace/.onboarding/audio-tests.py all
python3 -B /workspace/.onboarding/audio-tests.py ring-neg
```

The regular host proof passes **1,060,272 checks / 129,685,039 stereo frames**.
ASan/UBSan pass **1,059,840 checks / 129,629,743 frames**, with timing skipped
and no unexpected finding. Both detect deliberately flipped PCM and private
cursor bits in a case that exercises four optimized outputs from two source
frames. The fixture includes the entire preceding silent/mixer suite, all
six fraction boundaries (including noncanonical `UINT32_MAX`), all 16 source
offsets, source lengths and calls around odd/vector/block/EOF boundaries,
explicit signed PCM extrema, every 0–255 voice volume, gain/pan boundaries,
interleaved parameters, adjacent steps, aliased music and 1–64 voices. Whole
output arrays with canaries and the complete mixer/voice object are compared
after every call.

Both actual `I_GetSfx` implementations also pass the existing backend lifecycle
and **363-file** cache proof: 223 stock raw effects, 112 synthetic WAV layouts
and 28 DMX variants. Complete PCM/state digests, retained descriptors/bytes,
ordered input-read hashes/counts, allocation peaks and active/pending pinning,
eviction, failure and release behavior match. Both versions already include
the preceding PCM compaction. This boundary uses the same handstub lump type
on each side; it does not claim to verify a real resource-header ABI change.
PCM-payload and ordered-read negative controls are detected separately.

The established host suites also pass 50,636,800 threaded/direct frames,
5,944,488 frames against the original mixer, 384 native/EOF/odd-offset cases,
20 million float conversions, backend pending-write/fade/cache tests, the IOP
ring checks and both ring negative controls. Their final output is retained in
`build/audio-half-next4-final-regressions.log`.

## EE verification and timing

Reference, candidate and fixture are separate strict `-O2 -G0` translation
units without LTO; neither implementation is included in the benchmark TU.
The driver checks all twelve existing clip/music/native/decimation/format
kernels for identical disassembled instructions, registers and branch offsets,
allowing only relocated absolute targets. They all match. Both renderer call
frames remain 4208 bytes. The selected new helper and MMI frames are 80 and
16 bytes: the half-rate call chain uses 4304 bytes, 48–64 bytes more than its
preceding mono/stereo scalar chain. Thread stack allocations are unchanged.

Strict mixer object text grows **1336 bytes (8912 → 10248)**; data/BSS stay
zero. This pass claims no persistent RAM or VRAM saving. Complete engine LTO
sizes and integrated-game validation are measured by the coordinator.

```sh
source /workspace/.onboarding/emulator-env.sh
SRB2_PCSX2_SLOTS=1 python3 -B tools/ps2/run_pcsx2.py \
  --elf build/audio-half-next4-20261114-final/ee/AUDIO_HALF_NEXT4.ELF \
  --log build/audio-half-next4-20261114-final/ee/run.log \
  --until 'HALF4 DONE' --timeout 300
SRB2_PCSX2_SLOTS=1 python3 -B tools/ps2/run_pcsx2.py \
  --elf build/audio-half-next4-20261114-final/ee/AUDIO_HALF_NEXT4.ELF \
  --log build/audio-half-next4-20261114-final/ee/negative.log \
  --args='audio-test --half-negative-control' --until 'sfx-next3:' --timeout 20
SRB2_PCSX2_SLOTS=1 python3 -B tools/ps2/run_pcsx2.py \
  --elf build/audio-half-next4-20261114-final/ee/AUDIO_HALF_NEXT4.ELF \
  --log build/audio-half-next4-20261114-final/ee/state-negative.log \
  --args='audio-test --half-state-negative-control' --until 'sfx-next3:' --timeout 20
```

The final EE executable passes **1,060,272 checks / 129,743,472 stereo frames**,
and both targeted PCM/private-cursor negative controls fail their intended
comparisons. Host/EE frame totals differ because C argument-evaluation order
changes the preceding randomized fixture's deterministic operation sequence;
each architecture compares its own identical frozen/current sequence.

Across the 24 eligible rows, summed cost falls **332,000,350 → 115,342,192
cycles (65.258%)**. Reduction ranges below include both even and odd PCM starts:

| Source format | 1 voice | 8 voices | 64 voices |
| --- | ---: | ---: | ---: |
| 8-bit mono | 35.97% | 54.90% | 58.77% |
| 8-bit stereo | 42.22% | 62.34% | 66.29% |
| 16-bit mono | 42.08–49.04% | 59.36–65.92% | 62.57–68.88% |
| 16-bit stereo | 44.70–55.54% | 62.10–71.69% | 65.28–74.39% |

The 24 high-phase fallback rows cost at most **0.06996% more**; the other
36 audible rows cost at most **0.06739% more** (some improve slightly).
Already-silent rows do not regress: their measured cost is 0.40–0.61% lower.
These costs include mixer setup, unchanged music mixing, voice mixing and final
clipping, rather than timing an isolated arithmetic expression.

The timing fixture renders 32 × 512 frames per measurement and retains the
minimum of five runs, using actual EE COP0 Count on the standard 32 MiB PCSX2
profile. These are selected mixer costs, not whole-game FPS percentages,
physical-console cache timing or audible SPU2 measurements.

Four focused dispatch ablations are preserved in
`build/audio-half-next4-dispatch`. The initial eleven-argument helper increased
silent-64 cost by 19.68%; reducing arguments alone or adding `noipa` still cost
19.17%. Disassembly showed gain divisions hoisted before the silent exit.
The selected six-argument, explicit-zero-test variant removes that regression
(427168 → 427128 cycles in the focused control) and retains the half-rate
speedup. These rejected variants are not the final production implementation.

Source copies, strict objects/disassemblies, ELF, source hashes, read/cache
reports and complete target logs remain in the named final directories.
`final-proof.json` binds the final source, fixtures, target executable, counts,
timings and negative controls to those artifacts. Source is frozen at the
SHA256 above.
