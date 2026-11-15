# Rejected audio candidates, sixth iteration — 2026-11-14

**No audio production change is retained in this pass.** Both candidates passed
their correctness checks but failed a relevant performance gate. The final
mixer, music decoder, MIDI, backend, thread, converters and public headers are
byte-identical to the immediately preceding completed source reconstructed by
`tools/ps2/prepare_next6_baseline.py` in `build/nov14-next6/baseline-src`.
There is no sixth-pass audio RAM, VRAM, CPU or FPS saving to report.

Final/restored `ps2_music.c` SHA256 is
`3f1a29df3518e6a79072b25f950e10fba087aa52af21c951b243c18dcaac2392`;
`ps2_midi.c` is
`15b44555b6290a1b0a9519a4b09e06f5c7e2d48b19a042e748b9913f47e3ec11`.
All preceding lossless audio work remains intact. No quality, precision, rate,
channel, voice, codec, feature, compatibility, ring, latency or thread capacity
was reduced.

## Rejected candidates

The MIDI candidate rendered event-free spans while preserving note/envelope/
oscillator operations and their order. It passed PCM/private-state/read checks,
including complete private-state hashes at actual input callbacks. Sustained
notes improved, but its 24-voice, one-event-per-frame EE row cost **1.22% more**.
It was removed before the final production build.

The music candidate separated `Ensure` into an always-inline decoded-buffer
test and an exhausted-buffer `Refill`. Its rejected source SHA256 is
`6197800e84effa871bd80746a5291556e7ffaaff153616a201bdc81b893afb90`.
Strict separate-TU, no-LTO timing initially looked positive: all 180 selected
rows improved, with summed Count **189,732,597→177,246,430 (6.58093%)**.
However, the production LTO engine already inlines the preceding `Ensure`.
That strict result did not establish a default-engine benefit.

## Actual production-code performance gate

The decisive fixture uses the **unchanged, already-generated complete-engine
LTO objects**, with real codec support and the same production link flags and
SDK libraries. It does not recompile engine functions for the benchmark.

The local `PS2_MusicRender.constprop.0` clone is renamed/globalized in object
copies. Both original entry sequences consume `a0=music`, `a1=output` and
immediately replace `a2` with 2048 output bytes, establishing the specialized
two-argument, 512-frame ABI. The driver uses opaque music objects created by
the real exported APIs and a complete bounded read callback. `--wrap=main`
replaces startup; no game render loop runs during timing.

Every allocated section remains byte-identical after symbol exposure:
**11,899 sections in the reference, 11,898 in the candidate**. Only symbol
metadata changes. The source objects are preserved as:

- Reference `engine_original.o`: SHA256
  `eb0513a7fab8c8ca0fc1e76d60c2a19fb456e9258329578c6f59afb53a025539`.
- Rejected candidate `engine_original.o`: SHA256
  `7110f9567c16802576520271fc028799585b27829f1dc83a02b7cea7ce71e9b8`.

Each row validates frame counts/canaries and matching PCM, public-state and
ordered-read digests, then times 8192 frames in 512-frame calls, minimum of five
runs using actual EE COP0 Count on the standard 32 MiB PCSX2 profile.

| Actual LTO rows | Reference Count | Candidate Count | Result | Slower rows |
| --- | ---: | ---: | --- | ---: |
| 168 general WAV rate/speed/format rows |124,494,832|122,468,436|1.62769% lower summed cost|52|
| 4 exact-default-step native WAV rows |477,079|484,091|**1.46978% higher summed cost**|1|
| 3 actual stock Ogg rows |6,929,874|6,923,495|0.09205% lower summed cost|0|
| All 175 rows |131,901,785|129,876,022|1.53581% lower summed cost|**53**|

The native 8-bit stereo row regresses **5.18891%**; the worst general row
regresses **3.46694%**. Ogg inputs are O__STJR, O_SPEC6 and O_DHZ, selected by
smallest/median/largest encoded size from all 109 stock Ogg files; gains are only
0.08932–0.09369%. A favorable aggregate therefore does not justify retaining
the candidate's regressions. All cross-ELF digests match, but the change was
reverted on performance grounds.

The rejected real linked Software/Hardware code also changes inlining: Render
is 3128→2176 bytes and Decode 2104→4112 bytes, **+1056 bytes across those two
symbols**, before offsets from removed/inlined helpers. Their frames fall
112→64 and 64→48 bytes respectively; MIDIRead stays 304 bytes. Strict music text
instead shrinks 136 bytes with codecs and 176 bytes without them. These strict
and LTO costs describe the rejected candidate, not the final restored engine.
No whole-game or physical-console speedup is inferred from either fixture.

## Correctness evidence retained for the rejected music candidate

The actual preceding/current functions were compared without reference
normalization. Paired calls check complete output arrays/canaries, private
music/MIDI state, public results and ordered read offsets/length hashes/counts.

- Host: **56,037 checks / 11,809,962 stereo frames / 216 files**, including all
  105 stock MIDI, 109 stock Ogg and non-silent mono/stereo MP3 plus the WAV/MIDI
  matrices, allocations/failures, controls, loops, seeks, speed, release tails,
  EOF, short reads and malformed inputs.
- ASan/UBSan: **55,971 / 11,777,194 / 216 files**, with no unexpected finding.
  PCM-bit, private music-cursor and premature-refill source mutations are
  detected in both host configurations.
- Strict EE full proof: **39,654 checks / 5,996,855 frames**, both PCM/private
  cursor negative controls detected. It uses synthetic inputs without real
  codec libraries; the separate actual-LTO fixture above links real codecs.
- Six additional true-native rows: **4,758 checks / 40,966 frames** on EE,
  host and ASan/UBSan. All 13 complete production strict/benchmark objects are
  byte-identical between the full-proof and added-row ELFs.
- Existing core/thread/backend/ring/equivalence suites and both separate ring
  negative controls pass. Final source restoration is established by byte
  comparison, not by treating these rejected-source tests as final-engine runs.

Actual steps are logged: the existing EE float speed setter can truncate even
a nominal unit ratio to 65535. The six added native rows retain `MusicOpen`'s
integer default 65536 without calling that setter. No float expression changes.

## Artifacts and reproduction

`build/audio-refill-next6-linked/report.json` is the decisive 175-row report;
its baseline/candidate subdirectories retain original/exposed engine objects,
original build reports, benchmark ELFs and logs. All codec arrays/headers are
generated under `build`, not committed. The frozen rejected audio snapshot is
`build/audio-refill-next6-rejected-src`.

With the retained engine objects and archive available:

```sh
source /workspace/.onboarding/environment.sh
python3 -B tools/ps2/audio_refill_next6_linked_check.py
source /workspace/.onboarding/emulator-env.sh
# Run baseline and candidate separately, substituting KIND below.
SRB2_PCSX2_SLOTS=1 python3 -B tools/ps2/run_pcsx2.py \
  --elf build/audio-refill-next6-linked/KIND/AUDIO_LINKED6.ELF \
  --log build/audio-refill-next6-linked/KIND/run.log --until 'LINKED6 DONE' --timeout 180
python3 -B tools/ps2/audio_refill_next6_linked_check.py --summarize
```

The linkage tool also accepts explicit `--baseline-object`, `--candidate-object`
and `--link-report` paths. It verifies allocated-section identity and records
input hashes. These saved objects describe the rejected build, not the restored
production build.

The separate-TU correctness driver accepts an explicit frozen candidate:

```sh
python3 -B tools/ps2/audio_refill_next6_check.py \
  --baseline build/nov14-next6/baseline-src \
  --candidate build/audio-refill-next6-rejected-src \
  --out build/audio-refill-next6-repro --ee
```

Use `--sanitize` for ASan/UBSan; `--ee` builds without running an emulator.
Default `--candidate` is the current checkout, which now matches the reference
audio source. The final driver includes all 180 strict timing rows. The older
retained full proof and six-row addition are combined with
`audio_refill_next6_report.py --target build/audio-refill-next6-20261114-target --native build/audio-refill-next6-20261114-native`.
Complete original host/sanitizer reports are in the corresponding
`build/audio-refill-next6-20261114-final` and `-final-sanitize` directories.
