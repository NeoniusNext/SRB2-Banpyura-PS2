# Shared flat deletion, isolated v3 — 2026-11-15

This evaluated size experiment is not recommended for adoption: it adds
40 bytes of actual EE text relative to V2 and does not reduce isolated `_end`.
It is kept separately from the unchanged V1/V2 evidence.

V3 adds only guarded `__attribute__((noinline, noclone))` to the existing
private `DeleteTextureMipmap`. The guard is the same active PS2/profile/
flat-metadata guard. Removing that one annotation block makes the entire file
identical to V2. The original helper declaration/body and all four teardown
callers remain literal-exact. There is no cold-parser attribute, routine
rewrite, fake map-record cast, compiler-flag change, padding, or reserve change.

The isolated source is `build/nov15-next13/flats-share-source-v3`; its cache
SHA256 is
`e936555a84c2cab3d8a6524822576c6ba4c50b6c505b8f2151521d3db6d0d431`.
[`next13_flats_share_variant.py`](../tools/ps2/next13_flats_share_variant.py)
recreates the complete 602-file tree.

| Actual default EE component | V2 | V3 |
| --- | ---: | ---: |
| `.text` bytes | 4,374,136 | 4,374,176 |
| `_end` | `0x8855b0` | `0x8855b0` |
| FreeMapTextures complete input section | 344 | 296 |
| Shared DeleteTextureMipmap complete input section | absent | 88 |
| FreeMapTextures frame | 64 | 48 |
| Shared DeleteTextureMipmap frame | absent | 32 |
| GetLevelFlat frame | 32 | 32 |
| LoadMapTextures frame | 64 | 64 |

The new helper's 88-byte contribution exceeds the caller's 48-byte reduction.
Its extra call frame also makes the direct free/helper frame sum 80 bytes
instead of V2's 64-byte free frame. These are actual body/PDR observations,
not native timing or whole-game stack-high-water measurements.

The host result `flats-share-host-v3/report.json` passes all 711 comparisons
over six profiles and eight source mutations. It preserves the 675-case V2
matrix and adds 36 delete-callback projections that clear pixel data or replace
a child link. Mutations that cache either field before the callback are
detected. `literal-sharing-proof.json` records whole-file attribute-only
identity, exact original helper identity, unchanged caller/constructor paths,
and frozen executed tool/fixture bytes. An independent reviewer reran all
711 cases and eight controls and confirmed the source token projection.

`flats-share-context-v3` uses the actual default 205-input EE LTO graph, with
only the cache IR replaced and the other 204 inputs exact. The changed unit's
entire `.gnu.lto_.opts` remains exact. The actual header probe verifies the
76/84-byte types, alignment, byte alias, and full-width index/pointer facts.
`flats-share-control-v3` restores the entire canonical ELF byte for byte.

V3 ELF SHA256:
`3af6918b0a2389488ae6463fb8f7f488ae2ae7b7f9f989ea730dfb8aba1ae1b0`.

`flats-share-binding-v3/report.json` passes complete linked binding for
60 baseline and 60 candidate bodies, all referenced data/literals, ordered
RELAs, padding, PDRs, and capacities. Ten inner binary mutations are detected,
including the new helper body. The exact target order witness records one
uncloned helper: its driver callback at input offset 24 precedes the fresh
data-field load at offset 64 and the `Z_Free` transfer at offset 76. All four
actual caller transfers and both fresh post-callback child-link reloads are
checked. The helper body still calls the driver before reading the possibly
changed data pointer; callers still reread the possibly changed child link.

All artifact paths above are under `build/nov15-next13/`. No emulator was run
for this experiment, and no additional attribute experiment was stacked onto
the measured result.
