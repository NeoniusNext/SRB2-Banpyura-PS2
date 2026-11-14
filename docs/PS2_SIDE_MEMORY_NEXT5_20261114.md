# Side spawn snapshot memory, fifth pass — 2026-11-14

The reference is the completed fourth pass in
`build/nov14-next5/baseline-src`, recreated and checksum-verified with
`tools/ps2/prepare_next5_baseline.py`. This change preserves all four preceding
uncommitted passes. Production edits are limited to `p_setup.c`, `p_setup.h`,
and the two side snapshot reads in `ArchiveLines` in `p_saveg.c`.

## Representation and scope

The ordinary PS2 side snapshot has six scalar fields: full signed 32-bit
horizontal/vertical offsets and top/bottom/middle texture identifiers, plus the
full signed 16-bit repeat count. All default to zero. Its public type and live
side representation remain unchanged.

The private representation gives each side one `UINT32` descriptor. Its low
six bits mark the nondefault fields; the upper 26 bits index that side's copied
`UINT32` values. Every nondefault field is copied verbatim, including negative
offsets, negative texture identifiers used by existing special handling, and
negative repeat counts. No values are narrowed, shared with live state, or
interned. `P_GetSpawnSide` reconstructs a full snapshot into caller-owned
scratch; fixed storage returns its preceding record directly.

EE GCC confirms `sizeof(spawnside_t) = 24`,
`sizeof(compactspawnside_t) = 4`, and four-byte alignment for both the compact
records and the value stream. For `N` sides and `B` nondefault values:

```text
before = 24*N
after  = min(24*N, 4*N + 4*B), when B fits the descriptor index
```

Equal, dense, and index-overflow cases keep the original fixed layout. The
index check is a representation fallback, not a map limit. The alternate
`SIDE_UDMF` build retains all 26 original scalar fields, their scale/default
values, signed widths, and booleans. Its descriptor uses 26 mask bits and six
index bits; more than 63 copied words selects the unchanged full layout.

The constructor counts before its existing side allocation, in the existing
sector/line/side/tag-pool allocation order. It checks the fixed multiplication
before input access, bounds counting by the index range, checks the compact
sum, and publishes the mode only after the replacement root allocates
successfully. Records and copied values occupy the same single allocation.
There is no additional allocation, reserve, resize, moving growth, temporary
heap storage, capacity reduction, or old-plus-new snapshot allocation peak.

The complete source audit finds only two production side-snapshot readers,
both `GetSideDiff` calls in `ArchiveLines`. Each finishes comparing its side
before the same scratch is reused for the other side. All archive field order,
widths and diff bits are unchanged. Sector/line snapshots, copied tag ownership
and colormap/string pointer ownership retain their preceding behavior. The
previous callback audit applies: intervening zone reclamation frees caches or
private renderer state; it does not run a Lua collector/gameplay callback that
could change fields between counting and copying.

The gate remains **network games or `-keepspawn`**. Ordinary single-player
keeps zero snapshot allocations and gets no side-snapshot RAM saving. Defining
`PS2_NOOPT_SPAWNSIDES` restores the preceding fixed side allocation. Graphics,
physics, Lua, UDMF, add-ons, network features and accepted map limits remain.

Costs are a six-field scan during ordinary snapshot allocation and a six-field
reconstruction per archived side. Archiving uses one 24-byte EE automatic
snapshot buffer. Isolated production EE `-O2` non-LTO assembly has
`P_CopySpawnState` frames 192→208 bytes and `P_SaveNetGame` frames 272→288
bytes; the latter inlines `ArchiveLines`. New `P_GetSpawnSide` has a 32-byte
frame and `P_SpawnSideMask` has no frame. These are isolated compilation
results, not a final LTO stack high-water or runtime/FPS measurement.

## Verification

```sh
python3 -B tools/ps2/prepare_next5_baseline.py
python3 -B tools/ps2/next5_side_hosttest.py --no-ee-abi --negative-controls
python3 -B tools/ps2/next5_side_hosttest.py \
  --out build/nov14-next5/side-memory-asan --sanitize --no-ee-abi --negative-controls
source /workspace/.onboarding/environment.sh
python3 -B tools/ps2/next5_side_hosttest.py --ee-abi-only
SRB2_PS2_OUT=build/nov14-next5/side-syntax python3 -B tools/ps2/build.py \
  --syntax --jobs 1 src/p_setup.c src/p_saveg.c
SRB2_PS2_HW=1 SRB2_PS2_OUT=build/nov14-next5/side-hw-syntax \
  python3 -B tools/ps2/build.py --syntax --jobs 1 src/p_setup.c src/p_saveg.c
SRB2_PS2_HW=1 SRB2_PS2_DEFS=-DPS2_HW_SIDE_FULL \
  SRB2_PS2_OUT=build/nov14-next5/side-hw-full-syntax \
  python3 -B tools/ps2/build.py --syntax --jobs 1 src/p_setup.c src/p_saveg.c
python3 -B tools/ps2/next5_side_fixture.py
```

The native fixture extracts and runs the actual production constructor,
snapshot gate, accessors, `GetSideDiff`, `ArchiveSide`, `ArchiveLines`,
`ArchiveSectors`, FOF archive, tag comparison, save-buffer writes and real zone
allocator. Unrelated engine services have deterministic stubs. Its independent
field inventory checks every side scalar before and after every live side
field changes. Complete sector/line snapshots and pointer/tag ownership are
also checked. Native booleans use the production one-byte `_Bool` width.

All **1,558 cases** pass: 656 ordinary and 902 alternate `SIDE_UDMF` cases.
They cover twelve sizes from zero to 70,001 sides, sparse/default/dense and
exact equality thresholds, both arena directions, red zones, network and
`-keepspawn` gates, shared front/back sides, absent sides, signed 32-/16-bit
extrema, high-bit sector flags, nondefault scales and all UDMF booleans. The
26-field cases also check actual descriptor index boundaries at 62/63/64 words.

Every case compares both actual framed world archives as literal bytes against
the frozen reference and the old-side-layout switch:

| Layout | Compared bytes | SHA256 |
| --- | ---: | --- |
| Ordinary PS2 sides | 44,376,624 | `8802abf3521ee00832e460df3a91064f1d643fad3f3971d7bfc8522211ed64a0` |
| Alternate `SIDE_UDMF` | 132,198,198 | `59d5c7d9f116d8f52d6fa8dac28e61f108afa8db32019edb0167155684c2b57a` |

All **176,574,822 bytes** match. No measured request, native arena peak,
retained byte count or construction allocation count increases in any case;
the fixed switch reproduces all preceding allocator results exactly. Fault
injection at all four construction allocations passes cleanup and reload.
Failed side-root replacements in both representation directions preserve the
old root and its mode together, then retry and release completely. Real cache
eviction clears its owner without changing the snapshots. Overflow is rejected
before input access or allocation. Ordinary single-player allocates nothing.

ASan/UBSan repeats the complete suite successfully. Eight negative controls
across the two layouts detect altered defaults, corrupt value indexes,
premature representation publication and altered repeat-count wire bytes.
The EE ABI compilation asserts exact unsigned-word round trips through signed
32-/16-bit extrema and all bits of both sector flag types. Software, Hardware,
and full-side Hardware syntax checks pass.

Native logs, source extracts, allocator results and wire hashes are in
`build/nov14-next5/side-memory*`. Wire files are deleted after literal comparison
unless `--keep-wire` is requested. The EE ABI source/object/constants are in
`side-memory`; isolated production assembly and frame comparisons are in
`side-objects`. Frozen production sources, hashes and the isolated fifth-pass
diff are in `build/nov14-next5/side-production`.

## Memory evidence and integration fixtures

The following figures are measured with the real native allocator, including
64-bit pointers, its native header/rounding and the unchanged other snapshot
allocations. The side structures themselves contain no pointers and are 24/4
bytes on both the native and EE ABIs. These are synthetic construction results,
not whole-level EE RAM or resident corpus totals.

| Synthetic ordinary-side case, one-sided arena | Before peak | After peak | Before retained | After retained |
| --- | ---: | ---: | ---: | ---: |
| 31 sides, all defaults | 2,448 B | 1,824 B | 2,224 B | 1,600 B |
| 1,200 sides, one changed value each | 53,856 B | 34,656 B | 53,632 B | 34,432 B |
| 25,100 sides, all defaults | 1,105,456 B | 603,456 B | 1,105,232 B | 603,232 B |
| 25,100 sides, two changed values each | 1,105,456 B | 804,256 B | 1,105,232 B | 804,032 B |
| 70,001 sides, all defaults | 3,081,120 B | 1,681,104 B | 3,080,896 B | 1,680,880 B |
| 70,001 sides, all six values changed | 3,081,120 B | 3,081,120 B | 3,080,896 B | 3,080,896 B |

`SIDE_COMPACT.pk3` and `SIDE_FIXED.pk3` in
`build/nov14-next5/side-addons` each have the same 12-sector, 31-line,
48-side MAP99 geometry. The first keeps the original one-/two-texture sides:
82 copied words and EE side payload **1,152→520 bytes**. The fixed fixture
gives every side two signed offsets and three valid texture names, reaching
exact equality at 1,152 bytes and retaining the full layout. Both use supported
UDMF fields and valid resources. Generated expected loader metadata accompanies
each add-on.

Load each independently with `-file <addon> -warp 99 -keepspawn -loadhash -zck`.
Integrated loader hashes, complete render frames, final linked code, map RAM
and stack measurements are the coordinator's engine checks. The component
proofs above make no whole-game FPS, VRAM or universal peak-RAM claim.
