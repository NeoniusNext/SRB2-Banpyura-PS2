# Sprite-frame metadata memory — eighth pass, 2026-11-15

The comparison source is the completed preceding pass,
`bd1160aaf3466ee8bcbb28a92f2116c1b8b64b2a`, frozen in
`build/nov15-next8/baseline-src`. The sole production edit is the member order
of the internal PS2 `spriteframe_t` in `r_defs.h`.

## Representation and lifetime

The full `UINT16 flip` mask now follows the one-byte `rotate` selector, using
its existing alignment padding. The sixteen full-width lump numbers and
sixteen existing `UINT16` header-table identifiers retain their offsets.
No field is narrowed, removed, reconstructed or shared with mutable state.

Actual EE GCC/header results:

| Property | Before | After |
| --- | ---: | ---: |
| `sizeof(spriteframe_t)`, normal `ROTSPRITE` | 108 B | 104 B |
| Size without `ROTSPRITE` | 104 B | 100 B |
| Natural alignment | 4 B | 4 B |
| `rotate` offset | 0 | 0 |
| `flip` offset | 100 | 2 |
| `lumppat` / `lumpid` offsets | 4 / 68 | 4 / 68 |
| Optional rotated-table pointer offset | 104 | 100 |

All element, word and pointer accesses remain naturally aligned. The frame
metadata is CPU data, with no DMA/SIMD consumer. Texture pixels and their
alignment are independent. `PS2_NOOPT_SPRFRAME_LAYOUT` restores the preceding
layout. The preprocessed non-PS2 header interface is byte-identical.

The source audit finds no positional initializer, raw object comparison,
serialization, fixed byte offset, or externally loaded binary-plugin consumer
of `spriteframe_t`. All readers use typed fields. The loader's clears, copies,
allocation requests and dynamic 64-to-256-frame scratch growth already use
`sizeof(spriteframe_t)`. Skin compatibility arrays copy `spritedef_t` pointers;
skin fallback selects an existing definition without allocating frame copies.
Lua/HUD and Software/Hardware consumers continue to read the same fields.

Every allocation stays at the same construction point, with the same tag,
owner, replacement/free order and lifetime. There is no added allocation,
resize, final shrink, capacity limit, fallback, retry, cache-purge rule or
failure path. The existing 256-frame support, all sixteen viewing angles,
full flip mask, dynamic limits, sprite-header capacity, skins and add-ons stay
available. Smaller metadata requests can still change heap placement, so the
coordinator's same-map stress and complete-image checks are required before
acceptance. Composite recovery and pack-readiness code are untouched.

## Memory and code scope

The prepared-stock loader replay has **3,360 frames in 587 nonempty frame
roots**. It reads metadata/patch headers from all four unchanged stock packs
and executes the actual production frame constructor in engine order.

| Replay scope | Frames | Frame roots | EE payload before | EE payload after |
| --- | ---: | ---: | ---: | ---: |
| Global sprite definitions | 2,813 | 391 | 303,804 B | 292,552 B |
| Six skins, normal and super definitions | 547 | 196 | 59,076 B | 56,888 B |
| Total | 3,360 | 587 | 362,880 B | 349,440 B |

The exact replay frame-payload saving is **13,440 B**. Applying the unchanged
EE 16-byte header and minimum 16-byte block rounding to each replay frame root
gives **375,152 → 361,152 B**, a 14,000-byte minimum-block difference. Alignment
gaps, absorbed tails, other game allocations and allocation topology are
separate. These replay values are not presented as a whole-engine checkpoint
or a universal peak-RAM guarantee.

The normal static 64-frame scratch array also shrinks **256 B**. If full
dynamic limits are activated, its existing 256-frame heap scratch request is
1,024 B smaller. The latter is conditional and must not be added to ordinary
stock savings. On the native 64-bit host, eight-byte pointer alignment keeps
the normal frame stride at 112 B in both versions; native results are not
substituted for EE savings.

Isolated default-release EE `-O2` objects for `r_things.c`,
`r_patchrotation.c`, `ps2_limits.c` and `r_skins.c` retain exactly the same
summed text, rodata, writable data and function frame sizes. `r_things.c` BSS
changes 65,168 → 64,912 B. The opt-out's every allocated object section
matches the frozen baseline byte-for-byte. These are isolated object results,
not final-LTO code-size or runtime/FPS measurements.

## Exactness proof

`next8_memory_hosttest.{c,py}` extracts the complete actual frame installer,
name parsers, mirror/check helpers, sprite index, `R_AddSingleSpriteDef`,
scratch-growth statements and rotated-frame cache accessor/constructor. It
uses actual `z_zone.c` and `ps2_mem.c`, retaining the shared zone fixture's
boundary stubs and independent arena checker. Pack metadata/header reads and
the image-rotation kernel are controlled boundaries; this fixture does not
replace the full-game renderer comparison.

Frozen/current/opt-out and current ASan/UBSan each pass **69 cases and
6,162,003 checks**. The complete **1,285,276-byte** normalized field/cache
output agrees literally, including the prepared-stock replay. Padding and
process addresses are excluded; every logical field and all sixteen identifier
slots are retained. The output SHA256 is
`83256fd9fd5727575f0d1c2525b7b6ac8ca50877093824aa2a2caab1f14891ba`.

The matrix covers arrays of 1 through 65,535 records, both arena directions,
red zones, hostile initial bytes, neighboring canaries, all flag bits, full
32-/16-bit identifiers and pointer identity, short/long names, all rotation
policies, mirrored slots, invalid/empty inputs, add-on replacement, 64/256-frame
growth, cache hits, every roll angle and pivot/default selection. Actual
rotated-cache writes and copied table identity are checked; all fixture roots
are released and the real arena checker passes. Each changed request is
smaller or equal; no tested used/peak/request increases on the native ABI.
Four compiled and executed negative controls alter a flip
bit, a high lump-number bit, a loader copy length and a rotated-cache slot;
each is rejected by assertions or the complete literal comparison.

Actual EE32 runtime results and the coordinator's full-engine integration
results are separate from the host and isolated-object checks. The actual
EE32 frozen/current/opt-out witnesses each pass all **69 cases**, with
108/104/108-byte frames, four-byte alignment and 16-byte real zone headers.
Each complete **1,285,276-byte** snapshot equals the frozen native oracle
literally. Current performs **6,160,471 checks**, while frozen/opt-out perform
6,160,477; heap-walk check counts differ with the changed block layout.

The complete stock replay's actual EE arena used/peak bytes change
**441,552 → 427,520 B**, saving **14,032 B**. This includes the replay's
unchanged other allocations and physical layout; it is separate from the
13,440-byte payload and 14,000-byte minimum-frame-block calculations. No
target row has a larger request, used value or peak, and every opt-out row
matches the frozen baseline. Runtime results, source/oracle/ELF hashes and
all three successful locked runner logs are in `memory/ee/report.json` and
its sibling directories.

## Reproduction

```sh
source /workspace/.onboarding/environment.sh
python3 -B tools/ps2/next8_memory_hosttest.py --sanitize --negative-controls
SRB2_PS2_OUT=build/nov15-next8/baseline/software \
  python3 -B tools/ps2/next8_memory_objectcheck.py
SRB2_PS2_OUT=build/nov15-next8/baseline/software \
  python3 -B tools/ps2/next8_memory_eetest.py
source /workspace/.onboarding/emulator-env.sh
SRB2_PS2_OUT=build/nov15-next8/baseline/software \
  python3 -B tools/ps2/next8_memory_eetest.py --analyze-only --run
```

The EE fixture reserves a static 16 MiB backing arena once on the standard
32 MiB machine, reinitializing the unchanged real allocator for each case;
the largest two-table payload is 14,155,560 B on EE, so all 69 cases fit
without reducing their coverage. Declared alignment slack and exterior guards
are checked independently over all 1,024 possible 64-byte base residues.
Every exterior prefix/suffix byte is filled and checked before reuse and
after the final case. Native backing retains the original malloc/free path.
An earlier repeated-libc-backing attempt failed at its test arena reservation;
its exact cause remains undiagnosed and the failed trace is retained under
`memory/ee/failed-libc-backing`. No game allocation result is inferred from
that fixture reservation failure. The final fixture uses the real zone's
ordinary single-reservation model and passes its entire matrix.

Independent review is bound in
`build/nov15-next8/review/memory-backing-review.json`. Fixture-only unused,
sign-comparison and clobbered warnings retain their existing suppressions;
array-bounds diagnostics remain enabled, and all final EE fixture compile logs
are empty. Production object checks retain default release warning flags.
Runtime launches use the existing locked `run_pcsx2.py` path.
Raw source extracts, input hashes, native/sanitizer logs,
literal snapshots and EE ABI/object results live under
`build/nov15-next8/memory`. The coordinator records final linked builds,
unchanged-arguments zero-warning MAP11 pressure acceptance and complete game
images in the eighth-pass integration report. No whole-game FPS, VRAM or
physical-console timing result is inferred from this metadata change.
