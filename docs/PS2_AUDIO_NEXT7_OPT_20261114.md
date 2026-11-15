# Vorbis state only for Ogg music, seventh iteration — 2026-11-14

The reference is the frozen, complete preceding source at
`build/nov14-next7/baseline-src`, revision
`b1f243f5f768d24aef5cb3ee62b45f2341e53ff4`. Reference `ps2_music.c` SHA256 is
`3f1a29df3518e6a79072b25f950e10fba087aa52af21c951b243c18dcaac2392`.
The paired fixtures compile the actual preceding/candidate source without
reference normalization.

Production audio changes in this pass are confined to `src/ps2/ps2_music.c`,
SHA256 `3fd8a98987bd9df7386a97bb30da6c078ac19bad90bcd3c09c8dc0bafe5b5b84`.
The separate resource-location change uses the semantic position getter in
`i_sound.c`; it is documented and validated with the resource-memory work.
Mixer, MIDI, audio-thread, conversion and public-header sources remain
byte-identical to the reference.

## Storage and preserved behavior

The music object previously reserved an entire `OggVorbis_File` even for WAV,
MIDI and MP3. It now ends in a flexible array containing that state only when
the existing twelve-byte header sniff begins with `OggS`.

There is still one sixteen-byte-aligned allocation and one complete zeroing
operation. The initial sniff precedes the allocation in both implementations;
there are no additional input reads or allocations. The full Vorbis state is
inside the same allocation for Ogg, with its required natural alignment. The
4096-byte PCM buffer keeps its size, sixteen-byte alignment and lifetime.
Non-Ogg paths never evaluate or access the absent Vorbis state: Ogg calls are
inside the Ogg branches or the existing `ogg_open` close guard.

| Actual music allocation request | Preceding EE | Current EE | Saved |
| --- | ---: | ---: | ---: |
| WAV, MIDI or MP3 | 4944 B | 4224 B | **720 B** |
| Ogg | 4944 B | 4944 B | 0 B |

Host requests are 5216→4272 bytes for non-Ogg, saving 944 bytes; Ogg stays
5216 bytes. These are actual `memalign` requests, separate from allocator
overhead, zone-arena reservation and VRAM. Only the current song is open, so
the saving is not multiplied by the number of archive songs. Stock Ogg music
receives no allocation saving from this change.

No renderer, decoder arithmetic, resampling, source advancement, rate,
precision, channel, note/voice, format, loop, thread, ring, latency or feature
limit changes. Failed initial allocations return before parsing, as before.
Failed Vorbis opens retain the preceding cleanup behavior; successful Vorbis
opens followed by rejected metadata still run `ov_clear` exactly once.

## Paired correctness and failure checks

The host proof passes **73,239 checks / 17,080,403 stereo frames / 227 inputs**:
all 105 stock MIDI and 109 stock Ogg songs, generated non-silent mono/stereo
MP3, and eleven malformed Ogg cases. ASan/UBSan pass **73,173 / 17,047,635**
on those same inputs with no unexpected finding. Timing is skipped under the
sanitizers, accounting for the smaller totals. These are bounded render,
seek, speed, loop, EOF and failure windows, not complete-song exports.

Paired calls compare complete output arrays and canaries, serialized music
cursors/flags and buffered-PCM digests, complete MIDI private state in the
direct MIDI matrices, public results, and ordered input offsets/length hashes
and read counts.
Additional music and MIDI input callbacks compare their state at the actual
input boundary, including deferred input failure and loop/seek refills.

The allocator fixture fills returned storage with nonzero bytes, checks both
sixteen-byte redzones and buffer alignment, checks the complete PCM buffer is
zero before rendering, and verifies every byte of the full Vorbis state is
zero before `ov_open_callbacks`. It verifies the state lies entirely within
the actual requested allocation, and that every guarded allocation is freed.
Host evidence includes **666** paired allocation/failure/cleanup cases and
**298** complete Vorbis zero-state checks.

Malformed inputs include truncation at seven boundaries, CRC-correct invalid
Vorbis version/channel/rate headers, and a two-stream chained Ogg with a
different second serial number. Open results, reads, failure publication and
external `ov_clear` call counts match, including failed opens and rejected
post-open metadata.

The EE proof links real Vorbis/Ogg/MP3 libraries and passes **47,138 checks /
6,914,562 frames**, including **250** paired allocation/failure/cleanup cases
and **86** complete Vorbis zero-state checks. It embeds the smallest, median
and largest stock MIDI and Ogg by encoded size, both MP3 tones and the eleven
malformed Ogg cases. All arrays are generated under `build`; no assets are
committed. Synthetic WAV/MIDI matrices, all 1–32 track counts and the preceding
native/general/small-call render matrices also run.

All three configurations detect independent PCM-bit and private-cursor
negative controls. Actual source mutations that omit tail zeroing or allocate
sixteen bytes too little also fail their intended initialization/bounds
assertions on host, under sanitizers and on EE.

The established core, threaded, backend, ring and original-equivalence host
suites pass, including 50,636,800 threaded/direct frames, 5,944,488 original
mixer frames, 384 native/EOF/odd-offset cases and twenty million float
conversions. Both independent ring negative controls are detected.

## Code costs and production-code gate

Strict EE `-O2 -G0 -Wall -Wextra -Werror` codec-enabled music text increases
**32 bytes**, 6336→6368. Data/BSS are unchanged. Strict Open/Render/Decode
frames remain 128/64/96 bytes. With Vorbis disabled, the **entire strict
production objects are byte-identical** on native and EE builds, both with
MP3 enabled and with all codecs disabled.

The decisive gate links copies of the actual complete Hardware engine LTO
objects, with the original production link command and real codec libraries.
It does not recompile engine functions. The preceding/candidate object SHA256s
are `f9d8de3424c4f8bdf6dc07ad3ae7fb19df586ebbc07a57fab54a4ebea49ae137`
and `6b837f730e77a5ae931a1be1f12fd7d7b82e91c5d8517c1fcb2b7385173bafc4`.
All **11,899 allocated sections** in each original object remain byte-identical
after exposing the local renderer symbol. Both disassembled entries overwrite
`a2` with 2048 bytes at offset four, preserving the existing specialized
two-argument, 512-frame ABI. Only the driver replaces `main`.

Each row checks frame counts, output canaries and matching PCM, public-state
and ordered-read digests. It then measures 8192 frames in 512-frame calls,
minimum of five runs, using EE COP0 Count on the standard 32 MiB PCSX2 profile.

| Actual LTO rows | Preceding Count | Current Count |
| --- | ---: | ---: |
| 168 general WAV rate/speed/format rows | 124,495,000 | 124,495,000 |
| 4 exact-default-step native WAV rows | 477,083 | 477,083 |
| 3 actual stock Ogg | 6,930,230 | 6,930,215 |
| 3 actual stock MIDI | 23,810,835 | 23,797,316 |
| All 178 completed rows | 155,713,148 | 155,699,614 |

All signatures match. The **172 WAV rows have exactly equal Count**. Two codec
rows measure three and 69 Count higher, respectively; the largest increase is
0.00099636%. These small differences and the 0.00869% lower summed result are
not claimed as a CPU improvement.

The full-LTO renderer and Decode sections remain 3128 and 2104 bytes; Open
increases 1664→1704 bytes. For the baseline and candidate separately, all
**39** selected music/MIDI/decoder/callback function sections match byte for
byte between Software and Hardware objects. A frozen Software object copy, input hashes
and the section comparison are retained with the gate.

The measured candidate was built from `d5947d9df66564d7e545a5ec21b325e4695575ec`.
The final source, `2ec90684af655a9286b097224564083c4a007170`, subsequently adds
the bounded composite-growth retry; audio source is unchanged. A read-only
binding compares each measured renderer object with its final counterpart:
all **39 sections and 203 ordered relocations**, including explicit addends,
symbol targets and defining sections, match for Software and Hardware
separately. Three additional defined helpers (float conversion, PCM parsing
and chunk validation), their twelve relocations and all referenced data also
match, including the VU0 constants. The renderer's first eight bytes and
specialized two-argument ABI remain unchanged.

Final object SHA256s are
`a8592311111063376fdc311fb6e61995b1d8fb9efb12d39713bd170b147e836d`
(Software) and
`5e3c0b8b421638ace1450631a83be1c9990ad7c1762cd506687c9d4b8713e5a3`
(Hardware). `final-object-binding.json` also pins the actual final ELF hashes.
The existing 178-row gate therefore binds to those final audio instruction
bodies without another benchmark run.

The fixture also prepares MP3 rows, but the frozen production baseline returns
NULL opening the first generated tone under this wrapped full-engine startup.
A single driver-only `--mp3-only` probe establishes that result; its failure
exit then enters engine teardown instead of completing. **MP3 production
timing is unavailable** in this fixture. The complete passing real-codec host
and EE paired MP3 correctness/allocation proofs above remain separate evidence.
No production startup or codec source was changed to accommodate the benchmark.

No whole-game FPS, physical-console timing or audible SPU2 improvement is
inferred from the selected function timings. The retained benefit is the
smaller non-Ogg music allocation.

## Artifacts and reproduction

```sh
source /workspace/.onboarding/environment.sh
python3 -B tools/ps2/audio_storage_next7_check.py \
  --baseline build/nov14-next7/baseline-src \
  --out build/nov14-next7/audio-storage-host-final
python3 -B tools/ps2/audio_storage_next7_check.py \
  --baseline build/nov14-next7/baseline-src \
  --out build/nov14-next7/audio-storage-sanitize-final --sanitize
python3 -B tools/ps2/audio_storage_next7_check.py \
  --baseline build/nov14-next7/baseline-src \
  --out build/nov14-next7/audio-storage-target-final --ee-only
python3 -B tools/ps2/audio_storage_next7_objectcheck.py \
  --baseline build/nov14-next7/baseline-src
python3 -B /workspace/.onboarding/audio-tests.py all
python3 -B /workspace/.onboarding/audio-tests.py ring-neg
```

`--ee-only` builds the target proof and two storage-mutation ELFs without
starting PCSX2. Run sequentially through the shared emulator wrapper:

```sh
source /workspace/.onboarding/emulator-env.sh
SRB2_PCSX2_SLOTS=1 python3 -B tools/ps2/run_pcsx2.py \
  --elf build/nov14-next7/audio-storage-target-final/ee/AUDIO_STORAGE_NEXT7.ELF \
  --log build/nov14-next7/audio-storage-target-final/ee/run.log \
  --until 'MUSIC7 DONE' --timeout 600
```

The same ELF uses `--args='audio-test --negative-control'` and
`--args='audio-test --state-negative-control'`; their log names are
`negative.log` and `state-negative.log`. The storage mutations use
`tail-zero.ELF` and `tail-size.ELF`, with logs `tail-zero-negative.log` and
`tail-size-negative.log`. Each negative run stops at `audio-lossless:` with
a thirty-second timeout and must contain its intended assertion.

Frozen paired source copies, source/fixture manifests, complete logs and
strict disassemblies are in the three final output directories above.
`audio-storage-target-final/ee-assets.json` records embedded input hashes.
The no-Vorbis object proof is `audio-storage-objects/report.json`; the shared
regression log is `build/nov14-next7/audio-storage-regressions.log`.

The actual LTO gate is
`build/nov14-next7/audio-storage-linked-hardware-final/report.json`.
Original/exposed objects, disassemblies, ELFs and complete logs are in that
directory's `baseline` and `candidate` subdirectories; `sw-hw-code.json`
records the cross-renderer section identity. The first interrupted 180-row
attempt is retained separately in `audio-storage-linked-hardware`; the final
directory retains the MP3 startup probe with its twenty second timeout as well.
`final-object-binding.json` records the final-source section/relocation/data
audit, and `build/nov14-next7/audio-storage-final-proof.json` binds the source,
fixtures, documents, correctness logs and final objects.

```sh
# Read-only final-source binding: no compiler or emulator is run.
python3 -B tools/ps2/audio_storage_next7_finalbind.py

# Reproduce the measured gate in a separate output directory.
source /workspace/.onboarding/environment.sh
python3 -B tools/ps2/audio_storage_next7_linked_check.py \
  --out build/nov14-next7/audio-storage-linked-repro \
  --baseline-object build/nov14-next7/baseline/hardware/engine_lto.o \
  --candidate-object build/nov14-next7/audio-storage-linked-hardware-final/candidate/engine_original.o \
  --link-report build/nov14-next7/baseline/hardware/build-report.json
source /workspace/.onboarding/emulator-env.sh
# Run baseline and candidate separately, substituting KIND below.
SRB2_PCSX2_SLOTS=1 python3 -B tools/ps2/run_pcsx2.py \
  --elf build/nov14-next7/audio-storage-linked-repro/KIND/AUDIO_LINKED7.ELF \
  --log build/nov14-next7/audio-storage-linked-repro/KIND/run.log \
  --args='audio-test --skip-mp3' --until 'LINKED7 DONE' --timeout 60
python3 -B tools/ps2/audio_storage_next7_linked_check.py \
  --out build/nov14-next7/audio-storage-linked-repro --summarize
```
