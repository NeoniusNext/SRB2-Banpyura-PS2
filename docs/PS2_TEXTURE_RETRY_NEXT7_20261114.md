# Lossless composite recovery — seventh pass, 2026-11-14

The compact lump-location candidate exposed a real quality regression in the
32 MiB Software MAP11 `-keepspawn` stress run: texture 1530, CERAILT1, fell back
to a plain column twice. The immediately preceding completed pass had no such
warnings. This candidate was rejected as a final result. The recovery below
addresses the failing allocation without releasing live renderer caches or
changing the texture's pixels, posts, dimensions, sampling or source formats.

The final production revision is `2ec90684af655a9286b097224564083c4a007170`.
Its Software ELF SHA256 is
`dbc708685926cf5f8368f3a29f5846a1f309bb25b9c5c3f79ab9dbd0cb267767`;
Hardware is
`9258483fa3fc655d035d9084286d339e309b6e6fa6132e5418eb4e16c17505d3`.
The frozen comparison source remains `build/nov14-next7/baseline-src/src`,
immediately before this seventh pass. The rejected compact-only Software ELF
is retained under `build/nov14-next7/compact-only-candidate/final/software`,
SHA256 `80cee005c73dbe32559a01632d0b32cad24baf8d28c44a9078f2b66eb2164ec5`.

## Failure and change

The same failing ELF with its existing allocation diagnostics confirmed the
sequence twice: resize to 530,432 B, attempt a second 530,432 B allocation,
release the old 524,304 B allocator block, then plain-column fallback.
CERAILT1 contains eight 256×256 CESTONE patches stacked vertically and four
64×64 CERAIL overlays. Its exact final EE32 layout is 524,288 pixel bytes,
3,072 column bytes and 3,072 post bytes. The original grow may require both the
old pixel root and a new final root at once, although the final root alone fits.
Evidence: `isolation-runs/location-textrace/boot.txt` under `build/nov14-next7`.
An old-location-only build also removed the warnings, tying the regression to
the changed heap topology rather than a changed texture or drawing expression.

After an eligible composite's first final grow fails, `R_GenerateTexture`
releases all of that attempt's unpublished local work and rebuilds **once**,
reserving the known final size as the initial pixel-root capacity. Its final
resize can then keep the same allocation. There is no persistent retry state,
recursive call, live-cache purge, changed allocator policy or early publication
of the new texture owner. The normal successful path keeps its exact original
allocation sizes and order. It gains eligibility checks and conditional
branches; no zero-overhead or whole-game FPS claim is made.

Every retry allocation is a nonfatal Try allocation, including small masks,
columns, post counts and descriptors. Failed scratch or source reads release
only the local unpublished roots and take the existing fallback path. The old
fallback's allocation and diagnostic behavior remain unchanged. The retry is
bounded even if its final resize also fails. Mask/post lifetimes, all work
pointers, total posts and counted-post policy state are reset. Final publication
still sets the owner and then makes the completed root purgeable. The texture
memory counter retains one original charge during restart and replaces it once
if restart reaches final accounting; it never charges both attempts.

## Source and policy coverage

Actual `r_defs.h` defines `TEXTURE_255_IS_TRANSPARENT`, so the production default
uses that descriptor branch, **not** `R_COUNTED_POSTS`. Both branches are
implemented and tested. Optional recovery requires the existing packed-mask,
texture-reuse and source-stream policies, and is disabled by
`PS2_NOOPT_TEXRETRY`. Other policy paths retain their previous behavior.

Recovery is deliberately restricted to non-flat, copy-style composites whose
sources can be rebuilt without an unchecked converter or reader allocation:
raw Doom or cooked Doom records in cooked packs, with RAW/LZ4 records at most
64 pack blocks. This is an eligibility bound on the added recovery, not a new
texture, add-on, format or engine limit. Existing PNG, flat, blended and other
source paths remain available with their original successful and failure
behavior.

Source bounds are checked before dereferencing wad/lump metadata. Format and
eligibility are checked again on the second pass, including full raw-column
bounds before use. A cached first-pass patch may disappear before restart;
eligibility follows the source bytes, and a retry cache miss streams those
same bytes. A changed PNG, corrupt source, unsupported codec or short read
returns to fallback before any unchecked constructor can run.

`WPack_CanReadWithoutAlloc` performs a read-only readiness check with
`StreamFind`, never `StreamFor`. It requires the existing stream window and
decoder buffers and rejects a decoder index that would need the heap. This
also handles stream-slot turnover across more than six pack handles. Recovery
calls `WPack_ReadLumpN` directly, so failed reads return short instead of
entering the generic fatal wrapper. It does not change global soft-read mode.
There is no callback or source-format conversion added between the checked
read and its use. Original normal-path callbacks remain in their original
order; recovery is enabled only for the audited bounded source path.

## Proof

`next7_texture_retry_hosttest.{c,py}` extracts the complete real generator and
patch code and runs them with the real arena allocator. Boundary platform and
pack-I/O services are controlled; the separate real WPack readiness fixture
tests the actual reader state. The fixture reads the actual nine CERAILT1
source lumps and also constructs odd, tiny, fully transparent and overlapping
hole-pattern patches. Its default policy comes directly from `r_defs.h`;
counted-post coverage explicitly selects the alternate policy.

Both default and counted policies pass **135 rows each** for frozen baseline,
candidate and opt-out, plus candidate ASan/UBSan. Successful recovery compares
the literal normalized texture bytes, every column's post count and every
post field against the frozen roomy output: **6,982,841 B default** and
**7,337,538 B counted**. An independent Python Doom compositor additionally
checks the actual CERAILT1 pixels/posts. All normal-success allocation-event
sequences, counters and outputs agree; the opt-out agrees on every row.

The matrix covers first and repeated final-grow failure, all 17 ordinary
retry allocations, all 24 header/full-source read positions, changed source
format/codec/readiness, and the scratch/read stages of the small/transparent
cases. Raw and cooked cached patches are explicitly evicted after the first
failed grow and then reconstructed by streaming. Four live current-frame cache
canaries survive each row. Every row checks the real zone structure,
owner/pinning/lock state, removal of all unpublished render work, and an empty
zone after cleanup. Four invalid/stale source IDs per policy retain the
original validation diagnostic rather than dereferencing invalid metadata.

All **five executed negative controls per policy** are detected: remove the
retry bound, reserve only the pixel capacity, duplicate the accounting charge,
omit a required pointer reset, and change a raw post's output. Each mutated
actual function builds and runs; a compiler failure is not accepted as a
negative-control success. Reports:
`build/nov14-next7/texture-retry-host-{default,counted}/report.json`.

The actual **EE32 witness** also passes all **135 rows**, with 12-byte columns
and posts, **56,260 checks**, and the same **6,982,841 literal compared bytes**.
Every status/hash field agrees with the host proof and every memory charge
agrees with the actual EE32 layout. A real fragmented arena, with no injected
failure and four protected cache blocks, makes the frozen/opt-out construction
fall back but lets the candidate publish the exact 530,432 B result in two
attempts. It exercises all retry fault positions on the target as well.
ELF SHA256:
`5b8ea38961027bf0ebdec70d17d0065bc069ec2dcf96fc0f87947b9e3be0a7a8`.
Evidence: `build/nov14-next7/texture-retry-ee/{inputs,report}.json`, complete
`run.log` and exit-zero `runner.log`.

The witness uses actual EE32 generator/allocator code with controlled platform
and pack-read boundaries; it is separate from complete-engine integration.
The actual WPack readiness fixture passes 392 checks under normal and sanitizer
builds plus three negative controls, including handle turnover. Independent
ownership/readiness review is signed in
`build/nov14-next7/memory-retry-helper/independent-review.json` and bound to the
three final production hashes below.

## Costs and integration

Complete actual `r_textures.c` EE objects use release flags and `-Werror`,
without LTO only for section inspection. Both controls use the same current
shared headers, isolating recovery from the earlier metadata-layout change.

| Object quantity | Before | After | Change |
| --- | ---: | ---: | ---: |
| Complete texture module text | 20,968 B | 22,904 B | +1,936 B |
| Texture module rodata/data/BSS | 3,792/28/44 B | 3,792/28/44 B | 0 |
| Generator frame | 240 B | 256 B | +16 B |
| Readiness helper text/frame | absent | 176/0 B | +176/0 B |

`PS2_NOOPT_TEXRETRY` restores every complete module code/rodata/storage section
byte-for-byte: framed digest
`0f23cd163363189042a304fb362a6ef60da34f0e0e53aa1f666a93e9690b462b`.
Object evidence: `build/nov14-next7/texture-retry-objects/report.json`.

Actual final default-LTO code differs from the inspectable object, so it is
audited separately. Relative to both frozen baseline and compact-only ELF,
the linked generator grows from 4,836 to 6,428 B; its frame grows from 224 to
240 B. New linked retry-read and Try-lump helpers use 292/48 B and 260/64 B
text/frame respectively; the readiness helper uses 176/0 B. The existing
Try-read helper remains 204/48 B. These are individual frames, not a bound on
the complete thread's maximum stack. The final ELF file is 2,556 B larger than
the compact-only ELF. No static heap table or permanent cache is added.
Evidence: `build/nov14-next7/texture-retry-final-linked/report.json`.

The active probe completed all 175 MAP11 `-keepspawn` frames with **zero plain
column warnings**, used 23,600,048 B and retained a 542,640 B largest free run.
Its 6,661 evictions compare with 11,040 in the preceding pass's same stress.
This probe is retained as a distinct measurement, not silently rebound to a
later source: ELF
`23931789c1d6c7e65a89eed08ad38c5588ca425a45dd7adc8ab16c2be8417ec8`,
generator source
`975246359c29f7a4a80c354eb8287ae583cdffb3fcdeb8db221c700d25eea81b`.
Later opt-out-token and invalid-ID guards are covered by the final host/EE
proof. Probe logs are in
`build/nov14-next7/isolation-runs/retry-probe-map11-spawn-175`.
The coordinator's final complete-engine stress, matched pressure-frame captures,
load hashes and Software/Hardware checks remain the integration acceptance
record. Ordinary no-keepspawn screenshots alone do not establish this recovery.

Final production SHA256:

- `src/r_textures.c`: `32e41d258c3892894f5a4eb017b17ab5a05e63092f8802bf2df99bad5a2f323d`
- `src/w_pack.c`: `94532c49e6a1886677099849ce96ecd5e804eb2facd6a05c1732660443ab2664`
- `src/w_pack.h`: `e76426bb03e6a0798661f2ecbe6f76b01c7143170b20336ce4fe794e9a1085da`

## Reproduce

```bash
python3 -B tools/ps2/next7_texture_retry_hosttest.py \
  --base-src build/nov14-next7/baseline-src/src \
  --out build/nov14-next7/recheck-retry-default --sanitize --negative-controls
python3 -B tools/ps2/next7_texture_retry_hosttest.py \
  --base-src build/nov14-next7/baseline-src/src \
  --out build/nov14-next7/recheck-retry-counted --counted --sanitize --negative-controls
source /workspace/.onboarding/environment.sh
python3 -B tools/ps2/next7_texture_retry_objectcheck.py \
  --base-file build/nov14-next7/sw-fog-source/r_textures-before-retry.c \
  --out build/nov14-next7/recheck-retry-objects
python3 -B tools/ps2/next7_texture_retry_eetest.py \
  --host-proof build/nov14-next7/recheck-retry-default \
  --out build/nov14-next7/recheck-retry-ee
source /workspace/.onboarding/emulator-env.sh
python3 -B tools/ps2/run_pcsx2.py \
  --elf build/nov14-next7/recheck-retry-ee/RETRY.ELF \
  --log build/nov14-next7/recheck-retry-ee/run.log \
  --until 'RETRY DONE' --marker RETRY --timeout 240 \
  > build/nov14-next7/recheck-retry-ee/runner.log 2>&1
python3 -B tools/ps2/next7_texture_retry_eereport.py \
  --host-proof build/nov14-next7/recheck-retry-default \
  --out build/nov14-next7/recheck-retry-ee
```
