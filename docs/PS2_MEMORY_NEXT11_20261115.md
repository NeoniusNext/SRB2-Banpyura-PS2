# Rejected slope storage experiments — eleventh pass, 2026-11-15

**Both memory candidates are rejected. No slope RAM saving ships.** The
coordinator restored `src/r_defs.h` and `src/p_slopes.c` byte for byte to the
preceding production sources. Both smaller representations pass their
completed component checks but worsen a matched-local-seed 350-frame game
workload. Restoring the original slope storage while retaining the accepted
Software scan restores every non-time zone/cache/process metric.

## Controlled whole-game rejection

All columns use the same primary local seed and MAP11 350-frame workload.
The Software-only counterfactual retains the scan and original slope storage.

| Metric | Reference | Software-only | 112 B / 64 slots | 112 B / 73 slots |
| --- | ---: | ---: | ---: | ---: |
| Used zone B | 24,559,152 | 24,559,152 | 24,318,512 | 24,318,576 |
| Peak zone B | 24,734,944 | 24,734,944 | 24,727,360 | 24,727,344 |
| Evictions | 19,606 | 19,606 | 20,834 | 20,853 |
| Evicted B | 849,579,632 | 849,579,632 | 920,223,680 | 920,400,560 |
| Failed attempts | 2,513 | 2,513 | 2,663 | 2,664 |
| Engine `t_ms` | 106,884 | 106,891 | 115,557 | 115,657 |

The header-only candidate adds **1,228 evictions, 70,644,048 evicted bytes,
150 failed attempts and 8,673 ms** relative to the reference. The bounded
byte-pool follow-up adds **1,247 evictions, 70,820,928 evicted bytes,
151 failed attempts and 8,773 ms**. Their lower used/peak zone values do not
make these outcomes acceptable.

All loader hashes match, and the runs report zero quality warnings/errors.
The Software-only counterfactual matches every non-time `ZCK` field at every
checkpoint and every final non-time `ZSTAT` field, including arena, zone,
cache, libc, stack and spill totals. Its seven-millisecond elapsed difference
is not advertised as overall FPS improvement. In this controlled context,
restoring the original slope storage removes the extra cache work while
retaining the Software change. No particular allocation event or absolute
address phase is identified, and no physical-console FPS claim is made.

The byte-pool run's actual slopes checkpoint changes used zone
**16,879,088 → 16,845,792 B**, saving **33,296 B** at that phase. This is a
real component/checkpoint saving alongside a failed whole-game performance
gate. It is not a shipping saving or a total-process/RSS reduction.

Raw paired reports are under `build/nov15-next11/`:

- `early-pressure-seed1/next11-pressure-350-report.json`: header-only candidate.
- `byte-pool-pressure-seed1/next11-pressure-350-report.json`: bounded follow-up.
- `sw-only-pressure-seed1/next11-pressure-350-report.json`: original-storage control.

The common diagnostic reference ELF is
`c866a2a341f9c361528afc6746f4665f7d8f8a24ac46251d24339f6c2fae3a3c`.
Header-only is
`b577b50131210fa311ef3de64af315c956b6534b29169716d8c356c4dd1bb5db`;
byte-pool is
`9c7e0a614d481a29d5f1823c58506f3d3b5b81bc44a998fe4eb9f0f2cdb80099`;
the original-storage Software control is
`7762701f0370999398639320cf2a3c36e3017228ec78c9811b63fd132d12556e`.
The coordinator's official-run archives preserve full before/after INIs,
non-UI settings, local-seed initializer/input provenance and immutable ELF
pins. Normal production RNG, rain, thunder, audio and other effects stay
unchanged.

## Rejected representations

The first candidate moved independent full-width `moved`/`flags` bytes into
the two alignment bytes after the complete `UINT16 id`. Every geometry
field, pointer and double kept its preceding width, alignment and offset.
Actual complete EE headers show size **120 → 112 B**, alignment **8 B**, and
real pool stride **128 → 112 B**. The original pool kept 64 slots, reducing
each root request from 8,192 to 7,168 B. Persistent vertices, drawsegs,
precision, field ranges and every feature/capacity limit were unchanged.

The only bounded follow-up changed the allocation parameter in `Slope_Add`:

```text
stride = align16(sizeof(pslope_t))
slots_per_chunk = max(1, floor(8192 / stride))
```

It packed 73 complete 112-byte slots into 8,176 payload bytes, using the
preceding 8 KiB byte budget. The original 120-byte type naturally returned
64 slots. It changed chunk count/timing without changing object order,
values, IDs, pointer stability, zeroing, tag/owner/lifetime, failure
publication, epoch invalidation or the generic allocator. No padding,
map-specific switch, lower reserve, smaller limit or allocator rewrite was
introduced. Both bounded candidates are nevertheless rejected.

For ordinary minimum EE pool blocks, the follow-up calculation was
`ceil(N/73)*8192`, compared with `ceil(N/64)*8208`. For the existing source
comment's 2,089 inserts this gives 33 → 29 roots and
270,864 → 237,568 B. This is not a new whole-game slope-count measurement.
On the native 64-bit host both strides remain 128 B and capacity stays 64;
no native pool RAM saving is claimed. Standalone floor-sprite requests,
explicit saved fields and Lua getters kept their original semantics.

## Completed proofs and explicit execution boundary

All original first-stage artifacts remain untouched under
`build/nov15-next11/memory/{host,ee,objects}`. Original tools/docs/sources are
copied with a manifest in `memory/header-only-stage-tools`; earlier comment
bindings remain in `memory/objects-pre-comment`. The first-stage native
reference/current/opt-out each passed **108 cases** and matched the complete
**51,896,400-byte** frozen host oracle. The reference/opt-out native ELFs are
byte-identical. This valid component evidence did not establish a game win.

The expanded byte-pool host proof passes **124 cases and 41,106,024 checks**
for frozen/current/old-layout/pool64-counterfactual and ASan/UBSan. All
**54,167,184 normalized bytes** agree, SHA256
`a659eae8769a603f3d4479dfcec4f233cfc7f40357ecceaf84c1b5a37a93d345`.
Six executed mutations are detected: flag/ID truncation, a lost link,
a corrupt saved angle, a shortened copy and an excessive pool quota.

The fixed-count corpus includes 63/64/65 and 72/73/74 boundaries, 65,537
inserts with unchanged ID wrap, complete named fields, exact pointer
identity, all seven doubles' storage bits, copies, guards, both arena
directions, save/read/Lua/query/fingerprint consumers and standalone
replacement/free/zero. Actual constructor failure/retry at the common fixed
4,672 = 64 × 73 boundary verifies every original pointer and full record,
list/count and next successful record; all logical fields are compared
literally. Separate per-capacity pool invariants assert private used/chunk/
epoch state rather than pretending different physical capacities are equal.
Byte transport, Lua VM and fingerprint sink are controlled service boundaries.

**The four expanded byte-pool EE ELFs were built, not executed.** Their
readiness, source/oracle/ELF pins and clean compile logs remain under
`memory/byte-pool/ee`. Reference and old-layout complete ELFs are identical.
The failing primary game gate stopped further native execution; no 124-case
EE runtime result is claimed. Original compiled-stage tools/hashpins are
preserved under `memory/byte-pool/compiled-tools`.

Eight complete isolated consumers restore every allocated section signature
and ordered relocation target/addend under the old-layout control. The pool64
counterfactual restores the first-stage header-only signatures/relocations.
All isolated text/data/BSS sizes and function frames stay unchanged. These
are non-LTO component bindings, not final-LTO CPU/FPS evidence. The separate
whole-game byte-pool context replaces only `p_slopes.c` IR in the complete
189-input canonical graph, retaining the other 188 inputs and generated
metadata; its coordinator-owned build/run records remain preserved.

Preliminary 120-case host output remains in `byte-pool/host-120`. A
fixture-only aggregate-free fill failure remains in
`byte-pool/failed-common-fill-total-free`; the corrected fixture fills from
the largest free block. Neither trace is a game allocation result.

## Restored source and isolated replay

Shipping source is restored exactly to these preceding hashes:

- `src/r_defs.h`: `6c88912e24bb3f85cc8d0166f5734ad5e01e42dfe0bad3ae266e91cba831f62c`.
- `src/p_slopes.c`: `3ad3ec8f379a2f9b1395aa6eccd49832e561a70dd09678d3318cf8d82c6f0e4d`.

Rejected header hash is
`d58cff0b673ea3cb131737bd80c18a0a17fbfe080d62f812e663d6115de277a3`;
rejected byte-pool source hash is
`ae6d25600a4e9a8d04b9aa11fd2e349a889db875dbec5522b6f88ea52cd43e78`.
`next11_memory_replay.py` verifies/recreates the complete frozen baseline,
applies the checksum-locked zero-context `next11_memory_candidate.patch`
only in an isolated build tree, and checks every one of its 602 source files.
Header-only changes exactly one source; byte-pool changes exactly two.
The live source/index is never modified. Existing mismatched destinations
are refused. The patch SHA256 is
`03498a9458f3bbf58c960064a8dba50451d4401dd8572256167045a9821a644b`.

```sh
python3 -B tools/ps2/next11_memory_replay.py --stage header-only
python3 -B tools/ps2/next11_memory_replay.py --stage byte-pool
source /workspace/.onboarding/environment.sh
python3 -B tools/ps2/next11_memory_hosttest.py --candidate build/nov15-next11/memory/header-only-replay-src --sanitize --negative-controls
python3 -B tools/ps2/next11_memory_hosttest.py --sanitize --negative-controls
```

Public proof defaults select the checked isolated byte-pool source, never
restored `ROOT`. Shared validation rejects unchanged/live or peer-modified
trees before compilation. Default replay outputs use separate stage-specific
`memory/replay-*` directories and refuse to overwrite completed proofs.
Header-only uses five applicable mutations; byte-pool adds the quota mutation.

Optional target reproduction uses `next11_memory_objectcheck.py` and
`next11_memory_eetest.py`, with the same `--candidate` selection. To bind a
fresh byte-pool replay to a fresh header-only object proof, pass
`--header-only-proof build/nov15-next11/memory/replay-header-only/objects`.
The EE tool builds four variants. Its explicit `--analyze-only --run` option
runs the official locked runner inside `next11_runtime` with immutable
fixture/ELF/oracle/tool pins and complete settings archives. Those optional
runs do not describe work performed for the rejected four byte-pool binaries.
