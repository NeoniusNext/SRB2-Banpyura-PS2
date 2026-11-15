# SRB2 PS2 exact optimizations — ninth pass, 2026-11-15

This pass retains slope-light interpolation, GS state staging and a compact
Hardware mipmap layout. Four GPT-6.1 Sol subagents at max reasoning handled
Software, GS, metadata and pack I/O; a fifth independently audited source,
target code, proofs and integration. Previous optimizations remain intact.

The reference is `19cf5df2a82b04e37fabe76a4441f3d5062ebaf1`. Final retained
source is **`2d0e8bf28f31a37c2d9ce2af9342f5f35284a69f`**, following the
initial candidate commit `6dd7e150b`. Exactly **three of 602 source files**
differ from the reference. The pack-header overlay was rejected after a
controlled whole-game cache regression and its source restored exactly.

## Retained results and scope

| Change | Measured result | Scope |
| --- | --- | --- |
| `R_CalcSlopeLight` integer loop | **17.01356% lower summed EE helper cost**, 132 faster / 78 ties / zero slower across 210 rows | Complete actual default-LTO helper 376→352 B, zero-byte frame; float math/division unchanged |
| `state_flush` temporary register addresses | **0.7556941% lower summed EE helper cost**, 46 faster / 18 ties / zero slower across 64 rows | Complete body 1360→1320 B, frame 160→128 B; packet values/order unchanged |
| PS2 `GLMipmap_t` / `GLMapTexture_t` | **88→76 B / 96→84 B**, unchanged four-byte alignment and every field width | 12 B less requested payload per mipmap; two existing map/flat arrays save 24×N B, without lowering N |

These are kernel costs on emulated EE, not weighted game FPS, physical-console
timing or frame-time variance. Existing capacity, texture size, precision,
filtering, sampling, geometry, physics, formats, palettes, voices, rates,
network, Lua, UDMF, add-ons and resource limits remain. No VRAM capacity,
DMA/GIF byte count or GPU draw-work reduction is claimed.

Hardware metadata savings depend on live record counts. Requested payload,
rounded zone blocks, controlled libc allocations, linked sections and complete
process/cache occupancy are different quantities and must not be added.
In particular, a smaller allocation can cross the unchanged 24 KiB spill
threshold and move between libc and the zone. The allocator policy is unchanged.

## Exact code and ownership proofs

[Software](PS2_SOFTWARE_NEXT9_20261115.md) preserves the original floating
expressions, conversion, signed accumulation and division. It counts down the
existing width, keeps the original negative branch and uses a scalar signed
upper selection on EE. An explicit declaration of the old register clobber
keeps all **14 complete caller instruction streams** unchanged. No instruction
is added by that declaration. The full 352-byte helper, all 15 ordered code
RELAs, exact literals and every resolved linked body byte are checked in both
release renderers. The complete mapped pools are 1272 B for Software and
2968 B for Hardware, including 22 fully resolved data-pointer relocations.

Five host policies/sanitizer runs each pass 298,391 cases and 763,880,960
lighting-array bytes. Live-content pixel tests, disabled policies, source
mutations and final binary controls pass. The native 210-row run adds 15
zero-length rows: actual old/new EE binaries both return without writes;
host C division-by-zero is not treated as a portable valid-input proof.
Every zero/reversed/fully negative row ties. Earlier branchless/MMI variants
that slowed negative rows were rejected.

[GS](PS2_GS_NEXT9_20261115.md) stores ten register addresses in bytes rather
than words; every used address fits, while all ten register values remain
64-bit. Host and ASan/UBSan each check **1,070,080 complete packet/state
cases and 6,410,547 register writes**; the deterministic native corpus checks
37,888 cases and 229,516 writes. RNG draws are explicitly sequenced, so host
and EE use identical input order. Six executed mutations, binding mutations
and report-parser mutations are detected. All eight original code RELAs and
resolved function bytes match the timed fixture and final game. The five
prior upload callbacks retain their complete recursive graph: seven code
sections, 15 data sections, 86 RELAs and all three AP88 PPACB sites.

[Mipmap metadata](PS2_MEMORY_NEXT9_20261115.md) reorders only PS2 extension
fields; generic leading fields, full byte/16/32-bit values, owner pointers,
pixel alignment, constructors, copy/growth, two arrays, colormaps, driver
lifetimes and allocation counts remain. Eight consumer-object opt-outs
restore frozen sections exactly. Host/sanitizer and five executed mutations
cover 186 cases and **29,351,712 literal bytes**. All three EE variants produce
those same bytes against the frozen host oracle. Baseline/opt-out make
77,428,289 target checks and compact layout 70,774,783 because physical heap
walks differ. Actual target ABI, single guarded arena backing, all allocation
failure/retry paths and real spill wrappers are included. Pure-table
request/used/peak rows are nonincreasing; this is not a whole-game peak claim.

Audio production source remains unchanged. Complete baseline-to-final music/
MIDI code, ordered relocation, referenced-data and specialized-ABI retention
is checked separately, without repeating codec tests for identical code.

## Rejected pack-header overlay

[The I/O investigation](PS2_IO_NEXT9_20261115.md) found mutually exclusive raw
and parsed pack-header lifetimes. The candidate saved 768 B BSS, cost 208 B
selected text and added 112 B local depth on a rare late probe. A local probe
preserved the reachable unprobed-then-parsed stream case and its callback/
file-change behavior. Correctness passed 106 lifecycle cases, all 15,052
stock lumps, 300,940 partial reads and **401,801,745 literal decoded bytes**.
The full-LTO opt-out restores 97 owned sections and four referenced policy
strings; no whole-engine/shared-string-pool equality is claimed.

That correctness and payload saving were insufficient for adoption. In the
combined four-change candidate, net image shrink moved the Software arena
boundary by one 4096-byte page and worsened texture-cache placement:

| MAP11 `-keepspawn`, frame175 | Reference | Four-change candidate | Pack-only opt-out control |
| --- | ---: | ---: | ---: |
| Arena capacity | 24,743,936 | 24,748,032 | 24,743,936 |
| Used zone | 23,634,736 | 23,896,896 | 23,634,736 |
| Free arena | 1,109,200 | 851,136 | 1,109,200 |
| Largest free block | 478,720 | 369,280 | 478,720 |
| Evictions | 9801 | 10,269 | 9801 |
| Evicted bytes | 464,359,296 | 486,174,528 | 464,359,296 |
| Failed attempts | 1020 | 1017 | 1020 |
| Engine elapsed `t_ms` | 59,453 | 61,218 | 59,449 |

At frame350, the four-change candidate made 1889 extra evictions and evicted
113,154,592 extra bytes; `t_ms` increased 106,901→117,330. All its frames
still matched and it had no warnings, but the extra cache/rebuild work
opposed the performance goal.

The causal control replaced **only w_pack IR**, retaining the other 188
canonical IR inputs, generated source metadata, SDK and link wrappers. It
restored every reference arena/cache/stack/libc/spill metric at frame175.
The original pack source is therefore restored byte for byte; **no ninth-pass
pack-overlay saving is retained**. Candidate source, patch/reconstruction,
14 rejected-candidate runs, exact code and the control remain separately
preserved. Final shipping ELFs are rebuilt and rerun after that restoration,
because even embedded revision text can alter a heap boundary.

## Final builds and game integration

Both final full-feature release builds embed source `2d0e8bf28`, link with
zero reported compile/link diagnostics and have current verified build
inputs. All final Software/Hardware lighting, GS/upload, restored-pack and
audio graph bindings pass against the actual shipping objects and ELFs.

| Linked text+data+BSS | Reference | Shipping | Difference |
| --- | ---: | ---: | ---: |
| Software | 6,685,969 B | 6,685,945 B | −24 B |
| Hardware | 7,885,748 B | 7,885,652 B | −96 B |

Initialized writable data is unchanged in both. Software read-only/code
sections shrink 24 B with unchanged BSS; Hardware read-only/code shrink
88 B and BSS shrinks 8 B. The three isolated metadata statics save 36 B
payload, but inter-section alignment adds 28 B padding in the linked image.
These section totals are not complete process RAM and must not be added to
dynamic metadata measurements. Final ELF file sizes are 10,324,660 B and
11,261,544 B. Normal thread/main stack reservations remain unchanged.

[Final ELFs, SHA256SUMS and validation metadata](../prebuilt/lossless-next9-20261115/README.md)
are copied byte for byte from the tested shipping files. Earlier releases
and the rejected candidate/control evidence are retained separately.

**Fourteen selected shipping runs on standard 32 MiB EE RAM** complete:
seven fresh runs of the actual post-restoration ELFs and seven exact
historical references from the preceding release. Their staged ELFs,
arguments, all stock assets and scripted fixture bytes are checked. All
**13 complete PPM pairs** and paired loader hashes match exactly. No shipping
run has warnings, texture substitutions, heap/fatal errors, reported audio
underruns or decoded/consumed hash errors. Both Hardware pairs confirm native
startup without Software fallback.

Paired images cover Software MAP01 with the existing NPOT/fog/water/MIDI/
stereo scene, Software MAP11 ordinary play, native Hardware MAP01 with the
same scene, native Hardware MAP02 ordinary play, and the Software MAP11
spawn-pressure image captured only after tic175. Configuring the scene is
not a claim that every kernel is visible in those images; native component
fixtures exercise the exact measured code directly.

| Map-loaded used zone | Reference | Shipping | Difference |
| --- | ---: | ---: | ---: |
| Software MAP01 combined | 6,059,584 B | 6,059,584 B | 0 B |
| Software MAP11 default | 15,316,208 B | 15,316,208 B | 0 B |
| Hardware MAP01 combined | 7,968,560 B | 7,854,896 B | −113,664 B |
| Hardware MAP02 default | 8,567,248 B | 8,453,568 B | −113,680 B |

Hardware checkpoints save about111 KiB of zone use. They overlap the typed
metadata payload saving and are not an additional sum. Software metadata
checkpoint use remains equal. No universal peak/free-space reduction is
inferred from these four maps.

The actual shipping Software pressure traces restore the reference arena
and every allocator/cache/stack/libc/spill metric at both 175 and 350 frames:

| Metric | Reference175 | Shipping175 | Reference350 | Shipping350 |
| --- | ---: | ---: | ---: | ---: |
| Arena capacity | 24,743,936 | 24,743,936 | 24,743,936 | 24,743,936 |
| Used zone | 23,634,736 | 23,634,736 | 24,559,152 | 24,559,152 |
| Free arena | 1,109,200 | 1,109,200 | 184,784 | 184,784 |
| Largest free block | 478,720 | 478,720 | 32,832 | 32,832 |
| Peak used zone | 24,734,944 | 24,734,944 | 24,734,944 | 24,734,944 |
| Evictions | 9,801 | 9,801 | 19,606 | 19,606 |
| Failed allocation attempts | 1,020 | 1,020 | 2,513 | 2,513 |
| Main stack high-water | 132,560 | 132,560 | 132,560 | 132,560 |
| Engine elapsed t_ms | 59,453 | 59,449 | 106,901 | 106,885 |

The small elapsed differences are recorded for completeness, not advertised
as whole-game FPS gains. Failed attempts include safely recovered allocations;
they are not lost frames or downgraded textures. The inherited 393,216 B main
stack reservation and 132,560 B measured high-water remain unchanged.

The standard real-driver suite retains **CT32 55/56 and CT16S 50/51**:
only the same historical VU0 result, 24,625 outcode differences among 49,152
vertices and zero XYZ/reciprocal-W error. Its nonzero returns are preserved;
it is not declared an entirely successful suite. All **17 complete CT32
readbacks** equal the preceding published hashes. Those driver inputs did
not change when the unrelated pack module was restored, so its recorded
standalone result remains valid for the final graphics source.


## Reproduction and limits

With the SDK/emulator configured as in [TOOLCHAIN.md](TOOLCHAIN.md), use the
normal full-feature release `-O2`, `-G8` and engine LTO:

```sh
python3 -B tools/ps2/prepare_next9_baseline.py
python3 -B tools/ps2/build.py --jobs 1
SRB2_PS2_HW=1 SRB2_PS2_OUT=build/nov15-next9/hardware \
  python3 -B tools/ps2/build.py --jobs 1
python3 -B tools/ps2/next9_integration_check.py --out build/next9-repro \
  --baseline-sw build/nov15-next9/baseline/software/SRB2.ELF \
  --baseline-hw build/nov15-next9/baseline/hardware/SRB2.ELF
python3 -B tools/ps2/next9_pressure_check.py --out build/next9-repro --frames 350
python3 -B tools/ps2/next9_gs_integration.py --out build/next9-gs-repro
```

Supply user-owned cooked packs at `build/pak`; no BIOS or game assets are
published. Source reconstruction verifies all 602 reference hashes in an
isolated directory without modifying the live checkout. Original reference
LTO objects and ELFs were preserved before editing; rebuilding source alone
does not promise identical historical SDK/Git metadata.

The game checker creates the existing deterministic NPOT/fog/water/MIDI/SFX
fixtures when absent. `--reuse-baseline` requires exact selected ELF,
arguments, fixture bytes and stock assets; `--analyze-only` rechecks staged
outputs. The separate 350-frame checker has the same strict warning/error/
completion gates. Unit reports contain their own failure, sanitizer, target
and code-binding commands, including the rejected I/O reconstruction.

Raw freezes, controls, full logs and component manifests remain under
`build/nov15-next9`. The standard GS suite's historical VU0 failure remains
visible and is never declared all green. CP0 counters can wrap; kernel
measurements and elapsed traces are not converted into physical PS2 FPS or
power claims. No physical-console FPS/frame-time-variance measurement was made.
