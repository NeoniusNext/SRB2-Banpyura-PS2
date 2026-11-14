# SRB2 PS2 exact optimizations — seventh pass, 2026-11-14

This pass retains compact resource locations, conditional Vorbis storage,
seven fog/solid-color drawer improvements, two PSMT8 parameter snapshots and
bounded composite recovery. Four GPT-6.1 Sol subagents at max reasoning
handled Software, GS, resources and audio; the coordinator reviewed, built,
integrated and committed the result.

The preceding six passes remain intact. Source reference is
`b1f243f5f768d24aef5cb3ee62b45f2341e53ff4`; final source is
**`2ec90684af655a9286b097224564083c4a007170`**. All **602 reference source
hashes** are reconstructed by `prepare_next7_baseline.py`; exactly eleven
source files differ. The preceding ELFs embed older `7de3fcfc8` build metadata
but have the same reference source. Their original LTO objects were saved
before edits, enabling production-code comparisons without recompiling them.

## Changes and measured scope

| Change | Result | Scope and costs |
| --- | --- | --- |
| Resource locations, 40→36 B per record | **60,208 B less directory payload** across all 15,052 stock entries | Full offsets, paths, names, counts and lifetimes retained; +632 B isolated metadata text |
| Vorbis state only for Ogg | **720 B less per open WAV/MIDI/MP3 song**, one allocation | Ogg allocation stays 4944 B; PCM buffer stays 4096 B/aligned16; no CPU gain claimed |
| Seven fog/solid-color drawers | **28.95163% lower summed EE kernel cost**, 159 faster/9 equal/0 slower rows | Complete default-LTO functions and lighting helper match measured fixture code; +336 B isolated drawer text, unchanged frames/data/BSS |
| Two PSMT8 row callbacks | **26.8144% lower summed separate-TU callback cost**, 168 faster/48 equal/0 slower rows | Actual LTO scalar reloads removed and fast4 kernels preserved; linked helper bodies grow 8 B, frames unchanged |
| Composite final-growth recovery | One bounded rebuild reserves the known final size after releasing unpublished work | Avoids concurrent old/new pixel roots in the eligible recovery path; no live cache purge; extra code and transient stack |

These are component measurements, not weighted game FPS. PSMT8 fixture timing
is not assigned to the differently specialized LTO callbacks: actual fast4
instruction forms and all three PPACB sites remain unchanged. Zero-copy P_8
reference uploads bypass those callbacks. No VRAM/DMA/GPU draw-work reduction,
physical-console timing/power or frame-time variance measurement is claimed.

There is no precision, sample, texture-size, filtering, geometry, physics,
feature, voice/channel/rate, format, Lua/UDMF/add-on/network or compatibility
limit reduction. Default release flags and the complete feature set remain.

## Preventing a texture-quality regression

Smaller directories change allocation placement. The compact-only control
exposed two plain-column substitutions for MAP11's 256×2048 `CERAILT1` under
the established `-keepspawn` pressure trace. Disabling only compact locations
restored the preceding zero-warning cache signature. Matching loader hashes
and ordinary screenshots alone were insufficient to accept that control.

Existing caller diagnostics identified failed **530,432 B final root growth**
while the **524,288 B pixel root** remained live. The final implementation
releases only unpublished composite scratch, then rebuilds at most once with
the measured final capacity reserved initially. Successful ordinary builds
keep their allocation/read sequence. No current-frame cache protection is
weakened and no emergency cache purge or frame abort is introduced.

Recovery is limited to already supported copy-style raw/cooked pack sources
with ready windows/decoder buffers and a stack-sized block index. It uses
nonfatal direct pack reads, rechecks source/header validity, and handles every
retry allocation/read failure through cleanup. Other formats and policies
retain their original primary construction path; these recovery conditions
are not new accepted-resource limits. The actual default
`TEXTURE_255_IS_TRANSPARENT` branch and alternate counted-post branch are both
tested. Final MAP11 stress has **zero substitutions**, and its complete
175th-frame image equals the preceding version exactly.

## Complete builds and artifacts

EE GCC 15.2, existing release `-O2`, `-G8`, full engine LTO, SDK/libraries and
feature configuration. Both final builds link and pass their diagnostic
gates. Final input hashes match the eleven source files; source revision
`2ec90684a` is embedded in both tested binaries.

| Linked text+data+BSS | Preceding | Final | Change |
| --- | ---: | ---: | ---: |
| Software | 6,682,985 B | 6,686,225 B | +3240 B |
| Hardware | 7,882,676 B | 7,886,004 B | +3328 B |

These section totals include metadata and linker layout, not total process
RAM. Software data/BSS stay equal; Hardware BSS grows 8 B. Actual final
`R_GenerateTexture` grows 4836→6428 B and its frame 224→240 B. Recovery helpers
use 48/64 B frames; the read-only pack query is 176 B/frame0. The complete
non-LTO texture module grows 1936 B with unchanged data/BSS. The measured
maximum main stack remains 132,560 of the existing 393,216 B reservation.

[Software/Hardware ELFs, SHA256SUMS and validation metadata](../prebuilt/lossless-next7-20261114/README.md)
are copied byte-for-byte from the final tested builds. Earlier releases remain.

## Exactness and failure checks

- [Resources](PS2_LUMPLOCATION_MEMORY_NEXT7_20261114.md): 96.1 million real-zone
  checks, ASan/UBSan, five negative controls, actual standalone/WAD/ZWAD/ZIP/
  folder constructors, lazy positions, dynamic file changes, allocation
  failures, growth and shutdown. All 15,052 stock records and 300,940 partial
  reads match. EE ABI is 40/36/40 B for baseline/current/old switch; the
  non-PS2 interface remains unchanged.
- [Audio](PS2_AUDIO_NEXT7_OPT_20261114.md): host 73,239 checks / 17,080,403
  frames / 227 inputs; real-codec EE 47,138 checks / 6,914,562 frames;
  sanitizers, initialization/alignment/bounds, cleanup/failure and four
  negative controls. Exact production-code gate matches all 178 PCM/state/read
  rows; all 172 WAV timings are identical. Codec differences are within measured
  repeat noise. MP3 timing is unavailable under wrapped startup because the
  frozen baseline rejects that tone; separate MP3 correctness proofs pass.
- [Drawing](PS2_SW_NEXT7_OPT_20261114.md): four policies each pass 68,886 cases
  and 4,679,618,952 framebuffer/canary bytes, seven live content-alias modes,
  sanitizers and 16 mutations. Full final default-LTO instruction/relocation
  streams, lighting helper and float literal bytes match the timed fixture.
- [GS](PS2_GS_NEXT7_20261114.md): host/sanitizer each pass 963,380 cases /
  414,572,750 pixels; EE 192,851 cases / 6,480,229 pixels, five mutations and
  all 216 timings. Final pinned LTO checker verifies fast kernels, stores,
  frames and removed context reloads, with actual +8 B code cost recorded.
- [Composite recovery](PS2_TEXTURE_RETRY_NEXT7_20261114.md): 135 default and
  135 counted real-zone cases, literal pixels/posts, natural fragmentation,
  all retry allocation/read failures, ASan/UBSan and ten executed mutations.
  EE32 passes all 135 rows, **6,982,841 literal bytes**, actual 12-byte
  columns/posts and a natural two-attempt 530,432-byte recovery. Independent
  resource-agent review approves ownership, cleanup, counters and bounds.
  Readiness-query normal/sanitizer suites pass 392 checks and three mutations.

## Integrated game output and RAM

Eleven selected runs complete on standard **32 MiB EE RAM**: six fresh final
runs, three fresh reference runs and two verified historical reference
runs. Every paired loader hash and all **13 complete PPM pairs** match exactly,
with no masking or tolerance. All final runs have no warnings, fatal/heap
errors, reported audio underruns or decoded/consumed hash mismatches.

| Paired scenario | Frames | Renderer |
| --- | ---: | --- |
| MAP01 combined NPOT/fog/solid/water scene, MIDI, stereo SFX, snapshots | 3 | Software |
| MAP11 ordinary single-player, digital music | 3 | Software |
| MAP01 same combined scene | 3 | Native Hardware |
| MAP02 ordinary single-player, digital music | 3 | Native Hardware |
| MAP11 snapshots/cache pressure, capture only at tic175 | 1 | Software |

Both Hardware pairs prove native startup without Software fallback.
`-singletics` sets the seed before loading. The new scene configures 49 fog,
49 translucent and 49 ripple-water FOF planes, 14 sloped FOFs and 156/59 base
floor/ceiling slopes with a 1×1 texture, while retaining the earlier eight
63×45 sloped floor sprites. These are configured counts, not visibility or
all-drawer invocation claims. Lua getter/setter semantics and fixture bytes
are audited. The pressure image is captured after rendering tic175, with no
earlier screenshot-buffer allocations.

| Map-loaded used zone | Preceding | Final | Difference |
| --- | ---: | ---: | ---: |
| Software MAP01 combined scene | 6,133,472 | 6,073,616 | −59,856 B |
| Software MAP11 default | 15,390,240 | 15,330,208 | −60,032 B |
| Hardware MAP01 combined scene | 8,042,768 | 7,982,640 | −60,128 B |
| Hardware MAP02 default | 8,641,152 | 8,581,184 | −59,968 B |

The exact directory payload saving is 60,208 B; whole-zone differences include
rounding/layout. Song heap requests are separate. These overlapping checkpoints
must not be added together or multiplied by the song/archive count.

The separate unchanged-arguments MAP11 `-keepspawn -zheap 1 -zstack -zquit 175`
trace completes with zero texture substitutions:

| Metric | Preceding | Final |
| --- | ---: | ---: |
| Arena capacity | 24,748,032 | 24,743,936 |
| Final used zone | 23,971,120 | 23,905,872 |
| Final free arena | 776,912 | 838,064 |
| Final largest free block | 376,064 | 530,544 |
| Peak used zone | 24,737,376 | 24,732,368 |
| Evictions | 11,040 | 10,703 |
| Failed allocation attempts | 1090 | 1168 |
| Main stack high-water | 132,560 | 132,560 |

Failed attempts include safely recovered allocations; they are not counts of
lost frames or downgraded textures. Cache placement is sensitive to layout;
earlier experimental/probe metrics are not substituted for this final trace.
No universal peak/free-space or whole-game FPS percentage follows from it.

## Standard GS and reproduction

All **17 CT32 readback images** equal the reference. Standard CT32 remains
**55/56**, CT16S **50/51**: the historical VU0 result is unchanged, with 24,625
outcode differences among 49,152 vertices and zero XYZ/reciprocal-W error.
Nonzero full-suite returns remain; the full suite is not declared all green.

With the configured SDK/emulator from [TOOLCHAIN.md](TOOLCHAIN.md):

```sh
python3 -B tools/ps2/prepare_next7_baseline.py
python3 -B tools/ps2/build.py --jobs 2
SRB2_PS2_HW=1 SRB2_PS2_OUT=build/nov14-next7/hardware \
  python3 -B tools/ps2/build.py --jobs 2
python3 -B tools/ps2/next7_integration_check.py --out build/next7-repro \
  --baseline-sw build/nov14-next7/baseline/software/SRB2.ELF \
  --baseline-hw build/nov14-next7/baseline/hardware/SRB2.ELF
```

Supply preceding-source ELFs and user-provided cooked packs at `build/pak`.
Reference source can be built in the isolated reconstruction using its own
`build.py`; source reconstruction does not promise identical historical SDK
or Git metadata. The checker generates deterministic fixtures if absent,
requires a fresh output directory and rejects texture warnings in the stress
run. `--analyze-only` verifies staged binaries/arguments/fixtures, complete
images and logs. Component reports contain their own reproduction commands.

Published `validation.json` records source/binary hashes, frame hashes,
measurements and limits. Raw source freezes, controls, boot logs and detailed
reports remain under `build/nov14-next7`; the compact-only failure and inactive
probe are preserved separately. No physical PS2 FPS/stability/power measurement
is claimed.
