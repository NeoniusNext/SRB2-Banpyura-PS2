# MIDI storage and synthesis work, second iteration — 2026-11-14

Reference: the complete pre-iteration source snapshot
`build/nov14-next/baseline-src`, including the preceding lossless audio changes.
The reference MIDI SHA256 is
`eee1942fe3fd5e3aa37059249f8c9f0ce90af7ef600010823cbfcaadede1218f`.
This comparison does not use the older git HEAD implementation.
Production changes in this iteration are confined to `src/ps2/ps2_midi.c`.

## Storage

The MIDI object now ends in a flexible array of its validated track count,
instead of reserving all 32 tracks. Each track retains its complete 256-byte
input cache and all original parser/timing fields. There is still one zeroed
allocation, the same failure cleanup, and no allocation during rendering.
The format/count validation and 32-track/24-note limits are unchanged. The
allocation expression cannot overflow after that count validation.

The paired fixture wraps the actual `calloc` call and checks its request and
injected allocation failure for **every count from 1 through 32**. On EE the
request is `2536 + 296 * tracks` bytes, versus 12016 bytes before. Moving the
track array also removes eight bytes of structure padding. These are libc heap
requests, separate from allocator overhead, the zone arena and VRAM.

| MIDI tracks | Previous EE bytes | Current EE bytes | Saved bytes |
| ---: | ---: | ---: | ---: |
| 1 | 12016 | 2832 | 9184 |
| 4 | 12016 | 3720 | 8296 |
| 8 | 12016 | 4904 | 7112 |
| 15 | 12016 | 6976 | 5040 |
| 16 | 12016 | 7272 | 4744 |
| 32 | 12016 | 12008 | 8 |

The 105 stock MIDI files contain 4–27 tracks, with a median of 15. Their MIDI
objects therefore save **1488–8296 bytes per open song**, median **5040 bytes**.
Only the current song is open; these per-song savings are not summed across
the archive. WAV/Ogg/MP3 object sizes and the prior 4096-byte music-buffer
saving remain unchanged. On the host the corresponding request is
`2552 + 312 * tracks`, versus 12536 bytes.

## Synthesis work

Each `PS2_MIDIRead` builds a temporary ordered list of active note pointers and
their exact integer velocity/volume/expression normalization. The synthesis
loop visits that list instead of checking all 24 note slots on every sample.
It checks each listed note's live active state so release completion retains
its original behavior. List order is the original ascending note-slot order;
sample arithmetic, envelopes, oscillator/percussion state, pan, mixing order
and clipping are unchanged.

The existing event/message parser returns a transient change mask: note
creation or all-sounds-off rebuilds the list; volume, expression and controller
reset refresh the affected channels' amplitudes. Other controls continue to
update the same live note/channel fields consumed by synthesis. Multiple
same-frame events still run in their original order before that frame is
synthesized. The cache is rebuilt at every public read, so it introduces no
persistent fields or assumptions about caller block sizes.

The temporary list costs 192 bytes on EE. Strict `-O2 -G0` assembly shows the
`PS2_MIDIRead` call frame increasing from 112 to 304 bytes, within the existing
decoder stack allocation. No thread stack allocation, ring, output buffer,
sample rate, format support or transport/concurrency contract is changed.
Strict non-LTO MIDI object text increases **152 bytes** (8704 → 8856); data/BSS
remain zero. Full-game LTO section sizes are measured separately by the
coordinator. There is no VRAM change.

## Paired verification

```sh
source /workspace/.onboarding/environment.sh
python3 -B tools/ps2/audio_midi_compact_check.py \
  --baseline build/nov14-next/baseline-src \
  --out build/audio-midi-compact-20261114-final --ee
python3 -B tools/ps2/audio_midi_compact_check.py \
  --baseline build/nov14-next/baseline-src \
  --out build/audio-midi-compact-20261114-final-sanitize --sanitize
python3 -B /workspace/.onboarding/audio-tests.py all
python3 -B /workspace/.onboarding/audio-tests.py ring-neg
```

The new driver compares actual copied production functions from that source
snapshot and the candidate; it applies **no reference normalization**. The
private-state probe serializes every field into the preceding fixed 32-track
layout. Every baseline track slot, including unused zero slots, is compared
with the candidate's validated tracks and zero-filled absent tail slots.
All channel/note fields, timing and division remainders, read caches, parser
positions/status, phases, envelopes and noise are included. This serialization
also lets the previous `audio_lossless_check.py` continue testing its older
reference after the private structure layout changes.

The final host run passes **37,013 checks / 9,966,187 stereo frames / 216 real
files** with identical output/canaries, public results, private state, ordered
input read offsets/length hashes and read counts. Coverage includes all 1–32
track counts; actual allocation requests/failures; dense gain/program/bend/
reset/note-steal events; zero/one-frame event spacing; the prior 168 WAV and
64 synthetic MIDI cases; seeks, release tails, EOF, looping, pause, speed,
short reads and malformed inputs. The 216 real files are all 105 stock MIDI
and 109 Ogg tracks, plus generated non-silent mono/stereo MP3. These are bounded
render/seek/loop windows, not complete song exports. PCM-bit and private-phase
bit-flip negative controls are both detected.

ASan/UBSan pass **36,947 checks / 9,933,419 frames / 216 files**, with both
negative controls detected and no unexpected findings. Sanitized runs skip
timing and the additional timing fixture; its dense controls are also covered
by the main synthesis cases. The existing host core/thread/backend/ring/
equivalence suites and both ring negative controls pass, including 50,636,800
threaded/direct frames, 5,944,488 original-mixer frames, 384 native-cursor cases
and 20 million float conversions. The older lossless script also still passes
its full original-reference run (32,915 checks / 8,874,319 frames / 216 files),
including the separate original Ogg crash and PCM negative controls.

## Host timing

Minimum of five runs, 65,536 frames in 512-frame calls for sustained notes:

| Tracks | Previous ms | Current ms | Reduction |
| ---: | ---: | ---: | ---: |
| 1 | 0.642 | 0.363 | 43.56% |
| 4 | 1.573 | 1.184 | 24.75% |
| 16 | 5.002 | 4.153 | 16.99% |
| 32 | 7.467 | 6.322 | 15.32% |

A second fixture keeps 24 voices active and changes a channel's expression
every 1 or 16 output frames. It compares PCM/state/read traces before timing
16,384 frames, again minimum of five runs:

| Expression-event spacing | Previous ms | Current ms | Reduction |
| ---: | ---: | ---: | ---: |
| 1 frame | 2.098 | 1.931 | 7.95% |
| 16 frames | 1.910 | 1.623 | 15.00% |

## EE verification and timing

```sh
source /workspace/.onboarding/emulator-env.sh
python3 -B tools/ps2/run_pcsx2.py \
  --elf build/audio-midi-compact-20261114-final/ee/AUDIO_MIDI_COMPACT.ELF \
  --log build/audio-midi-compact-20261114-final/ee/run.log \
  --until 'Audio MIDI compact:' --timeout 180
python3 -B tools/ps2/run_pcsx2.py \
  --elf build/audio-midi-compact-20261114-final/ee/AUDIO_MIDI_COMPACT.ELF \
  --log build/audio-midi-compact-20261114-final/ee/negative.log \
  --args=--negative-control --until 'audio-lossless:' --timeout 20
python3 -B tools/ps2/run_pcsx2.py \
  --elf build/audio-midi-compact-20261114-final/ee/AUDIO_MIDI_COMPACT.ELF \
  --log build/audio-midi-compact-20261114-final/ee/state-negative.log \
  --args=--state-negative-control --until 'audio-lossless:' --timeout 20
```

The final EE ELF passes **20,630 checks / 4,153,080 stereo frames**, including
all allocation requests/failures, private MIDI state, input traces, the prior
WAV/MIDI synthetic cases and both 24-voice dense-event fixtures. This fixture
does not need asset files or link real codecs; real-codec coverage is the
separate host test above. Target PCM and private-phase bit-flip negative
controls are detected in separate runs.

Minimum of five runs on the shared standard 32 MiB PCSX2 profile, using actual
EE COP0 Count at 294,912,000 cycles/second, with the same frame counts and
512-frame calls as the host measurements:

| Sustained MIDI tracks | Previous EE ms | Current EE ms | Reduction |
| ---: | ---: | ---: | ---: |
| 1 | 84.050 | 45.482 | 45.89% |
| 4 | 178.424 | 127.238 | 28.69% |
| 16 | 526.733 | 429.019 | 18.55% |
| 32 | 778.502 | 647.174 | 16.87% |

| 24-voice expression-event spacing | Previous EE ms | Current EE ms | Reduction |
| ---: | ---: | ---: | ---: |
| 1 frame | 253.283 | 247.818 | 2.16% |
| 16 frames | 199.883 | 168.579 | 15.66% |

These measure MIDI sequencing and synthesis, not whole-game FPS. The amount
of inactive-note scanning and event frequency determine the benefit. EE
measurements are from PCSX2 executing target code, not physical-console cache
timing or audible SPU2 output. Host measurements do not predict physical-console
timing either. Integrated-game validation is performed by the coordinator.

Reports, reference/candidate sources and strict EE objects/disassemblies are in
`build/audio-midi-compact-20261114-final`; sanitized evidence is in the matching
`-sanitize` directory. The previous-tool regression report is
`build/audio-lossless-after-next-20261114/report.txt`.

Final production MIDI SHA256:
`15b44555b6290a1b0a9519a4b09e06f5c7e2d48b19a042e748b9913f47e3ec11`.
