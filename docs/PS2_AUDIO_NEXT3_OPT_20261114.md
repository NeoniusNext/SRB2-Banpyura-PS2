# SFX metadata and inaudible-voice work, third iteration — 2026-11-14

Reference: the complete preceding source pass in
`build/nov14-next3/baseline-src`, including both earlier audio passes. The
comparison uses those actual files and applies **no reference normalization**.
`tools/ps2/prepare_next3_baseline.py` recreates and SHA256-verifies this snapshot
from the frozen `next3_baseline.patch/.json`, independently of the live checkout.

Production changes are confined to `src/ps2/i_sound.c` and
`src/ps2/ps2_audio.c`. Music/MIDI decoding, compressed-effect conversion, source
formats/rates, retained sample values, pitch, pan, gain arithmetic, output rate,
voice limits, threads, rings, pause/fade/loop transport and release fences retain
their existing definitions. No additional persistent buffer or allocation is
introduced. This pass adds no resampling or interpolation change.

## Retained PCM cache bytes

After the existing complete read and PCM validation, `I_GetSfx` moves the exact
parsed PCM range over metadata which mixing no longer needs, then shrinks its
existing zone block. Up to 15 prefix bytes remain: the new PCM offset equals
the old offset modulo 16. The block retains its 64-byte alignment, so all
2/4/8/16-byte PCM alignments, native/MMI eligibility and sample accesses remain
unchanged. DMX headers of eight bytes generally remain; there is no benefit
from moving them when preserving alignment.

The initial allocation, input read, rejection limit and original `sfx->length`
are unchanged. Only the private retained-byte accounting and parsed offset
change. The cache budget remains 2 MiB, with the same active/pending pinning,
LRU/reclaim and release protocol. The source memory is not published to a
playing voice until the move and shrink finish.

All **223 stock PCM effects** occupy 16,621,168 source bytes. Seventy retain
fewer bytes; the other 153 are unchanged. Their combined retained requests
decrease by **27,922 bytes** (individual savings 0–1114 bytes). Executing the
actual allocator on EE releases **28,240 arena bytes** across these separate
allocations; rounding explains the difference. These are **corpus totals**,
not simultaneous free RAM in the 2 MiB cache. The cache descriptor remains
56 bytes on EE and no extra live block is needed for shrinking.

The actual `z_zone.c` + `ps2_mem.c` fixture checks **3685 cases**: 1–64-byte
reductions around block boundaries, large/tiny retained payloads, live
neighbors, owners, red zones, exact free/used accounting and unchanged allocation
peaks. On EE it performs **24,867,470 checks**, verifies the 16-byte zone header
and 56-byte cache descriptor, and measures the corpus release above. Native
host execution performs **24,865,296 checks** with a 72-byte descriptor and
releases 28,144 corpus arena bytes; native and EE measurements are kept
separate. Forcing the allocator to copy instead of shrinking in place makes
the address/owner check fail as required. Production allocator files are
unchanged by this audio pass.

## Zero-gain voices

When both already-calculated integer gains are zero, mixing contributes exactly
zero on both channels. The mixer now advances that voice without fetching its
PCM or multiplying each sample. It still expires at the same source frame and
retains the same fractional cursor and handle state.

Away from EOF, a bounded 64-bit product combines the cursor updates. This path
requires a canonical fraction and a step which cannot overflow the original
32-bit fractional addition. Its computed frame advance must remain strictly
inside the source, preventing frame overflow and early EOF. The final block
and extreme steps use the original scalar cursor rules without PCM work.
Noncanonical fractions continue through the preceding mixer path. No division,
new voice field or persistent cache is introduced.

## Paired checks

The fixture compares actual preceding/candidate mixer functions, complete
output arrays with canaries, and the whole mixer/voice state after each call.
Host execution passes **653,040 checks / 94,238,426 stereo frames**. EE execution
passes **653,040 checks / 94,296,859 frames**. Architecture-dependent C argument
evaluation changes the deterministic random operation sequence between host
and EE; each target compares the same sequence against its own reference.

Coverage includes 8/16-bit mono/stereo, aligned/odd offsets, zero/one/odd/multiple
blocks, 64 concurrent voices, gain rounding to zero, source EOF, fractions at
0/1/32767/32768/65535 and noncanonical values, steps through `UINT32_MAX`, and
interleaved parameter changes. Music uses both distinct and aliased output
buffers. Both host and EE reject deliberate PCM and private-cursor bit flips.

The cache fixture runs the actual `I_GetSfx` from each source tree on **363
files**: all 223 stock PCM effects plus 112 synthetic chunked WAV and 28 DMX
variants. It checks every payload byte, all retained descriptor fields, the
preserved PCM alignment, original length, bounded mixed PCM/state windows,
ordered input reads, allocation failure, initial allocation peak and active
release. Reference/candidate output/state/read digests match for every file.
Deliberate payload and input-read hash mutations are detected. Synthetic WAV
cases include data before format, ancillary chunks, odd padding and the
existing accepted missing-pad-before-LIST convention.

ASan/UBSan pass **652,800 checks / 94,207,706 mixer frames**, all 363 cache
files and the full actual-allocator fixture, with the same negative controls
and no unexpected findings. Sanitized runs skip the 60 timing cases, which
accounts for their smaller check/frame count.

The existing all-host suite also passes: 50,636,800 threaded/direct frames,
5,944,488 original-mixer frames, 384 native/EOF/odd-offset cases, backend
queue/pinning/reclaim/fades, IOP-ring checks and 20 million float conversions.
Both ring negative controls are detected. The previous
`audio_lossless_check.py` still passes **32,915 checks / 8,874,319 frames / 216
real files**, including the isolated original Ogg EOF crash/guard and PCM
negative controls.

## EE timing and code size

Minimum of five runs, each containing 32 resets/renders of 512 frames, on the
shared standard 32 MiB PCSX2 profile. Timed functions include the unchanged
music mix and final clipping; Count values below are totals for all 32 calls.
The first rows show 16-bit mono at native rate; both zero master volume and
zero per-voice volume produce the same measured results.

| Native-rate voices | Preceding COP0 Count | Current COP0 Count | Reduction |
| --- | ---: | ---: | ---: |
| 1, zero gain | 478024 | 285763 | 40.22% |
| 8, zero gain | 1838952 | 301891 | 83.58% |
| 64, zero gain | 12726248 | 430915 | 96.61% |
| 1, audible | 478024 | 478019 | 0.001% |
| 8, audible | 1838952 | 1839939 | −0.054% |
| 64, audible | 12726248 | 12735395 | −0.072% |

Across all four PCM formats, zero-gain reductions are **33.36–40.22%** for one
voice, **79.11–83.58%** for eight and **95.50–96.61%** for 64. Audible native,
fractional-pitch and 2:1 source-rate rows expose the extra zero-gain branch:
the largest measured overhead is **0.09552%**. All 60 rows are retained in
`build/audio-sfx-next3-20261114/ee/run.log` and `timing-report.json`.
These are target-code microbenchmarks, not whole-game FPS or physical-console
cache/audio measurements. The benefit depends on how many active voices have
zero effective gain.

Strict `-O2 -G0 -Wall -Wextra -Werror` EE mixer text changes from 8592 to
8912 bytes (**+320 bytes**); data/BSS remain zero. This is the mixer object,
not a claim about the entire linked game or `i_sound.c`. Full LTO builds and
gameplay validation are measured by the coordinator.

## Reproduction and artifacts

```sh
source /workspace/.onboarding/environment.sh
python3 -B tools/ps2/prepare_next3_baseline.py
python3 -B tools/ps2/audio_sfx_next3_check.py \
  --baseline build/nov14-next3/baseline-src \
  --out build/audio-sfx-next3-20261114 --ee
python3 -B tools/ps2/audio_sfx_next3_check.py \
  --baseline build/nov14-next3/baseline-src \
  --out build/audio-sfx-next3-20261114-sanitize --sanitize
python3 -B /workspace/.onboarding/audio-tests.py all
python3 -B /workspace/.onboarding/audio-tests.py ring-neg
python3 -B tools/ps2/audio_lossless_check.py \
  --assets /workspace/.onboarding/srb2-assets \
  --out build/audio-lossless-after-next3-20261114
```

The new driver accepts `--assets` and `--sdk`. `--ee` compiles the fixtures
without starting PCSX2. Run these sequentially under the shared slot lock:

```sh
source /workspace/.onboarding/emulator-env.sh
python3 -B tools/ps2/run_pcsx2.py \
  --elf build/audio-sfx-next3-20261114/ee/AUDIO_SFX_NEXT3.ELF \
  --log build/audio-sfx-next3-20261114/ee/run.log \
  --until 'SFX3 DONE' --timeout 300
python3 -B tools/ps2/run_pcsx2.py \
  --elf build/audio-sfx-next3-20261114/ee/AUDIO_SFX_NEXT3.ELF \
  --log build/audio-sfx-next3-20261114/ee/negative.log \
  --args='audio-test --negative-control' --until 'sfx-next3:' --timeout 20
python3 -B tools/ps2/run_pcsx2.py \
  --elf build/audio-sfx-next3-20261114/ee/AUDIO_SFX_NEXT3.ELF \
  --log build/audio-sfx-next3-20261114/ee/state-negative.log \
  --args='audio-test --state-negative-control' --until 'sfx-next3:' --timeout 20
python3 -B tools/ps2/run_pcsx2.py \
  --elf build/audio-sfx-next3-20261114/ee/AUDIO_SFX_ZONE_NEXT3.ELF \
  --log build/audio-sfx-next3-20261114/ee/zone-run.log \
  --until 'SFX3 ZONE DONE' --timeout 180
```

The explicit `audio-test` supplies `argv[0]` for this fixture through PCSX2's
`-gameargs`. Both negative logs must contain their intended `!memcmp` failure.
The zone target embeds the stock request sizes and needs no asset filesystem;
its engine/platform boundary uses the existing zone fixture while executing
the actual allocator with EE pointer widths.

Production SHA256:

- `ps2_audio.c`: `f2b0bc02cb63d8a43d54d725a372f51161bb3e6abf984a0c5b513964dc774211`
- `i_sound.c`: `6dd35ef21ba2d5f5b70c310e0a6d5deab4705bedc693e7a74912c6a351b1285d`

Source copies, host/sanitizer reports, strict objects/disassemblies, both target
ELFs and complete target logs remain under the output directories above.
`build/audio-sfx-next3-20261114/final-proof.json` binds the final source hashes,
fixture counts, ELF hashes and negative controls to those logs.
