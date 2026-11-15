# Exact double-rate stereo mixing, fifth iteration — 2026-11-14

The reference is the complete immediately preceding source at
`build/nov14-next5/baseline-src`, reconstructed by
`tools/ps2/prepare_next5_baseline.py`. Its mixer SHA256 is
`b5c9b225f88e0875346924b47632f786f8d5d2ed880ca4ece2606d0f22fdc70a`.
Git HEAD is older and is not the reference. The paired fixtures use the actual
reference functions without source normalization.

Production changes are confined to `src/ps2/ps2_audio.c`, frozen at SHA256
`c1aa815b3c28a7527b9af4bf1335b91699006fdd1a873939486bed03eb022746`.
The surrounding backend, decoder, MIDI, audio thread and public headers remain
byte-identical to that reference.

## Work removed and exact bounds

A stereo voice with step exactly 131072 and a canonical fraction reads every
second source frame. The new EE MMI helper gathers four stereo frames from
eight source frames, then applies the preceding gains, signed division toward
zero, accumulator additions and final clipping. It handles both 8-bit and
16-bit PCM at every source alignment. Its unaligned loads consume 16 or 32
source bytes respectively, with all eight source frames bounded inside the
validated PCM range before dispatch. It requires no padding after that range.

Four vector outputs advance the cursor by eight source frames and leave its
canonical fraction unchanged. The scalar remainder retains the same source
values, frame increments, EOF boundary and active voice state. Adjacent steps,
noncanonical fractions, mono sources and existing native/half-rate paths keep
their preceding kernels. The existing zero-gain exit still runs before the
gain division expansion and the new audible dispatch.

The stock raw PCM corpus has eight eligible effects at ordinary pitch: DSBOWL
(8-bit), DSCORKH, DSCORKP, DSHLDGUN, DSMARIOB, DSONEUP, DSPUMPKN and DSVWRE
(16-bit). These are eight of 223 raw effects; their presence does not establish
how frequently they play or how many eligible voices are simultaneously active.
`stock-stereo-corpus.json` records the inputs and hashes.

No source is resampled, converted, rephased or discarded. No allocation,
persistent scratch, sample/voice field, cache budget, pin/release rule, input
read, thread, ring, latency, output rate, PCM precision, pitch/pan/gain
definition, codec, feature or compatibility limit changes.

## Target-only guard and code identity

An initial portable dispatch changed the host compiler's inlined generic-loop
register allocation. The final implementation restricts this new helper and dispatch to
`_EE && !PS2_NOOPT_AUDIO_MMI`. Both native and EE no-MMI **complete strict
objects are byte-identical to the actual preceding source**, and their
preprocessed token sequences match. This is an object comparison, not a
normalized reference implementation.

The full target proof was built before that guard-only edit, from mixer SHA256
`044ee6c7b4bb4c540f312323c76295e9666628138d90ef2eb8cf1e3a3d1c2f24`.
The final guarded EE object is **byte-identical in its entirety**, with identical
preprocessed tokens, to the measured object's SHA256
`9056136f307f0eeb79ecde96484371170dfad7b10330d411186c07ad7ffa3dac`.
The measured target ELF and logs remain under the `-prototype` directory below;
the guard proof binds them to the final production source.

The fourteen preceding clip/music/native/decimation/format/half-rate helpers
retain identical disassembled instructions, registers and branch offsets,
allowing relocated absolute targets. Both renderer frames remain 4208 bytes.
The new stereo helper and MMI frames are 64 and 16 bytes, giving a 4288-byte
selected chain versus 4240 bytes before: **48 bytes more transient stack**.
The preceding half-rate chain already reaches 4304 bytes. Allocated thread
stack sizes remain unchanged.

Strict non-LTO `-O2 -G0 -Wall -Wextra -Werror` mixer text grows **1080 bytes**
(10248 → 11328); data/BSS remain zero. There is no persistent RAM or VRAM saving
from this audio change. Complete engine section sizes and game validation are
measured separately by the coordinator.

## Paired verification

The final host proof passes **2,359,968 checks / 246,243,308 stereo frames**.
ASan/UBSan pass **2,359,296 / 246,157,292**, with timing skipped and no unexpected
finding. Both compare the final guarded host fallback with the actual preceding
implementation. The target proof exercises the new MMI code directly.

The EE executable passes **2,359,968 checks / 246,301,741 stereo frames**. Every
call compares the entire output arrays and canaries plus the complete mixer
and private voice state. Coverage includes both stereo formats, all sixteen
source offsets, signed PCM extrema, all 256 voice volumes, gain/pan boundaries,
fractions 0/1/32767/32768/65535/65536/0x8000ffff/UINT32_MAX, adjacent steps,
lengths around vector/block/EOF boundaries, invalid initial frames including
UINT32_MAX, zero/odd/multiple blocks, 1–64 mixed voices, parameter changes and
aliased music. The preceding silent, general and half-rate matrices also run.
Host/EE frame totals differ because the established fixture's randomized C
argument-evaluation order is architecture-dependent; each target compares its
own identical frozen/current sequence.

Both target negative controls fail their intended comparisons: a flipped PCM
bit after an eligible vector case and a private fractional-cursor bit. The
same controls are detected in the final host and sanitizer runs.

Both actual `I_GetSfx` implementations also pass the established **363-file**
cache proof (223 stock PCM effects, 112 synthetic WAV layouts, 28 DMX variants).
Complete PCM/state digests, retained descriptors/bytes, ordered input reads,
allocation peaks and active/pending pinning, eviction, failure and release
behavior match. Its handstub lump interface is shared by both sides; this is
not a real resource-header ABI test. Payload/read negative controls are detected.

The established all-host audio, backend, thread, equivalence and ring suites
pass, including 50,636,800 threaded/direct frames, 5,944,488 original-mixer
frames, 384 native/EOF/odd-offset cases and 20 million float conversions. Both
ring negative controls are detected. Their final log is
`build/audio-stereo-next5-regressions.log`; the guarded native object identity
also proves that subsequent guard edit preserves the exact tested host code.

## EE timing

Reference, candidate and benchmark are separate strict translation units with
no LTO. Measurements use actual EE COP0 Count on the standard 32 MiB PCSX2
profile: 32 resets/renders of 512 frames per measurement, minimum of five runs.
Each timed call includes mixer setup, unchanged music mixing and final clipping.

Across 24 selected eligible stereo rows, summed cost falls **351,048,071 →
134,847,696 Count (61.5871%)**. Ranges include even and odd PCM starts and
canonical fractions 0 and 65535:

| Stereo source | 1 voice | 8 voices | 64 voices |
| --- | ---: | ---: | ---: |
| 8-bit | 39.580% | 58.438% | 62.139–62.141% |
| 16-bit | 38.189–50.306% | 53.054–64.932% | 55.768–67.383% |

The other **138 timing rows** retain their preceding paths. The largest
measured dispatch overhead is **0.21382%**, for 64 audible 8-bit 2x mono voices.
Thirty silent rows have at most **0.02066%** extra cost; twelve noncanonical
stereo rows have at most **0.07283%**. All 48 preceding half-rate/high-phase rows
are included in the unaffected group. Full per-row results are retained in
`timing-report.json`. A separate short benchmark gate agrees with these results.

These are selected mixer costs, not whole-game FPS, physical-console cache
timing or audible SPU2 measurements. The benefit depends on the active effects,
rates and polyphony; no universal FPS or frame-stability percentage is claimed.

## Reproduction and artifacts

```sh
source /workspace/.onboarding/environment.sh
python3 -B tools/ps2/prepare_next5_baseline.py
python3 -B tools/ps2/audio_stereo_next5_check.py \
  --baseline build/nov14-next5/baseline-src \
  --out build/audio-stereo-next5-20261114-final --ee
python3 -B tools/ps2/audio_stereo_next5_check.py \
  --baseline build/nov14-next5/baseline-src \
  --out build/audio-stereo-next5-20261114-final-sanitize --sanitize
python3 -B tools/ps2/audio_stereo_next5_guardcheck.py \
  --baseline build/nov14-next5/baseline-src \
  --measured build/audio-stereo-next5-20261114-prototype
python3 -B /workspace/.onboarding/audio-tests.py all
python3 -B /workspace/.onboarding/audio-tests.py ring-neg
```

`--ee` builds but does not run an emulator. Execute the final rebuilt ELF, or
the instruction-identical measured ELF retained in the prototype directory:

```sh
source /workspace/.onboarding/emulator-env.sh
SRB2_PCSX2_SLOTS=1 python3 -B tools/ps2/run_pcsx2.py \
  --elf build/audio-stereo-next5-20261114-prototype/ee/AUDIO_STEREO_NEXT5.ELF \
  --log build/audio-stereo-next5-20261114-prototype/ee/run.log \
  --until 'STEREO5 DONE' --timeout 600
SRB2_PCSX2_SLOTS=1 python3 -B tools/ps2/run_pcsx2.py \
  --elf build/audio-stereo-next5-20261114-prototype/ee/AUDIO_STEREO_NEXT5.ELF \
  --log build/audio-stereo-next5-20261114-prototype/ee/negative.log \
  --args='audio-test --stereo-negative-control' --until 'sfx-next3:' --timeout 30
SRB2_PCSX2_SLOTS=1 python3 -B tools/ps2/run_pcsx2.py \
  --elf build/audio-stereo-next5-20261114-prototype/ee/AUDIO_STEREO_NEXT5.ELF \
  --log build/audio-stereo-next5-20261114-prototype/ee/state-negative.log \
  --args='audio-test --stereo-state-negative-control' --until 'sfx-next3:' --timeout 30
```

Final copied sources/host reports are in
`build/audio-stereo-next5-20261114-final` and its `-sanitize` counterpart.
Measured target objects/disassemblies/ELF, both complete timing logs, full
target proof, corpus and cache reports are in
`build/audio-stereo-next5-20261114-prototype`. Guard objects and token/object
identity report are in `build/audio-stereo-next5-guard-proof`.
`final-proof.json` under the final directory binds these artifacts, the final
production source and all fixture sources by SHA256.
