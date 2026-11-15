# Spawn snapshot memory, third pass — 2026-11-14

The comparison baseline is the completed second pass saved in
`build/nov14-next3/baseline-src`. Production changes are limited to `p_setup.c`,
`p_setup.h` and the `ArchiveLines` reader in `p_saveg.c`.

## Included change

The PS2 network world snapshot previously kept ten full-width arguments in
every line's `spawnline_t`, including lines whose arguments are all zero.
The loader now counts the populated vectors before making its existing single
line-snapshot allocation. It uses private compact records plus copied argument
vectors only when their complete allocation is smaller. Dense maps keep the
preceding fixed array. There is no additional allocation, reserve, resizing,
post-load compaction, argument interning, or shared writable argument storage.
The other snapshot allocations retain their preceding order.

EE GCC, compiling the production headers, emits `sizeof(spawnline_t) = 60`,
`sizeof(compactspawnline_t) = 24`, and four-byte record/argument alignment. With
`N` lines and `K` nonzero argument vectors, the line-snapshot payload is

```text
before = 60*N
after  = min(60*N, 24*N + 40*K)
```

The compact representation is selected only for a strict decrease; equal and
dense cases retain the original layout. The fixed-array multiplication is
checked before reading inputs, and the compact sum is checked before allocating.
The representation flag is published
after the replacement root has been allocated successfully, so an allocation
failure cannot label the preceding root with the new representation.

`P_GetSpawnLine` reconstructs one compact record into caller-owned scratch for
the network archive. In the fixed case it returns the original record. All ten
signed 32-bit argument values, signed special, executor delay, portal, and
existing string ownership survive. An implicit zero vector is reconstructed
with `memset`; it never points at the writable live `lineargs_zero`. The public
`spawnline_t` layout and all save/network wire fields remain unchanged. The
accessor's only production consumer is `ArchiveLines`, whose scratch lives
through every comparison and write of that line.

The cost is an extra scan of the ten arguments per line while building a
snapshot, plus a 60-byte `spawnline_t` scratch object during EE line archiving.
Compact archives reconstruct that object's arguments and metadata for each
line. The fixed representation returns its existing record. These paths are
map loading and network state serialization; no runtime FPS gain is claimed.

Snapshots are created for **network games and `-keepspawn`**. Ordinary single
player already skips all snapshots and gets no RAM saving from this change.
Defining `PS2_NOOPT_SPAWNLINES` selects the preceding fixed allocation.

## Verification

```sh
source /workspace/.onboarding/environment.sh
python3 -B tools/ps2/next3_spawn_hosttest.py --negative-controls
python3 -B tools/ps2/next3_spawn_hosttest.py \
  --out build/nov14-next3/spawn-memory-asan --sanitize --negative-controls
SRB2_PS2_OUT=build/nov14-next3/memory-syntax python3 -B tools/ps2/build.py \
  --syntax --jobs 1 src/p_setup.c src/p_saveg.c
SRB2_PS2_HW=1 SRB2_PS2_OUT=build/nov14-next3/memory-hw-syntax \
  python3 -B tools/ps2/build.py --syntax --jobs 1 src/p_setup.c src/p_saveg.c
python3 -B tools/ps2/next3_spawn_fixture.py
```

The host fixture extracts and executes the actual production snapshot
constructor, enable condition, archive comparisons, `ArchiveLines`, save-buffer
writes, and zone/arena. Its 768 cases combine twelve sizes (including zero,
25100, and 70001 lines), sparse/dense/threshold argument counts, one-/two-sided
arenas, red zones, and network/`-keepspawn` modes. Both extreme signed argument
values occur in every position. Every line snapshot is checked before and
after live argument mutations; complete sector/side snapshots are also checked.
Unrelated side-diff selection/writes are deterministic stubs; sector archiving,
the network transport and live server/client synchronization are outside this
standalone fixture.

The two production line archives per case are compared as literal framed
bytes, not only as digests. All **387199872 bytes** match the preceding
implementation and the old-layout switch. The stream SHA256 is
`27338b45b4cb686fce4d54c5dfa7727f9d6b43d4517634fdc3590139563ebe92`.
Large temporary wire files are removed after their literal comparison unless
`--keep-wire` is requested; source extracts, build/test logs, hashes and results
remain in the output directory.

No measured request, retained byte count, allocation count or construction
peak increases in the 768 arena comparisons. Four production snapshot
allocation sites are fault-injected for both sparse and dense inputs; level
release and a subsequent reload leave no level allocations behind. Additional
failed replacements exercise both representation transitions, check that root
and mode stay together, then retry successfully. Real cache eviction clears its
owner while preserving the level snapshots. Ordinary single player performs
zero snapshot allocations. Overflow is refused before input access. Negative
controls are rejected for a corrupt implicit zero vector, a live argument
alias, a changed wire argument, and premature representation publication.

ASan/UBSan and Software/Hardware target syntax checks pass. The native numbers
below use the actual 64-bit host allocator, including its 32-byte header,
native pointer widths and native alignment policy. They isolate snapshot
construction with small existing level/static allocations; they are **not EE
whole-level RAM measurements**. `K=1255` is a synthetic five-percent population,
not a measured count from MAP11.

| Native fixture, one-sided arena | Before peak | After peak | Before retained | After retained |
| --- | ---: | ---: | ---: | ---: |
| 31 lines, zero vectors | 3440 B | 2448 B | 3216 B | 2224 B |
| 25100 lines, zero vectors | 1808400 B | 1005200 B | 1808176 B | 1004976 B |
| 25100 lines, 1255 populated vectors | 1808400 B | 1055408 B | 1808176 B | 1055184 B |
| 25100 lines, all populated | 1808400 B | 1808400 B | 1808176 B | 1808176 B |
| 70001 lines, zero vectors | 5041280 B | 2801248 B | 5041056 B | 2801024 B |
| 70001 lines, all populated | 5041280 B | 5041280 B | 5041056 B | 5041056 B |

The generated add-ons `SP_COMPACT.pk3` and `SP_FIXED.pk3` contain identical
31-line MAP99 geometry. The first has zero argument vectors and selects compact
storage; the second sets unused `arg9` on every special-zero line and selects
the fixed layout. Each is loaded separately with
`-file <addon> -warp 99 -keepspawn -zck -loadhash -zheap 1`.
The EE requests are respectively 1860 → 744 bytes and 1860 → 1860 bytes before
the shared zone header/rounding. `-server` also enables snapshots, while
`-keepspawn` exercises their memory without a network peer. Integrated game
RAM, render frames, level hashes, and any baseline OOM are recorded by the
coordinator. These metadata results establish no FPS or VRAM gain.

## Explored but excluded

Narrowing the texture-patch alpha-style enum looked promising before checking
the actual EE allocation classes. Production booleans are one byte, and the
single-patch descriptor is already small enough that this narrowing alone
would leave its 64-byte arena block unchanged. No texture metadata change was
included. Previously rejected sprite-table compaction, scratch-column removal,
and unsafe decompression work remain excluded.
