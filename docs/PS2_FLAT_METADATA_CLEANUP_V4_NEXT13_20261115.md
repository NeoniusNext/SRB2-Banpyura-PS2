# Complete cleanup sharing, isolated v4 — 2026-11-15

V4 is a preserved unsuccessful size experiment. Its actual EE text grows
40 bytes relative to V2; isolated `_end` remains baseline +256 bytes.
No production source was changed.

Starting from V2, the active PS2/profile/flat-metadata path calls one
`noinline,noclone` `FreeMapMipmap(GLMipmap_t *, boolean)` for both wall and flat
records. Its body is the original `FreeMapTexture` body projected onto the
mipmap pointer and child-data policy parameter. It retains the child's
callback, fresh data read, fresh child-link read for free, nulling, and primary
callback/free order. The original `FreeMapTexture` definition and calls remain
literal under the inactive guard. `DeleteTextureMipmap`, the V2 constructor,
getter, parser, and reset semantics are unchanged. There is no fake map cast,
cold hint, compiler flag change, padding, reserve change, or new cap.

Source: `build/nov15-next13/flats-cleanup-source-v4`, cache SHA256
`00ce59995f79ff2fe96fd4bc8875b3bd875913c2b3e376c713cc1bd09a184453`.

| Actual EE quantity | V2 | V4 |
| --- | ---: | ---: |
| `.text` | 4,374,136 | 4,374,176 |
| `_end` | `0x8855b0` | `0x8855b0` |
| Cleanup caller complete section | 344 | 200 |
| Shared cleanup complete section | absent | 184 |
| Caller frame | 64 | 32 |
| Shared helper frame | absent | 48 |
| Getter / constructor frames | 32 / 64 | 32 / 64 |

The helper plus caller contributes 384 bytes rather than 344, and their frame
sum is 80 rather than 64 bytes. These are actual code/PDR measurements, not a
whole-game stack peak or timing result.

`flats-cleanup-host-v4b/report.json` passes 747 comparisons and nine source
controls over six profiles, including the prior 711 cases plus wall-child
ownership and callback projections. The earlier `flats-cleanup-host-v4`
development stage stopped on a stale negative-control template anchor and is
not the authoritative result. Exact executed tools and the body projection
proof are preserved with the v4b report. An independent reviewer replayed all
747 cases and nine controls successfully.

`flats-cleanup-context-v4` and `flats-cleanup-control-v4` use the complete
default 205-input graph, original O2 unit options, and 204 unchanged IR inputs.
The opt-out restores the entire canonical ELF. The v4 ELF SHA256 is
`687c80d190b0ebea0e98053e495abfcffdeece6e94cfb30e4dbe4a0be751dab7`.

`flats-cleanup-binding-v4/report.json` passes 60/60 complete body bindings,
400/401 referenced data objects, 1,006/1,007 literals, exact ordered RELAs,
padding, PDRs, capacities, and ten inner binary controls. The explicit target
order proof includes each callback/data/free path, fresh child-link loading,
free before nulling, and the two wall/flat helper transfers. Its SHA256 is
`8a6ebed612546a9c8bbe571a302a1b4eca0bbed34eb979e7b4b387bbbfffca04`.

All artifact paths are under `build/nov15-next13/`. V1–V3 remain unchanged;
no emulator was run for V4.
