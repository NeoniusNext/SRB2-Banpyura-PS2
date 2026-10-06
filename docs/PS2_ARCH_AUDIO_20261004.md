# Exact R5900 MMI mixer — 2026-10-04

The pre-existing dirty working tree was preserved. Only `src/ps2/ps2_audio.c`
and the new `tools/ps2/oct4_audio_bench.c/.py` were changed by this agent.
Pre-edit sources are under `build/ps2-arch-audio-20261004/`; the baseline is
the previous production mixer, including its native-rate and gain optimizations.
The shared build scripts, registry, renderer, physics, IOP and codec code were
not edited.

## Implementation

- EE native-rate playback mixes four stereo output frames per MMI group.
  `PMULTH` multiplies eight signed halfwords; `PMFLO`, `PMFHI`, `PCPYLD` and
  `PCPYUD` restore the original sample order into two sets of signed words.
  Unsigned 8-bit samples are widened and centered at 128. Mono is duplicated
  before multiplication by the alternating left/right gains.
- Six constant-format voice functions keep hot scalar loops separate from the
  main renderer's register allocation. An initial clipping-only experiment
  caused significant register spills; that implementation was rejected.
- 16-bit products divide by 256 with exactly the old truncation toward zero:
  packed negative products receive a bias of 255 before arithmetic shift.
  Gain outside 0..256 uses the scalar path. Valid products are bounded by
  `32768 * 256 = 8388608`, so no multiply overflows a signed 32-bit word.
  A valid 64-voice accumulator plus music is bounded by 2129920.
- The complete 32-bit accumulator is clipped at the end using `PMAXW/PMINW`,
  then `PPACH` packs eight halfwords. No voice is saturated early.
- Source alignment is checked for each native voice: 4/8 bytes for 8-bit
  mono/stereo, 8/16 bytes for 16-bit mono/stereo. Output packing requires 16-byte
  alignment. Partial groups, odd data offsets and unaligned output retain
  scalar code. Loads/stores never cross the bounded group or output length.
- `_EE` selects the MMI code. `PS2_NOOPT_AUDIO_MMI` retains the preceding scalar
  implementation for comparison. There is no new allocation or persistent
  buffer. The 4096-byte accumulator is explicitly aligned to 16 bytes.

Instruction lane semantics were cross-checked against the primary implementation
in [PCSX2 MMI.cpp](https://github.com/PCSX2/pcsx2/blob/master/pcsx2/MMI.cpp).
Actual candidate instructions were then compiled and executed on the EE emulator;
the measured equality checks below are the relevant correctness evidence.

Sample rate (22050 Hz), channel count (64), source bytes, pitch, pan, music gain,
cursor updates, EOF, clipping order and in-place music behavior are unchanged.
No new approximation or reduction in sound quality was introduced.

## Executed verification

```
$env:SRB2_PS2_RELEASE = '1'
$env:SRB2_PS2_OUT = 'build/ps2-arch-audio-20261004/final-verified'
python -B tools/ps2/oct4_audio_bench.py --baseline build/ps2-arch-audio-20261004/audio-before.c --out build/ps2-arch-audio-20261004/final-verified --ee
```

Host and EE each report `AM DONE checks=6080 failures=0`:
2048 repeated format/rate/voice cells and 4032 edge checks compare every PCM
byte, the entire mixer/voice state and untouched output prefix/suffix. Edge
coverage includes lengths 0..1025, tails and multi-block calls, all seven tested
gain cases (including >32768), PCM offsets 0/1/2, eight output alignments,
NULL/separate/in-place music, 64 full-gain clipping voices, zero master volume,
native/fractional/arbitrary steps and source EOF. Parameter comparisons also
cover rates 11025/22050/44100/8000/96000 and pitch 0/1/93/128/255/UINT32_MAX.
The host and EE negative controls each alter one result sample and are rejected
with exactly one failure. EE argument parsing includes argv[0], since the
emulator supplies the first argument there to this standalone ELF.

COP0 cycles per 512 stereo frames, averages of 64 repeats:

| PCM | Rate | Voices | Baseline | Candidate | Cycle reduction |
|---|---|---:|---:|---:|---:|
| 8-bit mono | native | 1 | 52370 | 30210 | 42.31% |
| 8-bit mono | native | 16 | 199450 | 100193 | 49.76% |
| 8-bit mono | native | 64 | 671087 | 324137 | 51.70% |
| 8-bit stereo | native | 64 | 769534 | 307808 | 60.00% |
| 16-bit mono | native | 64 | 933969 | 414377 | 55.63% |
| 16-bit stereo | native | 64 | 999151 | 397865 | 60.18% |
| 8-bit mono | pitched | 64 | 867122 | 884329 | **-1.98% (slower)** |
| 8-bit stereo | pitched | 64 | 1031024 | 982744 | 4.68% |
| 16-bit mono | pitched | 64 | 1194818 | 1147360 | 3.97% |
| 16-bit stereo | pitched | 64 | 1260505 | 1212848 | 3.78% |

All 32 cells and ELF/source SHA256 values are saved in
`build/ps2-arch-audio-20261004/final-verified/ee/report.json`. The parameter
benchmark observes one extra emulated cycle per call in the candidate despite
unchanged source for that function (8192 cycles over 8192 calls); no parameter
speed improvement is claimed.

Additional checks, all in the private output tree:

```
$env:SRB2_PS2_OPT_OUT = 'D:/Ai-Project3/SRB2-PS2-Port/build/ps2-arch-audio-20261004'
python -B tools/ps2/audio_equiv_check.py
python -B tools/ps2/audio_equiv_check.py --negative-control
python -B -c "import sys; from pathlib import Path; sys.path.insert(0,'tools/ps2'); import audio_check; audio_check.OUT=Path('D:/Ai-Project3/SRB2-PS2-Port/build/ps2-arch-audio-20261004/audio-check'); sys.exit(audio_check.main())"
```

Results: 5944488 stereo frames identical to the independent original loop;
384 native/EOF/odd-offset state comparisons; 20000000 float conversion values;
WAV loop/copy/EOF PASS; corrupted PCM negative control rejected. Production
backend tests pass pending short/zero RPC suffixes, bounded queue/cache,
decoder continuity, active pinning, pause, fades and cleanup. Twelve strict
`-Werror` EE syntax checks pass with codecs enabled/disabled, and the real
audsrv/Vorbis/Ogg/mpg123/MIDI smoke ELF links. The latter smoke ELF was not run.
All emulator executions use `run_pcsx2.py` and its shared lock.

Release EE object sizes: text **5032 -> 6640 bytes (+1608)**, data/BSS
**0 -> 0**. The renderer stack frame remains 4208 bytes; deepest native helper
calls add up to 64 bytes of transient stack. There is no new zone/libc allocation
or VRAM usage. Disassembly and size output are retained in the private tree.

Final candidate source SHA256:
`7c12298ba05cda89c16ddfb8fab1199f173068a2635e663c0aa572ffbd5471f8`.
Benchmark ELF SHA256:
`4bfeefd4073284e68c6f48061e0305cb2d4f0de666f357d6d0307d6227cc814d`.

## Limits and registry handoff

The measured reductions are for the mixer, not complete game FPS. Source/PCM
alignment, native versus pitched playback and simultaneous voice count determine
the gain. Worst measured pitched 8-bit mono/64 voices regresses by 1.98%; source
alignment fallback and physical-console timing are not claimed faster. No audible
output, physical EE cache/dual-issue timing, IOP/SPU2 offload or codec speed gain
was verified. Existing audsrv RPC and cache synchronization remain intact.
Integrated retail-memory/golden/FPS verification belongs to the coordinator.

No registry row was written by this agent. Proposed addition: exact R5900 MMI
native PCM mixing and final saturation; 64-voice native mixer COP0 cycles decrease
51.70..60.18% in the measured four formats, pitched mono 8-bit worst case +1.98%,
6080 host/EE byte/state/canary comparisons and negative controls pass, text +1608 B,
data/BSS unchanged. Report: `docs/PS2_ARCH_AUDIO_20261004.md`.
