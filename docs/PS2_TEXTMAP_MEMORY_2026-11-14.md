# TEXTMAP memory continuation — 2026-11-14

Changes are limited to PS2-profile map loading in `p_setup.c` and
`m_tokenizer.c/.h`. No map formats, parsed parameters, graphics, physics,
network formats, limits or add-on paths are removed. Defining
`PS2_NOOPT_TEXTMAPMEM` selects the preceding allocation behavior for comparison.

The fallback count pass, used for comments and extended top-level blocks,
formerly reserved 32767 positions on its first encounter with each of the five
block types. All five tables then stayed in `PU_LEVEL` until the next level.
The PS2 path retains the same 32767-entry initial reservation and 1024-entry
growth steps, checks the allocation-size arithmetic, and frees each table after
its type's parse loop. It uses the size of a position (`UINT32`), rather than the
size of its pointer; these are both four bytes on the EE. The native profile's
existing allocation behavior remains unchanged. A proposed smaller initial
table was removed: extra moving resizes can increase the construction peak on
large or interleaved fallback maps.

The ordinary one-pass scan now resizes its arrays through `Z_Realloc`, allowing
the arena to extend them in place. Its metadata belongs to `PU_LEVEL` with
owner slots, so an abandoned level's cleanup also releases the tables and
clears their pointers. The next count resets the stale counts/capacities; a
fallback table with a cleared pointer restarts at the original capacity. Block
types are freed after their final consumer builds the per-type index lists.
No final shrink requires an extra copy. A rejected oversized scan frees its
metadata and takes the existing fallback parser.

## Verification

```
source /workspace/.onboarding/environment.sh
mkdir -p build/memory-sol
python3 -B tools/ps2/memory_textmap_hosttest.py --out build/memory-sol/textmap --negative-controls
python3 -B tools/ps2/memory_textmap_hosttest.py --out build/memory-sol/textmap-asan --sanitize --negative-controls
python3 -B tools/ps2/tokenizer_hosttest.py build/memory-sol/textmap/ordinary.TEXTMAP build/memory-sol/textmap/comments.TEXTMAP build/memory-sol/textmap/extension.TEXTMAP
```

The dedicated fixture executes production counting, fallback pair parsing,
one-pass scan, tokenizer wrappers and the real zone allocator. Seven fixtures
compare exact ordered parser-stream digests/counts against the preceding path:
ordinary, commented, extended, empty types, no things, and large interleaved
maps crossing 32767 and 1024-entry boundaries. A large fixture has 170050 blocks
and 340100 pairs. Additional checks cover in-place and forced-moving growth,
overflow refusal, owner clearing, and an abandoned ordinary scan followed by a
fallback and another successful scan. ASan and UBSan pass. Negative controls
retain the position roots, shift fallback positions, or make scan metadata
static/ownerless; the respective lifetime assertions or exact-stream comparison
reject them.

These are **64-bit native host arena measurements**, including its 32-byte zone
headers. Both comparison paths use four-byte positions: the driver additionally
compiles the old fallback with its pointer-size expression normalized to a
position's size. This prevents the old x64 over-allocation from concealing an
EE allocation-peak regression. The actual unchanged x64 expression is also
tested for parser equivalence. The table is not a measurement of EE headers or
integrated game RAM.

| Fixture | Before peak | After peak | Before retained after parsing | After retained |
| --- | ---: | ---: | ---: | ---: |
| Ordinary 12-room map | 31120 B | 31104 B | 0 B | 0 B |
| Same map, comments | 668400 B | 668400 B | 655520 B | 0 B |
| Same map, unknown extension | 668416 B | 668416 B | 655520 B | 0 B |
| Large ordinary scan | 19234752 B | 16769392 B | 0 B | 0 B |
| Large commented map | 9318416 B | 9318416 B | 696480 B | 0 B |
| Commented map with one populated type | 133520 B | 133520 B | 131104 B | 0 B |

Full comparisons additionally pass with the one-sided allocator and with red
zones: all seven ordered streams match and no allocation peak increases in any
tested mode. On the EE, the five minimum fallback tables contain 655340 bytes
of position payload in total; that payload is now released after parsing.

The existing independent tokenizer differential suite reports **ALL EQUAL**:
69168 copy/length cases, 599452 accepted one-pass texts and 1400551 refused
texts. Its original token-reader comparison also passes. The new dedicated
suite's large streams use an unbounded running digest, rather than a truncated
text log.

Native game fixtures are generated with the existing tool:

```
python3 -B tools/ps2/make_udmf_map.py --out build/memory-sol/addons --map 99 --name UM_FAST
python3 -B tools/ps2/make_udmf_map.py --out build/memory-sol/addons --map 99 --name UM_FALLBACK --comment
```

They contain the same 12-room MAP99 geometry (20 vertices, 12 sectors, 31 lines,
48 sides, 15 things) and valid XGL3 nodes; the comment selects the fallback.
The coordinator records integrated EE builds, level hashes, frame comparisons
and RAM results separately. Host allocation figures do not establish an FPS
gain, physical-console timing, or success on arbitrary large maps. The existing
tokenizer input/token storage remains static; this change does not claim to
make every loader allocation transactional after an OOM.

## Rejected candidate

Shrinking the global sprite-header table after startup would reduce resident
RAM, and all consumers retain integer IDs. However, a later add-on may need to
move the compacted contiguous table after another allocation occupies its free
tail. Its old-plus-new peak can exceed the preceding implementation, which
already had enough reserved entries. This can introduce a new add-on OOM.
The exploratory sprite changes were completely removed; no sprite compaction
flag or modified sprite behavior remains.
