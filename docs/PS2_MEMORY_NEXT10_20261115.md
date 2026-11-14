# Rejected drawseg storage candidate — tenth pass, 2026-11-15

**The 420-byte drawseg candidate is rejected.** It passes exactness tests and
saves 24 bytes per retained slot, but a matched-local-seed 350-frame run adds
2,371 evictions, 109,404,512 evicted bytes, 719 failed attempts and 11,281 ms.
Restoring only 444-byte drawsegs, with all other candidate changes retained,
restores the reference zone/cache metrics and elapsed time. No tenth-pass
drawseg memory saving is retained.

The reference is source `5134b87aeb382a7e156acb8a85462bd9059c096a`, frozen in
`build/nov15-next10/baseline-src`. The memory candidate changes only the
PS2 `drawseg_t.leftpos` and `rightpos` member types in `src/r_defs.h`.
Rejected candidate SHA256 is
`e58fccb5ada224dafacc95398da41f2a6f683ce9f61c084ec6a487935930844e`.
The coordinator restored production `r_defs.h` byte for byte to the reference,
SHA256 `6c88912e24bb3f85cc8d0166f5734ad5e01e42dfe0bad3ae266e91cba831f62c`, in
source commit `e87421615`. The original candidate and all proof evidence remain
reproducible separately. Final validation of the other retained changes is the
coordinator's separate gate.

## Unseeded observations, not causal evidence

The 24-byte component saving and its exactness proof remain valid. The original
unseeded MAP11 `-keepspawn` 350-frame traces show different cache/rebuild work,
but their local-effect inputs were not fully controlled. **Causal regression
and rejection claims based on those unseeded traces are withdrawn.** They are
retained as observations, not as evidence that any optimization caused the
difference. The rejection above rests on the later fixed-seed counterfactual,
not this unseeded matrix.

All non-time zone `ZCK` checkpoints through precache agree, and the loader
hashes match. That does not establish identical precipitation positions or
local sound selection during play.

| 350-frame metric | Frozen reference | Combined candidate |
| --- | ---: | ---: |
| Arena | 24,743,936 B | 24,743,936 B |
| Used zone | 24,559,152 B | 24,405,680 B |
| Peak zone | 24,734,944 B | 24,730,224 B |
| Free zone | 184,784 B | 338,256 B |
| Largest free run | 32,832 B | 73,200 B |
| Evictions | 19,606 | 21,977 |
| Evicted bytes | 849,579,632 B | 958,984,144 B |
| Failed attempts | 2,513 | 3,232 |
| Engine elapsed `t_ms` | 106,885 | 118,167 |

The raw difference is **2,371 evictions, 109,404,512 evicted bytes, 719 failed
attempts and 11,282 ms**. It is not attributed to the source change while the
local-effect input differs, and is not a physical-console FPS measurement.
Reference ELF is
`9ccdd6c2b5b3a3dfc8de212742b4acd48a021174203872288a71b65d8919e5d8`;
combined candidate ELF is
`b7befc5802aaa467ce56c332e6a0ddd0cffaa982c019db40782680af4d7db358`.
Raw paired results are in `next10-pressure-350-report.json`.

The first source opt-out defines `PS2_NOOPT_DRAWSEG_XY` to restore 444-byte
drawsegs while keeping all other candidate changes, the same embedded
`e7131dc7f` source metadata and all 189 freshly compiled canonical IR inputs.
Its ELF is
`a5fdcb8ebd0f0e75e6c85127b7ea29708b47d1a2b528a2c1be77ffb3fde05f63`.
The complete observed matrix is:

| Unseeded 350-frame run | Used zone B | Evictions | Evicted B | Failed | `t_ms` | Sole `PU_SOUND` payload B |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Reference | 24,559,152 | 19,606 | 849,579,632 | 2,513 | 106,885 | 331,680 |
| Combined candidate | 24,405,680 | 21,977 | 958,984,144 | 3,232 | 118,167 | 331,330 |
| Drawseg opt-out | 24,274,064 | 20,367 | 886,396,816 | 2,671 | 111,195 | 331,330 |
| I/O opt-out, compact drawsegs retained | 24,405,680 | 21,977 | 958,984,144 | 3,232 | 118,197 | 331,330 |
| Drawseg and I/O opt-outs | 24,274,064 | 20,367 | 886,396,816 | 2,671 | 111,205 | 331,330 |
| All three Software-visible opt-outs | 24,559,152 | 19,606 | 849,579,632 | 2,513 | 106,885 | 331,680 |

The sound-size distinction led to the uncontrolled input. The stock SRB2 pack
contains these DMX source lumps:

| Source lump | Decoded source-lump bytes | Plus 56 B cached-sample metadata |
| --- | ---: | ---: |
| `Sounds/SRB2/DSATHUN1` | 331,274 | 331,330 |
| `Sounds/SRB2/DSATHUN2` | 331,624 | 331,680 |

Those sizes exactly match the differing sole `PU_SOUND` payloads. The ambient
thunder selector is `sfx_athun1 + M_RandomKey(2)` at
[`p_mobj.c:11859`](../src/p_mobj.c#L11859). `-singletics` fixes only the synced
`P_SetRandSeed(0x1B2D3F5u)` at
[`g_game.c:5136`](../src/g_game.c#L5136). The separate local `M_Random` stream
is seeded through `M_RandomSeedFromOS()` at
[`d_main.c:1598`](../src/d_main.c#L1598), falling back to `time(NULL)` on the
next line. Its PS2 entropy input mixes `GetTimerSystemTime()`, `cpu_ticks()`,
the calendar and a counter at
[`i_system.c:560`](../src/ps2/i_system.c#L560). Precipitation height also uses
`M_RandomRange` at [`p_mobj.c:11736`](../src/p_mobj.c#L11736). Matching the synced
seed and loader hashes therefore does not match every local effect. Event IDs
and the local seed were not recorded in these historical traces, so the
sound-size identification does not replace a complete event journal.

Run provenance was checked separately. Initial `.srb2/reference.cfg` bytes match
literally (76 B, SHA256
`de0d14f426449aa4b0d57074c8a3d22ad387eeb27d004b0e8ad7dc55029063c4`), as do
the final 8,860 B config, all 145 argument-file bytes and all five asset files.
Logs select the same PCSX2 2.6.3 slot0, European 2.00 BIOS, 32 MiB RAM, PAL50,
target speed 1 and null-audio fallback; neither ELF has a per-game settings file.
The complete historical pressure-run PCSX2 ini was not archived, so its exact
contents cannot be retrospectively asserted. Archived before/after Software
witness settings and the current settings agree on every emulation key; their
only current difference is a game-list UI column state.

For the drawseg-and-I/O opt-out, even the reported heap start, brk and every
non-time libc checkpoint match the reference: heap start `0x76065c`, brk
`0x1f5a000`, precache outside-arena use 380,732 B and free chunks 14,952 B. Both
also report identical spill totals: 680,448 B in seven live blocks, 713,216 B
peak, nine allocations, zero refusals and zero foreign-thread frees. Absolute
zone base, individual allocation addresses and initial frontier address were
not logged. The final frontier is an offset from that unlogged base, not its
absolute address. Mixer/decoder threads cannot directly spill into the zone:
`Spill_Allowed` in `ps2_spill.c` permits only the game thread. Sample reclaim
can depend on voice-busy state; existing counters do not prove identical
allocation order. No claim of an observed absolute-phase or asynchronous-spill
cause is made.

The controlled program uses the two predeclared seeds **`0x01B2D3F5` and
`0x9E3779B9`**, paired 350-frame traces, controlled complete-frame comparisons
and unmodified-game smoke runs. Fixed-M-seed controls patch test ELFs in place
without changing their layout or normal game source. Rain, thunder, audio and
all other effects remain enabled. No padding, map-specific feature switch,
reserve reduction or cache-policy tuning is substituted for matching inputs.

## Decisive fixed-seed counterfactual

At fixed local seed **`0x01B2D3F5`**, the first paired 350-frame run completes
with matching loader hashes, no quality errors and the same sole `PU_SOUND`
block: **331,696 B physical / 331,680 B requested** in all three ELFs. The local
RNG is controlled. The layout-only opt-out retains the other candidate source
changes and embedded metadata, and restores the reference zone/cache results:

| Controlled 350-frame metric | Reference | Compact 420 B | Layout-only opt-out 444 B |
| --- | ---: | ---: | ---: |
| Arena | 24,743,936 B | 24,743,936 B | 24,743,936 B |
| Used zone | 24,559,152 B | 24,406,016 B | 24,559,152 B |
| Peak zone | 24,734,944 B | 24,730,224 B | 24,734,944 B |
| Free zone | 184,784 B | 337,920 B | 184,784 B |
| Largest free run | 32,832 B | 73,200 B | 32,832 B |
| Evictions | 19,606 | 21,977 | 19,606 |
| Evicted bytes | 849,579,632 B | 958,984,144 B | 849,579,632 B |
| Failed attempts | 2,513 | 3,232 | 2,513 |
| Engine elapsed `t_ms` | 106,885 | 118,166 | 106,884 |

This is **2,371 extra evictions, 109,404,512 extra evicted bytes, 719 extra
failed attempts and 11,281 ms more engine elapsed time** in a controlled pair.
It cannot be dismissed as the earlier sound-size mismatch. The same-seed
444-byte counterfactual removes those extra cache costs while retaining the
other candidate changes. The one-millisecond reference/control elapsed
difference is not advertised as an FPS gain. This is sufficient to reject the
420-byte change for the stated performance goal despite its correct component
representation and smaller root request.

This establishes the effect of the change as built; it does not identify one
specific allocation event as the trigger or prove that every map/hardware
configuration would regress. The control restores the listed zone/cache
`ZSTAT` metrics, not every process byte: its libc free/peak values still differ
by 384 B and its final frontier offset differs by 128 B. No arbitrary padding,
map-dependent switch, feature reduction or allocator-policy adjustment is
introduced to retain the candidate. The coordinator's second-seed, final-image
and smoke validation of the remaining retained changes is separate from this
completed rejection decision.

Read-only input verification checks:

- Staged seeded reference ELF SHA256
  `7a1c5a20d0047ec31d3c8df2561a6e82e025f203b982e80f2251bee03f654dbb` and combined
  ELF `ae08d93c9d4db166de43075efa5191cb6d7c32ea617efc87cea8255ac4e10d24` match
  their diagnostic patch manifests and the executed run report. The executed
  444-byte control is
  `f01874d0b249b9ba071c4a4089c518fb3aa9328b4ebc92d2b678de2fbc300318`.
- The complete 212-byte fixed-seed initializer is byte-identical across all
  three, SHA256
  `1c00f1a0d994afbb24c9925d41abf089350576e6e7998171557c9ca6382f8fc0`.
  Its 40-byte stub calls the actual unchanged `M_RandomSeed(UINT32)` with
  `0x01B2D3F5`, preserves the 32-byte frame/RA slot and returns true; the remaining
  172 bytes are NOP fill. Every byte outside the original initializer extent,
  ELF length, symbol address and symbol size remains unchanged within each
  diagnostic copy. No production binary is patched in place.
- All six core RNG object sections (`M_RandomSeed`, `M_RandomKey`,
  `M_RandomRange`, `M_RandomByte`, `M_RandomFixed`, `M_RandomizedSeed`), their
  complete ordered relocations and the 16-byte initial state agree across
  reference, combined and 444-byte control. Resolved global-state addresses may
  differ between original ELFs; that does not change the SFC32 algorithm.
- Original engine object/ELF/map hashes remain valid; initial config and all
  argument bytes match. These checks establish common initialization and an
  unchanged generator, not a claim that a buggy consumer could never advance
  the RNG differently later. The planned complete-image controls remain needed.

Raw run data is `build/nov15-next10/matched-seed1/next10-pressure-350-report.json`
and `matched-drawseg-old-seed1/next10-pressure-350-report.json`;
patch bindings are under `build/nov15-next10/local-rng/{baseline,combined,drawseg-old}`.
The patch method's independent ABI review is in `review/local-rng-patches.json`.
The final memory decision and metric/source pins are in
`build/nov15-next10/memory/rejection-decision.json`; the earlier component
`summary.json` remains an unchanged historical stage record.

## Full-width XY and unchanged map vertices

Both renderer intersections store and use only `x/y`. Their former `vertex_t`
members also carried heights and height-presence flags that no drawseg consumer
ever reads or writes. The rejected PS2 candidate uses `vector2_t`, containing two
full-width `fixed_t` coordinates. The source geometry expressions, ray kernels,
floating-point fallbacks, endpoint writes and clipping/FOF/slope reads are
unchanged. There is no coordinate reconstruction, narrowing or rounding change.

Actual default-release EE GCC/header evidence:

| Property | Before | Candidate |
| --- | ---: | ---: |
| `sizeof(drawseg_t)` | 444 B | 420 B |
| Each endpoint size | 20 B | 8 B |
| Every endpoint `x/y` field | 4 B | 4 B |
| Drawseg natural alignment | 4 B | 4 B |
| Persistent `sizeof(vertex_t)` | 20 B | 20 B |
| Persistent `sizeof(polyobj_t)` | 196 B | 196 B |

Persistent map vertices retain their complete coordinates, floor/ceiling heights
and presence flags, with identical layout and lifetime. UDMF vertex slopes,
Software/Hardware rendering, Lua fields/indices/invalidation, save/network
indices and polyobjects remain available. No map/add-on/resource/geometry or
drawseg capacity limit changes. `PS2_NOOPT_DRAWSEG_XY` restores the former PS2
member types. The complete preprocessed non-PS2 header interface is identical.

There are 18 nondeclaration endpoint reference lines in the complete source;
all are typed `.x` or `.y` accesses in `r_segs.c`. There is no whole-endpoint
assignment/address escape, positional drawseg initializer, raw serialization,
raw comparison, fixed byte stride, assembly offset, DMA input or separately
loaded binary consumer. Every other drawseg field, including all forty entries
in both FOF pointer arrays, stays present and retains its full preceding width.

## Allocation and savings scope

The existing drawseg root still starts at 128 entries and grows by 1.5 times.
Its allocation uses `newmax * sizeof(*drawsegs)` at the same construction point,
with the same tag, owner, failure diagnostics and lifetime. Existing typed
indices repair `ds_p`, `curdrawsegs` and `firstseg` when the root moves. Per-slot
frontscale roots retain their own high-water widths and are freed by the same
level-transition teardown. No owner points inside the moving drawseg array.

For retained capacity **C**, the rejected root requests **24 × C B less payload**:
3,072 B at its initial 128 entries and 15,552 B at 648 entries. Counts/capacities
are unchanged. These are component payload savings, not a universal total-RAM,
fragmentation, peak, eviction, FPS or frame-time claim. Allocator block rounding,
alignment gaps, absorbed tails, linked sections and complete-game cache placement
are separate quantities. These savings are not present in the restored shipping
layout. The controlled cache regression demonstrates why component payload
savings and a nonincreasing synthetic peak do not establish a whole-game RAM or
performance benefit.

## Host proof and explicit boundaries

`next10_memory_hosttest.{c,py}` extracts the actual drawseg declaration, complete
growth block/pointer repair, frontscale allocation/release functions, slope-wall
intersection block and complete rational ray helper graph. It runs the actual
`z_zone.c` and `ps2_mem.c`. It also replays the unchanged persistent vertex
declaration and actual Doom/UDMF loaders/default loop, extended-node reallocation,
Lua getter/index, slope vertex selection, polyobject growth/copies and loadhash.
Archive decoding/tokenization, Lua VM services, the downstream slope geometry
kernel and the trigonometric table service are controlled boundaries.

Frozen/current/opt-out and current ASan/UBSan each pass **201 cases**. Frozen and
opt-out execute **30,058,102 checks** each; candidate and sanitizer execute
**30,058,117** because physical heap walks differ. All **38,152,928 literal
normalized bytes** match, SHA256
`b37e7b07c5df4847f6832de88fa86aa67a49a1fbbbfdfdb116da8aa2d001b5a0`.
Padding, process addresses and the former unread endpoint heights/flags are not
logical output. Every remaining drawseg scalar, full coordinate bit and pointer
identity is checked. All forty entries in both FOF arrays are serialized as
stable identities.

The matrix includes:

- One through 8,192 drawsegs, both arena directions, red zones, hostile initial
  bytes, complete copies, neighboring canaries and natural alignment of every
  endpoint/following pointer. Pure-array requested/used/peak rows do not increase.
- Actual initial reserve and four 1.5-times growth boundaries through 648 slots,
  all three repaired drawseg pointers, live external frontscale roots, 1/320/640
  view widths, repeated smaller views, invalid-width diagnostic and complete
  teardown/repeated teardown.
- 16,384 slope/no-slope intersection rows, full 32-bit coordinates, existing
  rational ray graph and actual member stores. The entire drawseg state is
  retained in literal output.
- Persistent vertex arrays through 65,535 records; all six independent floor/
  ceiling flags across triangular-sector permutations; moved/in-place extended
  node growth, count mismatch/zero-extra cases, pointer repair and zeroed new
  height fields; Lua getter types, invalid-userdata error and indices; polyobject
  capacity/copies/rotation preservation; missing-UDMF-coordinate diagnostics.
- **85 unchanged stock maps: 61 Doom and 24 UDMF, 349,444 source vertices**.
  Every map passes its vertex data through the actual respective production
  loader/default loop. Counts and raw source-lump/pack hashes are retained.
  This is sequential replay, not simultaneously resident map memory.

Five compiled and executed corruptions are detected: change an endpoint high
bit, repair `curdrawsegs` to the wrong index, lose `firstseg`'s index, reduce the
frontscale high-water width and shorten the drawseg array copy. A compile failure
does not count as detection. ASan/UBSan disables only `shift-base` for the
unchanged traditional loader's signed coordinate `<<16`; negative/extreme
coordinate coverage remains. Production code and flags are not changed by that
fixture exception.

The native 64-bit host has 800→776-byte drawsegs; these host sizes are not EE
measurements. Unlike the rejected preliminary fixture, extracted engine types
now use actual C `bool`, while the allocator's independent service fixture
retains its own earlier declarations.

## Target evidence and reproduction

The actual-header ABI check passes 444→420-byte drawsegs, full four-byte XY
fields and unchanged persistent vertex/polyobject layout. Six complete frozen
consumer units are compiled with only the candidate header overlaid, excluding
independent renderer changes. The opt-out restores **every allocated object
section and every ordered relocation stream** exactly. All isolated function
frames remain equal. `r_segs.c` text changes **65,912→65,808 B** (−104 B);
the other five text totals and all rodata/writable data/BSS totals are unchanged.
These isolated non-LTO values are not final linked engine size or FPS results.

All three actual EE32 witnesses pass **201 cases** and all **38,152,928 literal
bytes** against the frozen host oracle. Frozen/opt-out execute **30,058,100
checks** each; candidate executes **30,058,088**, reflecting physical heap-walk
differences. The opt-out reproduces every baseline state and allocator row,
and its complete ELF is byte-identical to the baseline witness. Every selected
pure-array/loader requested/used/peak row is nonincreasing. Each runner exits
zero with one complete DONE record and no fixture failure.

For the synthetic two-array, 8,192-record, one-sided/no-red-zone case, actual
target zone used/peak changes **7,274,672→6,881,456 B**, saving 393,216 B across
the two roots. This is a deliberately large component fixture, not resident
stock-game drawseg storage. The largest frozen table request is 7,274,496 B,
comfortably within the unchanged 16 MiB backing. No target case or game capacity
was dropped to make the fixture fit.

Final witness compile logs are empty. Each link retains the existing single SDK
`linking abicalls files with non-abicalls files` warning also present in the
preceding fixture and filtered by the normal engine build; no other link
diagnostic appears. Independent input/object/runtime review is recorded in
`build/nov15-next10/review/memory-witness-build.json` and `memory-ee.json`.
Whole-game image, cache/churn and elapsed-time acceptance remains separate.

Reconstruct and verify the rejected candidate from the restored checkout,
without editing production or historical proof outputs:

```sh
source /workspace/.onboarding/environment.sh
python3 -B tools/ps2/prepare_next10_baseline.py
python3 -B tools/ps2/prepare_next10_memory_candidate.py
python3 -B tools/ps2/next10_memory_hosttest.py \
  --candidate build/nov15-next10/memory/candidate-src \
  --out build/nov15-next10/memory/replay-host --sanitize --negative-controls
python3 -B tools/ps2/next10_memory_audit.py \
  --candidate build/nov15-next10/memory/candidate-src \
  --out build/nov15-next10/memory/replay-audit.json
SRB2_PS2_OUT=build/nov15-next10/baseline/software \
  python3 -B tools/ps2/next10_memory_objectcheck.py \
  --candidate build/nov15-next10/memory/candidate-src \
  --out build/nov15-next10/memory/replay-objects
SRB2_PS2_OUT=build/nov15-next10/baseline/software \
  python3 -B tools/ps2/next10_memory_eetest.py \
  --candidate build/nov15-next10/memory/candidate-src \
  --host-proof build/nov15-next10/memory/replay-host \
  --out build/nov15-next10/memory/replay-ee
source /workspace/.onboarding/emulator-env.sh
SRB2_PS2_OUT=build/nov15-next10/baseline/software \
  python3 -B tools/ps2/next10_memory_eetest.py \
  --candidate build/nov15-next10/memory/candidate-src \
  --host-proof build/nov15-next10/memory/replay-host \
  --out build/nov15-next10/memory/replay-ee --analyze-only --run
```

`prepare_next10_memory_candidate.py` verifies all **602 frozen source hashes**
and writes exactly those 602 files with only `r_defs.h` changed by the checked
zero-context `next10_memory_candidate.patch`. Baseline header SHA256 is
`6c88912e24bb3f85cc8d0166f5734ad5e01e42dfe0bad3ae266e91cba831f62c`;
patch SHA256 is
`16fda7c4ce24a05f8dd1c459e061aa8e5a04ef9d37fd52b589c2aba7de95afbc`.
The reconstructed extracted-component hashes match the original EE witness
inputs, and its source audit equals the original literally. Mutated source or
patch bytes are rejected. Historical proof checker copies/hashes are retained
in `memory/rejection-prep/proof-tools-before-replay`; adding replay routing does
not rewrite the old reports or their original checker bindings. Target reruns
use the same candidate/host/output arguments plus `--analyze-only --run` after
the existing emulator environment is loaded.

EE witnesses use one static **16 MiB** arena backing with complete exterior
prefix/suffix canaries. All 1,024 possible 64-byte-aligned base residues are
bounded independently. Native backing retains malloc/free. Compiler/emulator
jobs are serialized through the coordinator, and runtime launches use the
existing locked `run_pcsx2.py` path. Raw extracts, snapshots, ABI/object files,
source bindings and logs are under `build/nov15-next10/memory`.

## Rejected preliminary flag substitution

Replacing persistent vertex flags with `UINT8` was withdrawn before game
integration. Actual release `boolean` is already one byte and `vertex_t` already
20 bytes, so the substitution saves zero bytes. The initial host fixture used
the allocator harness's four-byte `boolean` stand-in and misleadingly showed
24→20-byte modeled records. Actual-header EE ABI evidence disproved that premise.
No such substitution remains in production. Original candidate source, scripts,
host results and the rejecting target ABI are preserved separately in
`memory/rejected-vertex-flags/rejection.json`; none are used as drawseg savings
evidence.
