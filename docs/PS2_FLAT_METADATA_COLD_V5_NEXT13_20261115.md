# Cold flat-table constructor, isolated v5 — 2026-11-15

V5 is a preserved alternate. The selected combined-classic candidate uses
[cut V2](PS2_FLAT_METADATA_CUT_V2_NEXT13_20261115.md), whose actual combined
static footprint fits without the constructor hint. These V5 results do not
supersede that decision.

V5 reduces complete actual EE text by 48 bytes relative to V2. Isolated `_end`
is unchanged; acceptance still requires the actual combined static footprint
and whole-game resource/image gates. No constructor load-time gain is claimed.

V5 starts from V2 and adds only guarded `__attribute__((cold))` before
`HWR_LoadMapTextures`. It adds no explicit `noinline`/`noclone`, cleanup
attribute, routine rewrite, parser change, flag, padding, or reserve change.
Removing that single annotation block makes the entire cache file identical
to V2. Constructor, getter, free, parser, types, allocation order, and callback
field timing remain literal-exact. All preceding experiments are preserved.

The hint is justified by all four project call sites: hardware catalogue
completion, both palette-state transition branches, and renderer acquisition
guarded by an unloaded table. The catalogue function is reached by boot/addon
loading; palette toggles are settings/startup events; PS2 renderer switching
is guarded by an actual renderer change. There is no steady-state geometry or
per-draw constructor call. This does not claim that external events can never
be repeated. Exact call-site bodies and all constructor references are in
`flats-cold-callgraph-v5/report.json`.

Source: `build/nov15-next13/flats-cold-source-v5`, cache SHA256
`2394bf0534c8183faab6496303eb4c1f90b1f9db6e7d10977ecc94bd9084be41`.

The global/default unit options remain O2 and match the canonical complete
`.gnu.lto_.opts` bytes. The shipping cold hint nevertheless permits
size-oriented code generation for this constructor; that is the intended
source change, rather than a benchmark-only compiler configuration.

| Actual EE quantity | Canonical baseline | V2 | V5 |
| --- | ---: | ---: | ---: |
| `.text` | 4,373,952 | 4,374,136 | 4,374,088 |
| `_end` | `0x8854b0` | `0x8855b0` | `0x8855b0` |
| Constructor complete input section | 456 | 632 | 464 |
| Constructor frame | 48 | 64 | 64 |
| Getter / free frames | 32 / 32 | 32 / 64 | 32 / 64 |

The constructor saves 168 bytes, but its existing O2 `PS2TexC_Reset` and
`Z_ClearRebuildCosts` callees become out of line and retained, adding 88 and
32 bytes respectively. The complete shipping text saving is therefore 48
bytes. Reset has a 16-byte frame and clear-costs has a zero-byte frame; both
are included in the target proof. This is not a whole-game stack peak claim.

`flats-cold-host-v5/report.json` passes 747 comparisons and nine source controls
over six profiles, including all callback, wall-child, failure, reset, and
inactive-path projections. Exact executed inputs and whole-file/body identity
are frozen beside the report. i386 source/ABI builds remain compiled but not
executed on this kernel.

`flats-cold-context-v5` replaces only the cache IR in the complete canonical
205-input graph; the other 204 inputs are byte-exact. The actual type/byte/
alignment/full-width ABI probe passes. `flats-cold-control-v5` restores the
entire canonical ELF byte for byte. V5 ELF SHA256:
`cbe328f7ff5f961035d4d878efe403d7466bfd31adbbebcbcd1d9b54d420a240`.

`flats-cold-binding-v5/report.json` binds 89 baseline and 90 candidate complete
bodies, 552/553 data objects, 1,103/1,104 literals, all ordered RELAs, padding,
PDRs, and capacities, with nine inner binary controls. Its own binder discovers
each input function's real `STT_FUNC.section`, including
`.text.unlikely.HWR_LoadMapTextures`, and verifies the actual function-owned
PDR, symbol extent, map contribution, and complete resolved bytes. It does not
alias, rename, or mask the cold section or change shared strict helpers. Both
newly retained original O2 callees are bound explicitly. Report SHA256:
`a7ed63f3a89e361e56229c64ad9b94c5d64fdd33598a442434163b5af534e7d3`.

All artifact paths above are under `build/nov15-next13/`. No emulator was run
for V5, and no further attribute or size trial was started without a new design.
