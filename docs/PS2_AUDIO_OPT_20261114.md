# Lossless audio buffers, MIDI scheduling and Ogg EOF — 2026-11-14

Baseline: `fc7b98fe6`. Production changes are confined to
`src/ps2/ps2_music.c` and `src/ps2/ps2_midi.c`.

## Changes and bounds

Music no longer reserves separate 4096-byte raw and stereo decode buffers.
WAV and MP3 read directly into the existing 16-byte-aligned stereo buffer.
Unsigned 8-bit and mono PCM expand backward, reading both source channels
before writing their output frame. Input strides are 1, 2 or 4 bytes; output
stride is 4 bytes, so this order cannot overwrite an unread source frame.
Little-endian signed 16-bit stereo already has the required representation and
needs no conversion. The byte-wise conversion fallback remains for other host
byte orders. Decode size, input offsets/read lengths and retained PCM capacity
are unchanged. Ogg and MIDI did not use the removed raw buffer.

Codec-enabled EE objects show the `memalign(16, sizeof *m)` request decreasing
from **9040 to 4944 bytes: 4096 bytes less libc heap per open song**. The same
host allocation decreases from 9312 to 5216 bytes. There is no additional
allocation, persistent buffer or VRAM use. With the strict non-LTO EE object
flags used by this test, combined music/MIDI text grows **632 bytes**:
music 6256 → 6336, MIDI 8152 → 8704. Data/BSS are unchanged (the existing MP3
initialization flag is four bytes). Full-game linked text can differ under LTO.
The 4096-byte figure describes the per-song allocation, not a measured change
to the zone arena's capacity.

`PS2_MIDIRead` now keeps the next track index and its due frame in local
variables until an event executes. Track positions, tempo, global tick and
fractional timing remainder only change when events execute or a seek/reset
runs; a read has exclusive ownership of its sequencer. Each event still uses
the original `Earliest` tie order, `Due` integer arithmetic and `Event` parser.
The same per-output-frame event budget remains. This removes repeated scans
of up to 32 unchanged tracks and repeated 64-bit timing divisions between
events. It adds no scheduler fields to the allocated MIDI object and leaves
the note, envelope, oscillator, percussion and mixing loops unchanged.

Sample rate, stereo output, volume/pan arithmetic, sample values, source formats,
pitch/speed, instrument behavior, track and voice limits, EOF/loop transport,
pause, decoded lookahead, command rings, concurrency fences and audsrv behavior
retain their existing definitions.

## Stability fix and the comparison reference

The expanded seek/EOF tests exposed an existing null dereference: libvorbis may
return zero frames with `pcm == NULL`; the old decoder nevertheless evaluated
`pcm[0]` and `pcm[1]`. ASan reproduced this in the **unmodified baseline** with
stock `O_ACZ1` during an EOF/seek exercise. The production decoder now calls
the PCM conversion only when the frame count is nonzero. End/loop handling
continues through the existing `Ensure` function.

The paired test explicitly applies **this zero-frame guard only** to its
reference copy of `ps2_music.c`; the normalization is recorded in its report.
It changes no positive decode result, PCM sample, read, cursor or timing
arithmetic. No other reference state is normalized. A separate subprocess
compiles the unmodified baseline and demonstrates the crash with a deterministic
legal zero-frame/NULL-PCM result. The guarded subprocess checks zero output and
ended state, then real-codec seek/revive and looping with identical PCM.

## Executed checks

```sh
source /workspace/.onboarding/environment.sh
python3 -B tools/ps2/audio_lossless_check.py \
  --assets /workspace/.onboarding/srb2-assets --ee
PS2_AUDIO_NO_BENCH=1 python3 -B tools/ps2/audio_lossless_check.py \
  --assets /workspace/.onboarding/srb2-assets \
  --out build/audio-lossless-20261114-sanitize --sanitize
python3 -B /workspace/.onboarding/audio-tests.py all
python3 -B /workspace/.onboarding/audio-tests.py ring-neg
```

The sourceable environment and wrapper above are workspace setup helpers; the
new test also accepts `--sdk`, `--assets`, `--baseline` and a private `--out`
directory under the repository's `build` directory.

New paired tests: **32,915 checks, 8,874,319 stereo frames, zero differences**.
These include 168 WAV bit-depth/channel/rate/length combinations, lengths around
1024-frame boundaries, extreme sample bytes, short reads, 0/1/odd/multiblock
render sizes, six speeds, pause, restart, seek, EOF, loop and compressed-music
skip/resynchronization. Sixty-four synthetic MIDI combinations cover 1/2/8/32
tracks, divisions 1/7/96/32767, fractional-frame timing, repeated same-tick tempo
changes, running status, controls, pitch bend, sustain, SysEx/meta skipping,
maximum VLQ, event-budget failures and malformed/truncated input. The complete
private MIDI object is compared after each operation, including track caches,
timing remainders, note phases, envelopes, noise state and parser state.

All **105 stock MIDI and 109 stock Ogg tracks** are opened and exercised at
multiple positions, alongside generated **non-silent mono and stereo MP3**
fixtures. These are bounded playback/seek/loop windows, not complete-track PCM
exports. Output arrays and canaries match; input read-call counts and ordered
offset/length hashes match. A deliberately flipped output bit is rejected.
Real codec calls use the installed host shared libraries with the public codec
headers from PS2SDK ports.

The same **32,915 checks / 8,874,319 frames / 216 files** pass under ASan and
UBSan, with no unexpected sanitizer findings. The isolated unguarded Ogg EOF
subprocess fails as required, and the guarded subprocess and bit-flip negative
control behave as required. Sanitized runs skip timing measurements.

The existing all-host suite passes: PCM/music/MIDI checks; **50,636,800** frames
of threaded/direct-path equivalence; backend queue, pending writes, cache
pinning/reclaim and fades; IOP-ring stale-audio/overflow cases; **5,944,488**
frames against the original mixer; 384 native/EOF/odd-offset state cases; and
20 million float-conversion cases. Both IOP-ring negative controls are detected.
The final Ogg guard does not change non-codec builds used by that suite.

Eight strict EE object variants compile with `-Wall -Wextra -Werror` (reference
and candidate music/MIDI, codecs enabled and disabled). The synthetic EE
equivalence/COP0 benchmark links as
`build/audio-lossless-20261114/ee/AUDIO_LOSSLESS.ELF`. The test script builds this
ELF without running an emulator.

The ELF was then executed on the shared standard **32 MiB PCSX2 profile** via
`run_pcsx2.py` and its single-slot lock. **16,532 checks / 3,061,172 stereo frames
pass on EE**, including all 168 WAV and 64 MIDI synthetic cases and full private
MIDI state comparisons. The EE output-bit negative control is detected by the
PCM/canary comparison. No real-codec calls or asset files are required by this
synthetic target fixture; real-codec coverage is the separate host test above.

```sh
source /workspace/.onboarding/emulator-env.sh
python3 -B tools/ps2/run_pcsx2.py \
  --elf build/audio-lossless-20261114/ee/AUDIO_LOSSLESS.ELF \
  --log build/audio-lossless-20261114/ee/run.log \
  --until 'Audio lossless:' --timeout 180
python3 -B tools/ps2/run_pcsx2.py \
  --elf build/audio-lossless-20261114/ee/AUDIO_LOSSLESS.ELF \
  --log build/audio-lossless-20261114/ee/negative.log \
  --args=--negative-control --until 'audio-lossless:' --timeout 30
```

## Timing and limits

Host minimum of five runs, each generating 65,536 frames in 512-frame calls,
with sustained notes and 1/4/16/32 merged tracks:

| Tracks | Baseline ms | Candidate ms | Reduction |
| --- | ---: | ---: | ---: |
| 1 | 0.755 | 0.637 | 15.56% |
| 4 | 1.750 | 1.613 | 7.83% |
| 16 | 5.389 | 5.045 | 6.38% |
| 32 | 8.497 | 7.581 | 10.79% |

EE minimum of five runs of the same sustained-note benchmark, measured from
COP0 Count and converted to milliseconds at 294,912,000 cycles/second:

| Tracks | Baseline ms | Candidate ms | Reduction |
| --- | ---: | ---: | ---: |
| 1 | 147.482 | 84.050 | 43.01% |
| 4 | 252.940 | 178.422 | 29.46% |
| 16 | 638.491 | 526.717 | 17.51% |
| 32 | 939.937 | 778.487 | 17.18% |

These measure MIDI sequencing plus the unchanged synthesis loop, not whole-game
FPS. EE numbers are from PCSX2 execution of the target code; they do not measure
physical-console cache timing or audible SPU2 output. The working set, event
density and polyphony determine the benefit. No music quality reduction, codec
shortcut, resampling change or feature cut is used. Integrated-game verification
and physical-console timing are separate coordinator/hardware checks.

Artifacts: `build/audio-lossless-20261114/report.txt`, the isolated original and
guarded Ogg EOF logs, strict EE objects/disassemblies and the benchmark ELF.
EE logs are `build/audio-lossless-20261114/ee/run.log` and `negative.log`.
Production source SHA256:

```text
ps2_music.c 3f1a29df3518e6a79072b25f950e10fba087aa52af21c951b243c18dcaac2392
ps2_midi.c  eee1942fe3fd5e3aa37059249f8c9f0ce90af7ef600010823cbfcaadede1218f
```
