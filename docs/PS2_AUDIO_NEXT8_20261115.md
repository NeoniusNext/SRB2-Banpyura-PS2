# Rejected MIDI storage candidates, eighth pass — 2026-11-15

**No audio production change is retained.** Both private-storage candidates
preserved PCM, state, input reads and supported features, but regressed selected
actual default-engine MIDI timings. A favorable aggregate did not satisfy the
performance gate. Their proposed allocation savings are not final-game savings.

All ten audio production files, including `i_sound.c`, are byte-identical to
the frozen preceding source at `build/nov15-next8/baseline-src`, revision
`bd1160aaf3466ee8bcbb28a92f2116c1b8b64b2a`. Restored `ps2_midi.c` SHA256 is
`15b44555b6290a1b0a9519a4b09e06f5c7e2d48b19a042e748b9913f47e3ec11`.
All preceding audio optimizations remain. There is no eighth-pass audio RAM,
VRAM, CPU, output-rate, voice/channel, codec, precision, cache-capacity, thread,
ring, latency, feature or compatibility change.

## Candidates and bounds

Both candidates stored private MIDI channel controls at their validated widths:
seven-bit program/volume/expression/pan, boolean sustain and fourteen-bit bend.
They reordered private tracks and stored running status and the ended flag in
bytes. Every 256-byte track cache and every supported track remained present.
`cached` remained `size_t`: the input callback can return an oversized count,
which must fail the original full-width comparison before any truncation.

The first candidate also narrowed note flags/channel/key/velocity and the
envelope. The envelope can reach **32992**, when a released note at 32736 is
held again and resumes its attack. A sixteen-bit field preserves this value;
assuming a strict 32768 maximum would be incorrect. Age, oscillator words,
source positions, ticks and timing retained their original widths. Explicit
unsigned operands preserved the original gain and seek expression types.

The second candidate restored the complete original note structure and every
original executable expression, retaining only track/channel storage changes.
Both structures are private behind the unchanged opaque public API.

| Rejected allocation proposal | Full scalar candidate | Track/channel candidate |
| --- | ---: | ---: |
| EE channel / note / track bytes | 8 / 24 / 288 | 8 / 44 / 288 |
| Preceding channel / note / track bytes | 24 / 44 / 296 | 24 / 44 / 296 |
| Proposed saving, 1–32 tracks | 744–992 B | 264–512 B |
| Proposed saving, 105 stock MIDI inputs | 768–952 B per current MIDI | 288–472 B per current MIDI |
| Native-host saving | 736 B | 256 B |

The first EE proposal was verified through actual guarded allocation requests,
including 2832→2088 bytes for one track and 12008→11016 for 32. The second EE
layout was compiled but its separate paired executable was not run after its
actual-engine performance rejection. Neither proposal saves storage for Ogg,
WAV or MP3, and neither is retained.

## Exactness and failure evidence

Each candidate passes **61,092 checks / 11,976,265 stereo frames / 216 files**
on host, and **61,026 / 11,943,497 / 216** under ASan/UBSan. Timing is skipped
under sanitizers. The corpus includes all 105 stock MIDI, 109 stock Ogg and
non-silent mono/stereo MP3 tones. These are bounded render/seek/loop/failure
windows, not full-song exports.

The paired tests compare complete PCM arrays and canaries, public results,
every private MIDI field and all 256 cache bytes per used track, and ordered
input offsets/lengths/counts. A shared old-width canonical structure excludes
only C layout padding; it does not normalize reference code or field values.
Serialization uses a properly aligned local structure and `memcpy` to caller
storage. Complete state is also compared at actual input callbacks.

Coverage includes all 1–32 allocation counts, actual allocation failure and
redzones, both SMF formats, all seven-bit controller values, running status,
channel resets, sustain/release/note stealing, all waveforms/percussion,
resumed attack at 32992, long public seeks and phase advancement, deferred
failures, and oversized callback counts `n+1`, `n+65536` and `SIZE_MAX` plus
short/zero returns. The complete failed state and subsequent successful seek
recovery match.

Both host configurations of each candidate detect PCM-bit and private-phase
negative controls. They also execute and detect five source mutations: discarded bend
bits, discarded envelope bits, truncated callback return count, broken running
status and sixteen-byte underallocation.

The first candidate's real-codec EE proof passes **45,198 checks / 6,357,227
frames**, including selected stock MIDI/Ogg, both MP3 tones and the synthetic
matrices. Its separate-TU MIDI timings regress 0.59–1.57%; they were not used as
production performance evidence. The narrower candidate's real-codec paired
EE executable was compiled only. Its actual complete-engine gate below was
executed with real codec libraries.

The established core, threaded, backend, ring and original-mixer host suites
and both ring negative controls also pass. Their retained results include
50,636,800 threaded/direct frames, 5,944,488 original-mixer frames, 384 native
PCM/cursor cases and twenty million float conversions. These additional suites
ran against the first candidate; source restoration is established separately
by complete file comparison, not by relabeling rejected-source tests as final.

## Actual default-engine performance gate

The decisive fixture uses the **original frozen complete Software engine LTO
object**, SHA256
`a8592311111063376fdc311fb6e61995b1d8fb9efb12d39713bd170b147e836d`.
It never recompiles reference engine functions. Both candidate objects came
from real complete default release builds with the existing feature set,
`-O2`, `-G8` and full engine LTO.

Only symbol metadata in object copies exposes
`PS2_MusicRender.constprop.0`; all **10,491 allocated sections** remain
byte-identical for each object. Separate objcopy rename/globalize steps are
used. The entry still overwrites `a2` with 2048 output bytes at offset four;
the driver calls its specialized **two-argument, 512-frame ABI**. `--wrap=main`
replaces startup. The engine functions and decoder libraries are unchanged by
the fixture.

Every row checks output counts/canaries and matching PCM, public-state and
ordered-read digests, then times 8192 frames in sixteen calls, minimum of five
runs using EE COP0 Count on the standard 32 MiB PCSX2 profile. The preceding
178 rows are supplemented with 45 synthetic MIDI cases: 1/8/24 voices,
sustained notes or nominal 1/16-frame event spacing, and four waveforms plus
percussion. Both candidates complete **223 rows** with matching signatures.

| Group, summed COP0 Count | Original reference | Full scalar | Track/channel |
| --- | ---: | ---: | ---: |
| All 223 rows | 809,394,185 | 808,330,380 | 802,906,571 |
| 172 WAV | 124,756,531 | 123,380,275 | 124,756,531 |
| 3 Ogg | 6,930,153 | 6,930,277 | 6,930,153 |
| 48 MIDI | 677,707,501 | 678,019,828 | 671,219,887 |
| 3 stock MIDI | 23,811,108 | 23,837,178 | 23,590,372 |
| 15 one-frame-event MIDI | 323,623,489 | 323,112,372 | 320,434,055 |

| Reason for rejection | Full scalar | Track/channel |
| --- | ---: | ---: |
| Slower MIDI rows | 29 / 48 | 19 / 48 |
| Worst MIDI cost increase | **1.58780%** | **1.27206%** |
| Worst dense-event cost increase | **0.57906%** | **0.26399%** |

The first worst row is eight sustained sine voices; the second is one
sustained percussion voice. The second candidate's stock MIDI rows improve
0.894–0.949%, but its regressions still reject it. No selected aggregate is a
whole-game FPS measurement or a retained CPU benefit.

Actual full-LTO MIDI read text changes 2432→2520 / 2472 bytes; its frame stays
304 bytes. The scalar version adds hot `andi ...,0xffff` operations after
envelope updates. Restoring full-width notes removes those operations but does
not remove all measured regressions. Across selected MIDI sections, text
changes +64 / −16 bytes; strict separate-TU MIDI text is 8856→8888 / 8856.
Music render and Decode frames stay 112 / 64 bytes. Candidate code costs are
not final-game costs.

The narrower comparison reuses the preceding baseline log only after verifying
**identical complete baseline ELF bytes**, original object, driver, asset
hashes and synthetic cases. The reference ELF SHA256 is
`1b52327a3c189dc5806896a8741d300a3f1856db6f2db134aa763b4d719ab0b8`.

MP3 production timing remains unavailable: the established wrapped full-engine
baseline rejects the generated tone. Both runs use `--skip-mp3`; no startup or
codec code is changed to bypass this limitation. The passing standalone
real-codec MP3 correctness proof remains separate evidence.

## Final production-code restoration

Final source revision `956162266634e1fbd422250bb945bd518a5acbfb` restores the
reference audio sources. A read-only comparison of each final renderer object
against its **own original frozen reference object** closes the complete
Music/MIDI API and defined-helper graph:

| Complete identical final-object contents | Software | Hardware |
| --- | ---: | ---: |
| Code sections | 42 | 42 |
| Ordered code relocations/addends/targets | 215 | 215 |
| Referenced data sections | 7 | 8 |

The specialized two-argument 512-frame entry is unchanged. Both final ELF
hashes, complete section bytes, data and relocation records are pinned in
`audio/final-software-code-binding.json` and
`audio/final-hardware-code-binding.json`. No compiler or emulator is run by
this binding, and rejected timings are not relabeled as final measurements.

```sh
python3 -B tools/ps2/next8_audio_codecheck.py \
  --final-object build/ps2/obj/engine_lto.o --final-elf build/ps2/SRB2.ELF \
  --out build/next8-audio-repro/final-software.json
python3 -B tools/ps2/next8_audio_codecheck.py \
  --measured build/nov15-next8/baseline/hardware/engine_lto.o \
  --final-object build/nov15-next8/hardware/obj/engine_lto.o \
  --final-elf build/nov15-next8/hardware/SRB2.ELF \
  --out build/next8-audio-repro/final-hardware.json
```

## Frozen sources, artifacts and reproduction

`next8_audio_variants.py` reconstructs each rejected source using explicit
transformations of the verified baseline and checks its complete SHA256:

| Variant | Exact MIDI SHA256 |
| --- | --- |
| `scalar` (commented host freeze) | `434984c9a96992c93a7ccf69ddb30c1321336452e6aeacfbfb7bfe84614860e2` |
| `scalar-build` (actual LTO input, comments absent) | `f1cb1e48cf8f574750928a568999d60278bc985eed895c63a8ca3bfad9b2f50d` |
| `channel` | `8c15942d09fe8a31881500981186c0aef9603968ae34e10662c901772b61f57d` |

```sh
source /workspace/.onboarding/environment.sh
python3 -B tools/ps2/prepare_next8_baseline.py
python3 -B tools/ps2/next8_audio_variants.py
python3 -B tools/ps2/next8_audio_check.py \
  --baseline build/nov15-next8/baseline-src --variant channel \
  --out build/next8-audio-repro/host
python3 -B tools/ps2/next8_audio_check.py \
  --baseline build/nov15-next8/baseline-src --variant channel \
  --out build/next8-audio-repro/sanitize --sanitize
python3 -B tools/ps2/next8_audio_check.py \
  --baseline build/nov15-next8/baseline-src --variant channel \
  --out build/next8-audio-repro/mutations --mutations-only
```

Use `--variant scalar` for the first proposal. `--ee-only` instead builds the
real-codec paired EE proof without starting an emulator. Its output is
`OUT/ee/AUDIO8.ELF`; run through `run_pcsx2.py` with `--args='audio-test'`,
`--until 'AUDIO8 DONE'` and a 600-second timeout. No `--variant` tests the
current restored production source by default.

The actual-LTO gate requires the original saved engine objects; rebuilding a
reference does not recreate the historical generated instructions. With those
objects preserved, the narrower gate can be relinked and run separately:

```sh
python3 -B tools/ps2/next8_audio_linked_check.py \
  --out build/next8-audio-repro/linked \
  --baseline-object build/nov15-next8/baseline/software/engine_lto.o \
  --candidate-object build/nov15-next8/audio/measured-narrow-software/engine_lto.o \
  --link-report build/nov15-next8/baseline/software/build-report.json
source /workspace/.onboarding/emulator-env.sh
# Substitute baseline and candidate for KIND, sequentially.
SRB2_PCSX2_SLOTS=1 python3 -B tools/ps2/run_pcsx2.py \
  --elf build/next8-audio-repro/linked/KIND/AUDIO_LINKED8.ELF \
  --log build/next8-audio-repro/linked/KIND/run.log \
  --args='audio-test --skip-mp3' --until 'LINKED8 DONE' --timeout 300
python3 -B tools/ps2/next8_audio_linked_check.py \
  --out build/next8-audio-repro/linked --summarize
```

The first candidate object is preserved in `audio/measured-software`; the
narrower object in `audio/measured-narrow-software`. Full hashes, commands,
source manifests, allocated-section preservation, ELF pins, all row timings
and complete logs remain under `build/nov15-next8/audio`. Compact binding
records are `rejected-scalar-proof.json`, `rejected-channel-proof.json` and
`restored-source.json`; `linked` and `channel-linked` contain the actual gates.
The candidate object hashes are respectively
`0d5ac36d12b501d5bff7bd9751435baf2c8cdf0ae23931408a96fc825bfa86db`
and `13672255010fdcf4997854ea580b9c2ad531e18bda3b98d335f7f14453674ca3`.
