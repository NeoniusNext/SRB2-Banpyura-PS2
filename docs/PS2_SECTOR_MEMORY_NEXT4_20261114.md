# Sector spawn snapshot memory, fourth pass — 2026-11-14

The reference is the completed third pass in `build/nov14-next4/baseline-src`,
reproduced by `tools/ps2/prepare_next4_baseline.py`. The production change is
limited to `p_setup.c`, `p_setup.h`, and the `ArchiveSectors` reader in
`p_saveg.c`; all preceding uncommitted optimizations remain in place.

## Representation and scope

Sector snapshots keep the original heights, flat IDs, light level, special,
independently copied tag lists, and original colormap pointers in each compact
record. The other 22 scalar fields use a presence mask and copied full-width
`UINT32` words. Their defaults include `FRACUNIT` scales and gravity,
`UINT32_MAX` portals, the original floor-special flag, and the relevant zero
values. Every deviation is stored, including negative values and all flag bits.
No live sector or Lua field representation changes.

EE GCC emits `sizeof(spawnsector_t) = 104`,
`sizeof(compactspawnsector_t) = 40`, and four-byte alignment for both the compact
record and the value words. For `N` sectors and `B` nondefault scalar values,

```text
before = 104*N
after  = min(104*N, 40*N + 4*B)
```

Only a strict decrease selects compact storage. The pre-count allocates no
memory; the compact records and words occupy the original sector snapshot's
single allocation at the same point in the allocation order. Equal and dense
cases keep the preceding fixed array. No extra allocation, capacity limit,
resize, moving growth, shrink reserve, or old-plus-new root peak is introduced.
The representation flag is published after successful allocation. The fixed
multiplication is checked before reading inputs, and the compact sum is checked
before allocation. The maximum 22 words per sector cannot overflow the word
count after the full-record size check on the verified host and EE layouts.

`P_GetSpawnSector` reconstructs a compact record into caller-owned scratch.
`ArchiveSectors` is its only production reader. All comparisons and writes for
that sector finish before the scratch is reused. The public snapshot type and
every network/save field, width and ordering remain unchanged. The fixed case
returns its original record. Colormap pointer identity and the independent
pooled sector tag ownership retain their original behavior.

The snapshot gate remains **network games or `-keepspawn`**. Ordinary single
player keeps zero snapshot allocations and receives no sector snapshot saving.
`PS2_NOOPT_SPAWNSECTORS` selects the preceding allocation and copy. This change
does not alter graphics, physics, UDMF, add-ons, Lua, network features or limits.

Costs: map snapshot creation adds a scan of 22 fields per sector plus packing;
compact network archiving reconstructs those fields. Both construction and
sector archiving have a 104-byte EE automatic scratch object. These object
sizes are not measured whole-function stack-frame deltas. No FPS or VRAM gain
is claimed, and dense maps can retain exactly the old dynamic allocation.

## Differential proof

```sh
source /workspace/.onboarding/environment.sh
python3 -B tools/ps2/next4_sector_hosttest.py --negative-controls
python3 -B tools/ps2/next4_sector_hosttest.py --no-ee-abi \
  --sanitize --negative-controls --out build/nov14-next4/sector-memory-asan
SRB2_PS2_OUT=build/nov14-next4/sector-syntax python3 -B tools/ps2/build.py \
  --syntax --jobs 1 src/p_setup.c src/p_saveg.c
SRB2_PS2_HW=1 SRB2_PS2_OUT=build/nov14-next4/sector-hw-syntax \
  python3 -B tools/ps2/build.py --syntax --jobs 1 src/p_setup.c src/p_saveg.c
python3 -B tools/ps2/next4_sector_fixture.py
```

The host driver extracts each version's actual production snapshot types,
constructor, enable gate, `Tag_Compare`, FOF diff/archive functions, complete
sector and line archives, side diff/archive functions, and save-buffer writers.
It runs the real zone and PS2 arena. The minimal live sector stand-in contains
all the snapshot fields plus the runtime fields those archives read; the
colormap-index service returns deterministic IDs. This is not a server/client
transport or complete engine fixture.

All **1,702 cases** pass for the frozen reference, current source and old-layout
switch. Eleven sizes include zero, 25,100 and 70,001 sectors. Every count from
zero through 22 populated fields per sector occurs, with field selection
rotated by sector so compact cases exercise every field. The corpus includes
`INT32_MIN/MAX`, `INT16_MIN/MAX`, `UINT32_MAX`, arbitrary high flag bits, zero
and nonzero colormap pointers, and independent tags. The complete snapshot is
checked again after changing every live scalar, colormap pointer and tag value.
The previous compact line snapshots and complete side snapshots are checked too.

Both actual archive streams per case are compared as literal framed bytes.
All **261,282,241 bytes** match, SHA256
`aa6c9333f41e8acbb1c4b1cd219fb2dcea5e5fe0713a0e7787f6fee59f3085d8`.
The complete sector/line/side/FOF writer paths retain their preceding behavior,
including any existing archive flag selection. Deliberately corrupted default
values, lost colormap identity, changed gravity wire bytes, and premature
representation publication are detected.

No measured request, retained arena use, allocation count or construction peak
increases. Sizes through 1,200 sectors exercise one-/two-sided arenas, red zones
on/off and network/`-keepspawn` gates; the two largest sizes use the one-sided
network case. All four construction allocation sites are fault-injected for
sparse and dense inputs. Failed replacements in both representation directions
preserve root and mode together; retry, level release, reload, real cache
eviction, ordinary single-player gating and overflow-before-input checks pass.
ASan/UBSan repeats the complete suite and negative controls successfully.

The EE ABI probe additionally compiles constant assertions for the actual
compiler's `UINT32` round trips through signed 32-/16-bit endpoints and all bits
of both production enum flag types. Software and Hardware target syntax checks
pass. Extracts, native logs, exact byte hashes, EE object/constants and results
are in `build/nov14-next4/sector-memory*`.

## Memory evidence and integration fixtures

Native figures below are measured with 64-bit pointers, one-byte production
booleans, the real native arena header/rounding and the unchanged other snapshot
allocations. The native record sizes are 120 and 64 bytes. These synthetic
results are **not EE whole-level measurements or simultaneously resident corpus
totals**.

| Synthetic sector case, one-sided arena | Before peak | After peak | Before retained | After retained |
| --- | ---: | ---: | ---: | ---: |
| 31 sectors, all defaults | 4,784 B | 3,040 B | 4,560 B | 2,816 B |
| 1,200 sectors, one changed field each | 149,728 B | 87,328 B | 149,504 B | 87,104 B |
| 25,100 sectors, all defaults | 3,113,328 B | 1,707,728 B | 3,113,104 B | 1,707,504 B |
| 70,001 sectors, all defaults | 8,681,056 B | 4,760,992 B | 8,680,832 B | 4,760,768 B |
| 70,001 sectors, all 22 fields changed | 8,681,056 B | 8,681,056 B | 8,680,832 B | 8,680,832 B |

Stock MAP11 has 4,828 sectors, giving an upper bound of **308,992 B** before
rounding; its actual saving depends on the state after binary conversion.
Integrated map checkpoints and final builds are recorded by the coordinator.
This upper bound must not be reported as a measured MAP11 saving.

`SECT_COMPACT.pk3` and `SECT_FIXED.pk3` under
`build/nov14-next4/sector-addons` each contain a 12-sector, 31-line MAP99 room
map. With `-file <addon> -warp 99 -keepspawn -loadhash -zck`, the first requests
1,248 → 480 bytes of sector payload. The second sets 16 safe, nondefault UDMF
fields per sector (plane transforms, lighting, gravity and a sector flag),
reaching exact equality at 1,248 bytes and retaining the fixed layout. It uses
no invalid portal or resource index. Expected input metadata accompanies both
fixtures; paired integrated loader hashes and render frames are the engine
checks, beyond the standalone archive/allocator proof.
