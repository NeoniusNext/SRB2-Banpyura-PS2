# OPT17: exact compact CPU patch cache

This candidate replaces the ordinary patch upload cache's power-of-two AP88
allocation with a tight index plane and a coverage bit plane. Logical texture
format, dimensions, coordinates and native GS format decisions stay unchanged.
Canonical source columns/posts/pixels remain pinned for existing consumers and
purge recovery. There is no new CLUT or persistent staging allocation.

The final isolated source is `build/nov17-sprite-cache/flags-source`, based on
frozen candidate-v7 renderer files. The four-file review patch is
[`packed_patch_candidate.patch`](../../../tools/ps2/opt17/packed_patch_candidate.patch),
with [before/after hashes](../../../tools/ps2/opt17/packed_patch_candidate.json).
Only `hw_data.h`, `hw_cache.c`, `ps2_hw_tex.inc` and `ps2_hw_tt.inc` differ from
that renderer snapshot. Production application and whole-game integration are
the coordinator's next gate. The resource loader and the separate P8
transparency-classification fix are outside this patch.

## Representation and exact boundaries

For real extent `uw*uh=N`, the allocation is `N + ceil(N/8)` bytes. The first
plane stores the original translated palette-index bytes, including the low
index of an alpha-zero texel. The second plane stores one bit for alpha 255;
zero means alpha 0. Pixels outside the real extent reconstruct the preceding
transparent AP88 background `0x00ff`. They do not clamp to the last real texel.

The logical `GL_TEXFMT_AP_88`, power-of-two width/height and existing real-extent
fields are retained. A mode byte consumes existing EE tail padding:

| Layout | Mipmap / map record | Mode offset | Driver record |
| --- | ---: | ---: | ---: |
| Default compact | 68 / 76 B | 65 | 140 B |
| Storage opt-out | 76 / 84 B | 74 | 140 B |
| Legacy layout opt-out | 88 / 96 B | 87 | 140 B |

Actual EE object probes verify these sizes and all preceding field offsets and
widths. The fill context still occupies 40 bytes: its previous padding word
carries the compact source pointer. Handles, texture IDs, dimensions, counters
and owner pointers retain their full widths.

Only ordinary canonical AP88 blocks made by `HWR_MakePatch` and its rescue are
encoded. Eligibility requires a kind-zero AP88 source, live canonical columns,
matching positive source/real extents within the logical rectangle, and posts
fully contained in that real rectangle. The original allocator byte bound
remains enforced. Unusual posts extending into logical padding retain the old
raw recipe. Models, raw flats, fade masks, other formats and unsupported sources
retain their existing recipes.

The encoder writes indices and coverage directly from the pinned source in the
preceding band order. It allocates no complete intermediate AP88 image. It
preserves overlapping-post last writes, including clearing coverage when a later
post writes alpha 0. With chroma keying, original index 255 first determines
alpha 0 and the colormap still translates its low byte; for example, translated
`index 100 / alpha 0` remains exactly that pair.

Normal try-versus-mandatory allocation still uses the **old logical AP88 byte
size and 64 KiB threshold**, regardless of the smaller allocation requested.
Rescue always uses the try allocator. A failed rebuild clears the old image
owner and representation mode. Generic raw `MakeBlockEx` also clears the mode,
including on allocation failure and when a diagnostic clones a compact mipmap.
The next successful rebuild publishes its mode only after the complete fill.
The compact source stays on the existing synchronous ring-copy upload path;
public P8 map/flat zero-copy and data-cache ownership gates are unchanged.

## Cached facts and source sampling

The first working prototype was exact but its virtual per-texel scan was too
expensive. Its native-v4 combined scan/fill procedure took 1.54–2.46 times the
preceding AP88 procedure in the tested cases. That candidate was rejected for
production application and remains preserved in `build/nov17-sprite-cache/source`.

The final mode byte additionally records exact, palette-independent facts:
whether the final real rectangle contains a hole, whether it contains an opaque
index 255, and the conservative invariant that all alpha-zero indices equal 255.
During each overwrite the encoder removes the old covered/index 255 contribution
and adds the new one. These counts therefore describe final bytes even with
overlapping posts. An exceptional transparent index may conservatively disable
the fast invariant after being overwritten; it cannot incorrectly enable it.

Actual `tex_upload` scans happen before `tex_plan`. With normal cropping, they
cover the complete real rectangle. With a cap disabling crop, they cover the
complete logical power-of-two rectangle, including its transparent border.
The constant-time scan is used only when the requested scan includes the whole
real rectangle and the cached invariant proves that no live RGB comparison is
needed. It ORs in virtual-border holes and retains the exact opaque255 fact.
Otherwise it performs the live palette RGB check through the exact accessor.
In particular, `hole_rgb` and palette RGB aliases are **not cached**.

The fast P8 filler can copy or gather tight index bytes only when alpha is
ignored by the existing keyed rule or every alpha-zero index is 255. Decimation
keeps the preceding centre sample. It copies exactly the samples whose centres
lie inside the real row, then writes 255 for the virtual border. Exceptional
transparent low indices keep the coverage-aware path. CT32 filling uses the
same palette colour and GS alpha scale as the raw AP88 path.

`HWDBG_COMPOSE` compares decoded compact pixels against the existing raw
reference drawer. Its pre-existing diagnostic reference allocation remains;
normal operation introduces no unpacked image. TT capture retains its existing
logical 4 MiB eligibility gate and makes an independent raw AP88 snapshot.
Later TT expectations do not follow the source after purge. Existing flag
refreshes, palette/colormap changes, filtering, teardown and restart rules remain
on their preceding paths.

## Executed component proof

The final sanitizer report is
`build/nov17-sprite-cache/host-v4/report.json`. The generated C contains literal
candidate types/encoder/rescue, original raw AP88 drawers, the complete actual
AP88 selection case, both source fillers, scan functions and TT functions.
Each compact image is compared directly against the complete raw image made by
the preceding drawer, including all logical padding. The new decoder is not
used to construct the reference image.

Every configuration below passes the same scope:

- Default 68-byte, 76-byte storage, 88-byte legacy layout and packing opt-out.
- All 12,443 stock decoded patch inputs, each with three translation variants:
  **37,329 builds** and **2,716,645,484 directly compared AP88 bytes** per profile.
- **347,600,782 native P8/CT32 upload bytes** compared through 27,350 upload cases,
  including real-extent and complete-POT scans before capped/decimated fills.
- The 595-case existing rescue/PNG/rotation lifecycle fixture, including 71
  nonzero rotation angles, both flips, mapped children, empty sources, repeated
  purges, normal allocation decisions, try failures and real teardown.
- Explicit original 255 → translated 100/alpha 0, covered translated 255, overlapping
  post writes, palette epochs with and without RGB aliases, post-creation flag
  changes, NULL patch with initialized metadata, and unusual raw-fallback posts.
- TT copies compared across the entire logical image and queried after source
  purge; exact single compact allocation sizes checked without AP88 staging.

Nine executed corruptions are detected: lost coverage, normalized transparent
low index, opaque virtual padding, stale raw mode, raw-only rescue, wrong centre
sample, missing opaque255 fact, missing hole fact and a palette-dependent fast
scan incorrectly enabled for arbitrary transparent indices.

Actual EE syntax passes for default, packing opt-out, storage opt-out and legacy
layout, each compiling both engine cache and complete driver with hardware
features enabled. The independent read-only review found no actionable source
bug, including the overwrite-count invariant and exact centre-count arithmetic.

```sh
source /workspace/.onboarding/environment.sh
python3 tools/ps2/opt17/packed_patch_hosttest.py \
  --source-root build/nov17-sprite-cache/flags-source \
  --corpus build/nov17-sprite-audit-v2/patch-corpus.bin \
  --out build/nov17-sprite-cache-host-replay --negative-controls
```

The frozen host artifacts preserve actual source, tools, binaries, logs, complete
source hashes and corpus hash. The corpus itself is retained by the separate
[stock audit](SPRITE_TEXTURE_COMPRESSION_AUDIT_20261116.md); game assets are not
copied into tracked documentation or tools.

## Native exactness and Count results

With the coordinator's exclusive emulator grant, the official locked runner
executed `native-v5/PATCH17.ELF`, SHA256
`382829642a6935d922f7d16d947b60175bf77518a613ab64b10a78ee09e66ff7`.
It completed in 4.10 seconds with zero reported errors. Source/ELF/tool inputs,
critical 32 MiB settings and emulator binary hashes remained unchanged. The
emulator was then released to the next queued test.

Three real pictures pass complete virtual-AP88 comparisons. Their six
cropped/capped views also pass complete contiguous native-upload comparisons,
including naturally unaligned row starts. The fixture executes 108 Count timing
rows across scan/fill/combined phases, both views, warm/cold setup and three
alternating repetitions. Volatile function targets prevent LTO from hoisting
the repeated scan/fill calls out of the timed loops.

| Source | Logical AP88 / compact payload | Combined Count ratio, cropped | Combined Count ratio, capped |
| --- | ---: | ---: | ---: |
| `CNT1A0`, 39x63 | 8,192 / 2,765 B | 0.486 | 0.102 |
| `CORLE0`, 112x112 | 32,768 / 14,112 B | 0.146 | 0.072 |
| `PPALA0`, 434x626 | 1,048,576 / 305,645 B | 0.299 | 0.040 |

Ratios are new/old medians within the same final fixture. Row filling alone is
0.287–0.834 of old Count when cropped and 0.388–0.778 when capped. Common cached
classification is constant-time instead of scanning texels.

These are standalone literal-source procedures compiled with `-O3 -G8` and
two-stage LTO. They are not a binding to shipping whole-game LTO code, a native
GS drawing test, allocator/encoding timing, frame timing or a physical PS2
measurement. Standard profile `EnableEECache=false` makes the warm/cold Count
results identical; those two setups are not independent physical-cache evidence.
The earlier rejected fixture used a row-reused output; the final fixture uses
contiguous image output. Cross-fixture elapsed-time comparisons are not a
performance result.

Build without launching:

```sh
python3 tools/ps2/opt17/packed_patch_native.py \
  --source-root build/nov17-sprite-cache/flags-source \
  --out build/nov17-sprite-cache-native-replay
```

The native `run-pre.json`, `run-post.json`, `timings.json`, logs, ELF/source and
`pinned-run-inputs/` retain the final run evidence. Aggregate source, host and
native results are also in
[SPRITE_CPU_PATCH_RESULTS_20261116.json](SPRITE_CPU_PATCH_RESULTS_20261116.json).

## Memory opportunity and remaining integration gate

Across the stock sprite/character corpus the existing AP88 payload totals
265,310,986 bytes; the final representation totals 72,292,822 bytes, a 72.75%
reduction. For the actual `PPALA0` allocation the payload falls by 742,931 bytes.
Allocator rounding/headers are separate costs. Those corpus sums are not
simultaneous residency, whole-frame peak RAM or a measured active-game saving.

No VRAM saving is attributed to this CPU cache change: existing P8/CT32 texels,
formats, dimensions and CLUT behavior are retained. A smaller CPU source can
change source-cache lifetime, so whole-game texture histories and images still
require integration validation. Encoding cost and shipping allocator behavior
also remain whole-game questions. Physics, resources and feature limits are
not changed by this renderer-only patch. The coordinator must merge the approved
candidate with the separate loader/transparency work, freeze the full source,
and run the complete hardware image/memory/restart gates before release.
