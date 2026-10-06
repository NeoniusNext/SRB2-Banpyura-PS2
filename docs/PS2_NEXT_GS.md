# Software GS/system continuation — 2026-10-03

The GS software presentation backend already has persistent GIF packets and CLUT change detection from `docs/PS2_GS_SOFTWARE_CONTINUATION.md`. This continuation preserves it and reduces audio CPU/static RAM costs in the same software-frame pipeline. It does not change source/output resolution, GS texture/filtering/VRAM layout, physics, game logic, sample rate, resampling or audio contents.

## Changes

- `src/ps2/ps2_audio.c`: unsigned 8-bit sample conversion multiplies by 256, while mixing divides by 256. Cancel those factors exactly before the per-sample multiply, including negative samples. The signed 16-bit truncation remains unchanged.
- Exact mixer cursor steps for source rates 11025, 22050 and 44100 Hz use shifts instead of the EE software 64-bit division. Other rates use the existing formula. Pitch 0 retains its previous pitch-1 behavior; UINT32_MAX pitch also matches the previous formula for these three rates.
- Zero music gain initializes the accumulator to zero without reading/scaling music. Music decoding still advances normally.
- `src/ps2/i_sound.c`: decode music into the existing aligned output buffer, then mix in place. Each entire mixer block is read into its accumulator before any output write. Pending/short RPC writes keep the same buffer/suffix and never decode twice. Remove the redundant 2048-byte static `music_pcm` array; no replacement allocation is added.
- `src/ps2/ps2_audio.h`: document supported equal music/output pointers.
- `tools/ps2/audio_equiv_hosttest.c`: compare in-place output with the independent reference across random multi-block lengths, gain 0, odd source offsets, voice EOF and the full cursor state.
- `tools/ps2/audio_mix_bench.c/.py`: benchmark and compare parameter steps at common/exceptional rates and pitch extremes, in addition to the existing 2048 mixer comparisons.

No further GS code change was made. Broad `FlushCache(0)` and DMA/FINISH lifetime waits remain intact. PCSX2 cannot justify a physical-EE cache-coherency shortcut.

## Executed verification

All EE measurements use `SRB2_PS2_RELEASE=1`, a private `build/ps2-next-gs` tree and `tools/ps2/run_pcsx2.py` through the benchmark wrapper/shared lock. The baseline is the pre-edit dirty working-tree mixer copied to `build/ps2-next-gs/audio-before.c`; it includes the earlier native-rate mixer optimization.

```
$env:SRB2_PS2_RELEASE = '1'
$env:SRB2_PS2_OUT = 'build/ps2-next-gs/mix-final'
python -B tools/ps2/audio_mix_bench.py --baseline build/ps2-next-gs/audio-before.c --out build/ps2-next-gs/mix-final --ee
```

EE COP0 cycles, mean per 512 output stereo frames (64 repeats per cell):

| Mixer input | Voices | Baseline | Candidate | Reduction |
|---|---:|---:|---:|---:|
| 8-bit mono, native rate | 1 | 54,415 | 52,369 | 3.76% |
| 8-bit mono, native rate | 16 | 232,215 | 199,394 | 14.13% |
| 8-bit mono, native rate | 64 | 802,132 | 671,118 | 16.33% |
| 8-bit stereo, native rate | 64 | 933,403 | 769,621 | 17.55% |
| 8-bit mono, pitched | 64 | 998,291 | 867,054 | 13.15% |
| 8-bit stereo, pitched | 64 | 1,194,855 | 1,030,959 | 13.72% |
| 16-bit stereo, native rate | 64 | 999,058 | 999,164 | Within 0.02% timing noise |

Each cell compares exact PCM and the entire voice/mixer state. Result: `AM DONE checks=2048 failures=0`. All 32 measured format/rate/voice cells are in `build/ps2-next-gs/mix-final/ee/report.json` and `run.log`.

EE COP0 cycles for **8192 parameter calls**, including loop/call overhead:

| Source Hz | Baseline | Candidate | Change |
|---|---:|---:|---:|
| 11025 | 1,351,683 | 385,028 | -71.51% |
| 22050 | 1,351,683 | 360,452 | -73.33% |
| 44100 | 1,351,683 | 409,604 | -69.70% |
| 8000 | 1,351,683 | 1,410,013 | +4.32% |
| 96000 | 1,351,683 | 1,409,028 | +4.24% |

Thirty extra parameter edge comparisons and the final bulk-call states also match. The fallback costs about seven additional emulated cycles per call. A direct stock `srb2.pk3` header census found 153 DMX, 70 WAV, 514 Ogg effects; DMX/WAV source rates are `{11025:12,16000:2,22050:48,32000:2,44053:6,44100:152,48000:1}`. Existing Ogg effects decode to 22050 or 11025 Hz. Thus **726/737 stock effects** use a shortcut rate; none are converted to another rate by this change.

Host verification:

```
$env:SRB2_PS2_OPT_OUT = 'D:/Ai-Project3/SRB2-PS2-Port/build/ps2-next-gs'
python -B tools/ps2/audio_equiv_check.py
python -B tools/ps2/audio_equiv_check.py --negative-control
```

Results: **5,944,488 stereo frames** identical to the independent original loop, including in-place mixing and cursor/active-state equality; **384** native-rate/64-voice/EOF/fraction/odd-offset comparisons; **20,000,000** existing float-conversion comparisons; WAV loop/copy/EOF checks pass. The negative control changes one candidate sample and fails the comparison as required.

`audio_check.py` was imported with only its `OUT` overridden to `build/ps2-next-gs/audio-check` to avoid its fixed shared output directory; its actual host/EE test logic is unchanged. `--host-only` passes PCM, music, MIDI and production backend tests, including queue limits, short/zero writes, decoder position continuity, pause, reentrant fades, cache pinning and cleanup. `--ee-only` passes **12** `-Werror` EE syntax checks with codecs enabled/disabled and links the real audsrv/Vorbis/Ogg/mpg123/MIDI smoke ELF.

Release EE `size` of isolated baseline/candidate objects, saved in `build/ps2-next-gs/size/size.txt`:

| Object | text before→after | data | bss before→after |
|---|---:|---:|---:|
| `ps2_audio.o` | 5,008→5,032 | 0 | 0→0 |
| `i_sound.o` | 7,448→7,448 | 6 | 5,966→3,918 |

Net static RAM saving: **2048 bytes**, text increase **24 bytes**. Stack accumulator and runtime allocation counts are unchanged. No VRAM saving is claimed.

## Limits and integration

The measured percentages apply to mixer/parameter work, **not the full game frame**. The 16-bit mixer, compressed codec cost, synchronous audsrv RPC, software rasterization and mandatory map structures remain separate bottlenecks. Muted-gain shortcut is equivalence-tested but has no separate COP0 timing claim.

No new complete-game before/after FPS/map-memory run was performed by this agent; the coordinator must build the integrated final tree and verify it. No physical-console audio, audible output, physical D-cache timing, GPU-load reduction or universal 32 MiB/map guarantee is claimed. The portable mixer/production boundary tests and isolated EE benchmark establish exact audio semantics and instruction-cost changes.

No shared registry/build/core renderer files were edited. Proposed registry addition/extension for the coordinator: PS2 audio uses exact 8-bit gain cancellation and common-rate step shifts, plus in-place music mixing; PCM/cursors unchanged, BSS -2048 B, release EE mixer improvements up to 17.55% in the measured 64-voice cells, common-rate parameter calls -69.70..73.33%, rare-rate fallback +about 7 cycles/call. Report: `docs/PS2_NEXT_GS.md`.
