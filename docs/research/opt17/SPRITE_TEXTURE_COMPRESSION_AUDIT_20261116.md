# OPT17: stock sprite and texture compression audit

The useful next target is the CPU patch upload cache. The GS already stores
ordinary stock patches as P8 and already crops their power-of-two upload images
to the real picture extent. A second AP88-to-P8 conversion in the GS therefore
does not save another factor of two. Shared-palette P4 has very little stock
sprite coverage. A tight CPU index/coverage representation has substantially
more opportunity without changing texels, native formats or light tables.

This is an audit and representation model, not an adopted implementation.
Production files were not edited and no game or emulator was run. Source input
is the complete frozen `build/nov16-hwopt/candidate-v7/source` snapshot. The
canonical four cooked packs in `build/pak` were read without modification.

## Reproducible corpus and actual source checks

Run from the repository root, with its existing toolchain/assets environment:

```sh
source /workspace/.onboarding/environment.sh
python3 tools/ps2/opt17/sprite_texture_audit.py \
  --source-root build/nov16-hwopt/candidate-v7/source \
  --out build/nov17-sprite-audit-replay \
  --sanitize --negative-controls --ee-abi
```

The final evidence is `build/nov17-sprite-audit-v2/`:

- `report.json` contains pack/source/tool hashes, aggregates, checks and ABI.
- `rows.json` contains every picture's dimensions, holes, palette states,
  source fields, compressed size and physical GS span calculations.
- `patch-corpus.bin` preserves each tested decoded patch and expected checksums.
- `actual-source-oracle.c`, compiler/run logs, executed binaries and
  `frozen-tools/` preserve the oracle and the exact tools used.

The parser checks every column and every complete post before admitting a
picture. The independent C oracle uses the literal frozen
`Picture_CheckIfDoomPatch`, `Patch_CalcDataSizes`, `Patch_MakeColumns`,
`Patch_CreateFromDoomPatch`, `HWR_MakePatch`, `ap88_scan` and
`tex_footprint`. It compares all normalized column/post fields and canonical
pixel streams by complete-stream CRC32, then checks complete generated AP88
images against the Python model's CRC32. The Python tight-index model also
reconstructs and directly compares the entire modeled AP88 byte array, including
all power-of-two padding. These are whole-input checks, not sampled pixels;
the C/Python comparison is a checksum comparison, not a formal collision proof.

All 12,443 admitted patches pass the sanitizer oracle: 2,485,855 posts,
154,276,454 canonical pixel bytes and 881,263,572 modeled AP88 bytes. The
executed post-corruption and AP88-corruption controls must fail. The actual EE
compiler asserts `patch_t=48`, `column_t=12`, `post_t=12`, with four-byte
column/post alignment. Pixel alignment in the embedded patch recipe is 16.

The earlier `v1` run was developmental and is superseded by the frozen `v2`
tools and report. It has the same stock totals but does not contain the final
tight-index reconstruction checks and corruption controls.

## CPU source and cache payload

These are **sums across all distinct lump IDs**, not simultaneous memory
residency or peak RAM. Texture-directory patches are also inputs to composition;
the audit's hypothetical standalone AP88 image for each of them is not a claim
that the running game creates all those images.

| Measure | All admitted patches | Sprites plus character pack |
| --- | ---: | ---: |
| Picture count | 12,443 | 7,306 |
| Canonical pixel bytes | 154,276,454 | 19,614,135 |
| Canonical structure/column/post/pixel bytes | 201,444,534 | 34,621,591 |
| Existing full power-of-two AP88 payload | 881,263,572 | 265,310,986 |
| AP88 cropped to the real extent, still two bytes/texel | 524,717,030 | 128,517,382 |
| Tight indexed-byte model, eligible primary stock images | 262,358,515 | 64,258,691 |
| Tight indexed byte plus an always-present coverage bit | 295,155,586 | 72,292,822 |

The primary unmodified sprite corpus has no opaque index255 mixed with holes.
Its tight indexed-byte model is 75.78% smaller than the existing AP88 payload.
The more general index/coverage model is 72.75% smaller. Coverage keeps holes
separate from opaque index255 when a runtime translation maps a visible colour
to 255. The original real-extent metadata supplies the implicit transparent
padding outside that rectangle. An adapter must still reproduce all original
index/alpha bytes for every existing consumer.

For example, `Sprites/Scenery/Pristine Shores/PPALA0` is 434x626. Its existing
512x1024 AP88 buffer is 1,048,576 bytes. Tight indices need 271,684 bytes, or
305,645 with a coverage bit, before a possible allocator/header cost. The
per-image payload opportunities are 776,892 and 742,931 bytes respectively.
`PPALB0`, `SNTTBLBR` and `SSTTBLBR` also have 1 MiB modeled AP88 buffers.
This does not establish that all four are resident in one scene.

The native crop is already present in `tex_upload`; the CPU allocation in
`HWR_MakePatch` still uses `mipmap.width*mipmap.height*2`. The source patch's
canonical columns/posts/pixels remain live as well. `Patch_IsEvictable` rejects
a source with hardware metadata. The newly retained AP88 rescue deliberately
depends on those source columns. Freeing canonical source after a GS upload
would therefore break an existing recovery path.

A bounded next prototype could encode only ordinary AP88 blocks generated by
`HWR_MakePatch` and its rescue, keeping the logical format and dimensions.
The default 68-byte mipmap has three tail padding bytes available, but any
representation byte requires actual target offset/size assertions. The driver
scan and both AP88 fillers, `tt_capture`/expectations and `HWDBG_COMPOSE` need an
explicit adapter. Capped and decimated reads must reproduce implicit padding;
they cannot read a tight buffer with the old power-of-two AP88 stride. Generic
images and unsupported sources retain their old path. Updates, colormaps,
repeated purges, teardown, failed try allocations and both header layouts also
need component proof before integration. A smaller buffer alone does not prove
unchanged global source-cache history or whole-game images.

## Canonical patch compression and duplicate sources

The audit compresses every canonical pixel stream and every AP88 image with
LZ4HC level12, immediately decodes it and compares the complete bytes. The
reported model retains raw data when it is smaller and charges an eight-byte
header for compressed buffers. It does not include a new allocator header, workspace
or any index needed for random access.

| Lossless model | All admitted patches | Sprites plus character pack |
| --- | ---: | ---: |
| Canonical pixels after HC12 model | 26,407,846 B | 7,446,435 B |
| Canonical pixel-byte opportunity | 127,868,608 B | 12,167,700 B |
| Full AP88 after HC12 model | 34,585,436 B | 11,225,168 B |
| Post12 to post8 layout opportunity | 9,947,184 B | 2,706,016 B |
| Post12 to post6 layout opportunity | 14,902,720 B | 4,045,360 B |

Maximum stock fields are post top2032, length254, data offset2032, and
2,048 pixel bytes per column. Compact post records would fit this corpus.
Those observed limits do not establish bounds for generated patches, rotations,
Lua or add-ons. The present `post_t` deliberately has unsigned/full-size fields;
a selective compact representation needs accessors and a full-width fallback.

Canonical pixel streams are currently consumed by direct column pointers in
software draw/composition, picture conversion, rotation, UI and hardware
reconstruction. LZ4 storage is not random access. A whole-image decode can add
the original bytes back at the worst allocation point; a useful compressed
source design needs bounded decode windows and explicit consumers/lifetimes.
The existing embedded patch also owns pixels, columns and posts in one block,
so its pixel tail cannot simply be freed independently.

There are 578 duplicate canonical patch instances in 426 groups, representing
4,197,278 canonical payload bytes across the full corpus. Ignoring patch
offsets for a possible shared immutable upload image finds 656 duplicate
instances in 498 groups and 24,879,454 modeled AP88 bytes. These are deduplication
opportunities, not current RAM savings. Pixel ownership, updates, translations,
rotations, script references and reference counts must preserve independent
object lifetimes. The packs' existing on-disk deduplication already covers a
different storage layer and must not be counted again.

## Native GS formats and palette costs

With the current real-extent crop enabled and no texture cap, every admitted
ordinary primary stock patch follows the already existing P8 path. Thirteen
contain opaque255; none also contains a real-extent hole. Thus a new
opaque255-to-an-alias conversion has **zero primary stock candidates** in this
mode. `INTRO6` (352x400) and `ZOOMING` (640x256) are two NPOT opaque255 pictures;
with a cap disabling crop, their transparent padding can force the existing
CT32 path. The report's full-size span arithmetic for the 21 oversized pictures
is not a claim of legal full-size GS sampling; the real driver still plans or
decimates them.

The four cooked packs contain no native RGBA pictures. The fifteen stock PNGs
already contain canonical indexed Doom patches. External model packs, arbitrary
add-on RGBA textures, rotations and runtime translations are not this census.
For generic images, partial alpha, opaque255 mixed with holes, and alpha-zero
RGB different from palette255 still require the existing exact fallback.
Transparent RGB matters under bilinear filtering.

| Legal direct P4 transfer candidate | All patch inputs | Sprites plus characters |
| --- | ---: | ---: |
| Existing aligned16-entry band, shared full CLUT | 882 | 12 |
| Sum of image span reductions, shared-band case | 10,850,560 B | 30,464 B |
| At most16 index/alpha states, custom remap required | 4,182 | 1,075 |
| Sum of image span reductions before custom CLUTs | 43,639,552 B | 3,847,936 B |

Only 16 sprite images even fit a shared aligned band; four have widths not
divisible by 8. Of the 12 legal direct transfers, ten have holes, eleven use band 15
and one uses band 10. No character-pack image qualifies for shared-band P4.
The large aggregate patch opportunity is mainly texture-folder inputs, which
must not be mistaken for composed texture coverage or sprite residency.

Raw indexed flats independently reproduce the earlier full census: 1,386
images, 34,735,104 bytes; 409 aligned-band images, 8,796,160 source bytes and
4,398,080 bytes of summed full-size GS image-span opportunity. All409 lack255.
There are 1,174 flats with at most 16 arbitrary colours, but arbitrary remapping
needs palette indirection and is not the existing shared-band mode.

PSMT4 eligibility is a constraint on index/alpha states, not just visible RGB.
With the existing palette, opaque indices must fit one aligned band and a hole
must still select the transparent255 entry. Merely masking off the high nibble
of a non-band sprite changes colours. The retained P4 protocol loads the full
T8 CLUT and then selects a T4 destination bank with CSA; CSA is not a source
offset. CSM2 is documented for CT16 only and is not a CT32 palette-packing
shortcut. See the checked manual locators in
[the OPT16 source catalog](../opt16/PS2_SOURCES_20261116.md).

An aligned-band view needs no extra CLUT storage beyond the current shared
palettes. A custom arbitrary16/256-colour view needs a source-index or RGBA map
and texture-specific palette cache identity, including palette generation,
translation, light-table generation, light row, keyed state and other CLUT
variants. The existing cache stores a full CT32 16x16 image per palette:
**1,024 bytes per resident row**. Thirty-two rows would be 32 KiB per image and
light table, or 64 KiB with both keyed/plain variants, if all were retained.
The existing total CLUT reservation is 124 slots (124 KiB); custom rows compete
for it and can increase uploads/flushes even without enlarging that reservation.
An independently proven smaller 16-entry source could have a smaller footprint,
but a 64-byte colour payload is not a 64-byte allocatable GS block. The minimum
allocation unit is 256 bytes and the CSM1 source placement must be proved.

Charging just one existing-format 1 KiB custom CLUT per eligible sprite already
reduces the summed positive image opportunity from 3,847,936 to 2,989,568 bytes.
That is a deliberately limited one-row illustration, not the cost of complete
lighting. It cannot justify treating all remap palettes as free.

The sprite stream currently rejects `psm != PSM_T8` in two gates. Its VU program
emits per-sprite TEX0/CLAMP, followed by a T8 row TEX2 load when needed. A P4
sprite needs the appropriate post-load selector and correct per-texture CSA
even when a light row is unchanged. Relaxing the C gates alone does not update
that packet protocol. Native tests must cover row/band alternation, clipping,
pending draws, palettes, translations, filters and context restarts.

Payload and physical allocation remain distinct: a 16x16 P8 image is 256 bytes
and its P4 payload 128 bytes, but either needs a 256-byte range. Sparse GS block
orders can also make a narrow/tall image's contiguous range much larger than
its texel payload. All opportunities above use the literal driver's contiguous
range calculation, not a blanket claim of half VRAM occupancy. Smaller physical
images also feed eviction, unplanned reserve and source-cache timing. The
existing adaptive-policy counterexample still rules out silently enabling P4
in ordinary planning mode; see
[the full-mode investigation](../opt16/P4_FULL_SIZE.md).

## Opaque255 RGB-alias proposal

The base PLAYPAL 255 is RGB `00ffff`, with no equal RGB below 255. Of its 14 banks,
only bank 4 has aliases for its 255 colour (`bf7171`). The report lists them.
Even when a dynamic palette permits an exact RGB alias, retaining `baked=1`
alone is insufficient to preserve the old CT32 rendering path:

- `baked` is only consumed by the palette-change drop loop.
- Native colormap lighting enters by indexed PSM and CLUT kind 1/2.
- Indexed tint/multiply branches and the TT expectation also change behaviour.
- `out_pal()` can use the screen palette; old baked CT32 uses the texture palette.

An indexed-baked semantic would need explicit handling in those consumers,
palette sources and epochs. The old CT32 fallback can also be affected by
partial-alpha/blend classification. This is a plausible separate generic-image
research direction, but it has no uncapped ordinary primary stock candidate
and is less valuable here than reducing the CPU AP88 duplicate.
