# Full-resolution tiled textures and EE captures

The tiled-source candidates handle images that cannot fit the PS2 GS texture pool without reducing their dimensions, depth precision, framebuffer format or effect passes. Capacity, native pixel equality and whole-game frame time have separate gates. The early exact final-r2 path had severe high-resolution frame costs; it is retained as evidence, not a shipping performance result. The combined6-box candidate reduces that work with grouped masks and bounded spatial classification. The measured V7 CPU changes and V8 indexed-window change are separate, source-bound steps with their own runtime gates. The [machine-readable results](HIGHRES_TILE_RESULTS_20261116.json) bind each source version, fixtures and reports. Physical PS2 hardware has not been tested.

At physical640×480, two CT32 framebuffers, Z24 pages and the existing CLUT allocation leave376832B for images. SKY1's1024×512 P8 source needs524288B; a complete CT32 screen needs1228800B. At physical640×512 the image pool is131072B. The former allocator cannot place these images even with an otherwise empty pool. A planner-clock fix alone does not change this bound.

## Source and draw representation

An oversized source is a metadata-only record with zero GS blocks. One permanent pinned16384B physical allocation is reused for uploads. V9 may temporarily borrow the adjacent free16384B for a fitting wide triangle and returns it before ordinary eviction. Captures and simple screen sprites use64×64 tiles with62×62 cores and one original neighboring texel on every side. Direct-colour general polygons use64×64 tiles with32×32 cores and sixteen original neighboring texels on each side. V8 uses128×128 tiles for general indexed PSMT8 sources in the same allocation, with64×64 cores and32 neighboring texels on each side. Each tile is rolled inside that physical allocation. REGION_REPEAT masks the original coordinates to that image. The original palette/index choice, alpha, nearest/bilinear policy, wrap/clamp behavior and draw order are retained. Packed AP88 reads use the existing logical pixel decoder, including its transparent padding.

The general path retains each original GS triangle, its STQ, RGB, XY, low24 depth and scissor. Splitting triangles or changing scissors had introduced coverage/sampling changes in earlier candidates; those reports remain failed evidence. The accepted mechanism uses the unused high byte of physical Z24 storage as a temporary gate. CT32 writes alias Z32 with x XOR32 and y XOR16 while FBMSK preserves all24 logical depth bits. Outside a selected rectangle the private byte is4; inside it is0. Original NoDepth draws use GEQUAL with integer source high1, leaving sufficient rejection margin. XYZ2 plus a constant FOG register carries the full integer depth for these passes; normal depth passes retain their existing packing. Deferred ramp overlays flush before the mask changes and preserve their own depth-test semantics.

For every original triangle, begin/end each issue one clear sprite over the conservative packed-vertex bounding box rounded outward to64×32 pages. The XOR mapping permutes pixels only within those pages. A20×20 box at(100,100) clears2048pixels per endpoint, compared with307200 for a full640×480 clear. End restores ordinary Z24, test, scissor and fog state. Logical ReadDepth returns low24 depth; direct PSM_Z32 readback can see the private upper byte and does not define a32-bit logical depth format. If a watchdog interrupts an active stencil sequence, transport stops and pending references are released before Abort/resource unwind; drawing cannot resume with an unknown mask.

The early run classifier used one determinant reciprocal per triangle and one Q reciprocal per covered sample. Grouped V5 batches completed disjoint rectangles by source window, uploads that window once, and submits the original triangle once for that group. Every selected rectangle is reset before each pass/overlay and restored afterward; the borrowed batch remains live through the final overlay flush. The combined6 helper defers initial activation until that first before-pass reset, removing duplicate mask writes. A separate immediate API remains for compatibility tests.

Combined6 tests original unwrapped vertex extrema for a complete64-texel window with an explicit original-neighbor margin and finite positive Q. Eligible triangles bypass stencil, classification and dynamic workspace. For others, an iterative binary screen-box traversal evaluates the original packed-vertex plane at four corners. Positive Q and raw extrema within one inclusive32-texel core bound its projective interior; a cancellation guard requires the conservative expression uncertainty to fit well inside the sixteen-texel halo. Ambiguous boxes split until1pixel, where the previous pixel key remains the reference. Completely outside boxes keep the old outside key0 rather than introduce a new coverage cull. The32-entry DFS stack has a bounded pixel fallback if exhausted. Every child rectangle strictly shrinks and the leaves are disjoint.

Only cache-window keys are reduced for periodic/deeply clamped UV values; GS data is unchanged and no UV limit is introduced. Source-X mapping is prepared once for all rows of an upload:64 in the previous path and128 for the V8 general indexed path. The signed-origin helper and complete upload bytes have independent component proofs; the old scanline recurrence benchmark does not describe the new box classifier.

## Memory and planner accounting

| Physical allocation | 640×480 | 640×512 |
| --- | ---: | ---: |
| Two CT32 framebuffers plus Z24 pages | 3686400B | 3932160B |
| Existing CLUT allocation | 131072B | 131072B |
| Image pool before scratch | 376832B | 131072B |
| Permanent reusable scratch reservation | 16384B | 16384B |
| Optional revocable prefix while wide | 16384B | 16384B |
| Usable ordinary-image capacity | 360448B | 114688B |
| Two full CT32 EE captures | 2457600B | 2621440B |
| Final-r2 temporary run workspace at640pixels wide | 25600B | 25600B |
| Grouped V5 temporary workspace | 38400B | 38400B |
| Combined6 completed batch plus DFS payload | 13312B | 13312B |

Physical layout and logical resolution are separate: a640×400 logical view in480p can still use640×480 physical storage. The standalone native fixture also exercises explicitly sized framebuffer layouts. Its results do not replace the whole-game mode-layout tests.

The EE capture path uses the existing8MiB screen budget and zone try-allocation/eviction. It copies all native framebuffer bytes directly to EE backing and streams from that backing; it never first requires a full-size GS capture image. Captures, wipes, tinted/additive wipe passes, RGB readback and screen effects retain their previous data and blend behavior. Same-slot recapture invalidates the tile; failed reads cannot leave a usable partial capture. A saved selected CLUT view pins its physical owner across screen operations, so allocating the tile cannot evict the image being restored.

Combined6 retains every non-regenerable source's existing CPU allocation while a proxy needs it. AP88 format alone does not guarantee rescue: the engine's recipe searches particular patch zone tags, requires a matching live hardware owner and columns, and can fail for arbitrary AP88 producers. Its byte ledger therefore counts raw AP88 as2×W×H and packed AP88 asN+ceil(N/8) over the actual stored rectangle. The existing mipmap padding carries the lease bit. Actual engine post-upload release sites, including patch load/update/unlock, respect it. Delete/Update clear it and release ownership, including explicit source free followed by same-address allocation. Queued DMA references remain independently protected during synchronous tile copies. Indexed RGBA may additionally retain1KiB of palette history. Whole-game measurements must include retained sources and simultaneous captures; the fixture does not establish that every map fits32MiB.

The V7 column map adds256B of temporary payload per synchronous upload; V8 raises that map to512B, an additional256B with no persistent allocation; compiler spills/alignment are separate (the isolated EE conversion wrapper grew304B). The last-use composite change adds128B to its held-patch table and frees only sources owned by that composite call after their last placement. Its631152467-byte component comparison and identical16851 canonical-build count are independent of the renderer's frame-time results. The [CPU component report](TILE_CPU_MATH_20261116.md) binds the origin/column evidence; combined stack and game RAM remain integration measurements.

The EE mipmap stays68B and each driver record140B. Proxy pointers share the existing mutually exclusive split tail; the original55 field offsets remain unchanged. Default320 reserves no tile until an oversized source actually needs one. High modes reserve the real16KiB tile at Init when a full capture cannot fit; cache clear preserves it. Ordinary admission and planner budget subtract that actual reservation once.

Only an existing zero-block tiled owner/view under a live full-size hint costs zero at planned L0. Adaptable/expired/missing/physical records retain their former costs. Carried immediate virtual entries under that hint are capped atL0, matching the existing draw-time force-full rule and preventing unsigned "saving" underflow. The actual PlanEnd witness at a512-block pool reserves64blocks once, keeps the256-block world atL0 with a virtual2048-block sky, and still charges an ordinary sky. A carried live-full proxy with measured density.75 and a larger world chooses worldL1/proxyL0/planned256. Removing its cap reproduces worldL2/proxyL1/planned576.

## Reproducing host and ABI checks

Use a GCC-compatible host compiler. Each command reads production sources by default; add `--source-root PATH` to audit a frozen source tree. Outputs and mutated negative-control compiler inputs go only beneath `--out`. No command below starts an emulator.

```sh
python tools/ps2/opt17/tile_stream_hosttest.py --out build/tile-host/stream --sanitize
python tools/ps2/opt17/tile_capture_hosttest.py --out build/tile-host/capture --sanitize
python tools/ps2/opt17/tile_planner_hosttest.py --out build/tile-host/planner --sanitize
python tools/ps2/opt17/tile_source_lease_hosttest.py --out build/tile-host/lease --sanitize
python tools/ps2/opt17/tile_watchdog_hosttest.py --out build/tile-host/watchdog --sanitize
python tools/ps2/opt17/tile_stencil_hosttest.py --out build/tile-host/stencil
python tools/ps2/opt17/tile_grouped_stencil_hosttest.py --out build/tile-host/grouped-stencil
python tools/ps2/opt17/tile_stream_cost_hosttest.py --out build/tile-host/cost --sanitize
python tools/ps2/opt17/tile_abi_check.py --out build/tile-host/abi --build-report build/nov17-integrated/candidate-v10/hardware/build-report.json
```

The ABI command uses the compiler, generated configuration and SDK flags from an existing target build report. It compiles the complete driver with the target140B/68B and55-prefix-offset assertions, without linking or running it. The normal-profile hw_cache, hw_md2 and hw_light translation units also compiled with warnings fatal. The normally disabled ALAM_LIGHTING build has the same pre-existing GLPatch width/height and fallthrough errors in the frozen baseline and candidate; the lease-aware release is present there.

Negative controls are explicit and must fail their own assertions. Available stream controls are `--negative halo|perspective|colour|capacity|lifetime|ownership`; capture controls are `copy|phase|invalidate|pin|view`; planner controls are `old-charge|carried-cap`. Lease/watchdog accept `--negative`; the stencil tool always runs its three negative variants. For example:

```sh
python tools/ps2/opt17/tile_stream_hosttest.py --out build/tile-host/bad-stq --negative perspective
python tools/ps2/opt17/tile_planner_hosttest.py --out build/tile-host/bad-carried --negative carried-cap
python tools/ps2/opt17/tile_source_lease_hosttest.py --out build/tile-host/bad-release --negative
```

The stream fixture compiles actual conversion, tile residency, draw/packing and ownership code. Counts depend on the source version: final-r2 passes501291 sanitized checks; combined6 passes416031, including one-window eligibility/fallback and actual raw/packed AP88 retention/byte accounting. The capture fixture compares complete640×400/448/480/512 backings and transfer pixels, screen scaling, normal/tinted wipes, partial failures and saved owner/view state. Its post-image packet test uses a64×48 destination with a full640×480 source; selected-owner post tests use32×32 destinations to bound host recorder memory. Combined6 passes7950416 checks. Full-frame post-image raster parity is a separate native test.

Lease tests execute real FadeMask/RawFlat and patch load/unlock callbacks, release macros, Update and zone pressure: combined6 passes1198223 sanitized checks, including actual raw/packed AP88 allocations and the caller-retag negative control. Earlier r2/V5 regressions pass105 each. Complete planner tests pass40; fault-only watchdog tests13. The bounded helper passes10804166 checks, grouped V5 passes12494812, and deferred combined6 passes13769887 with five helper negatives plus separate deletion controls for all three actual before-pass call sites. Component success is not reassigned to a later source version without a binding.

## Native scope and runtime cost

The [native tool](../../../tools/ps2/opt17/highres_tiles_native/build_native.py) builds standalone actual EE/GS comparisons from the selected source. The roomy resident source and small-pool proxy use the same source bytes and renderer. Native reports bind ELF, fixture, actual compiler dependencies and20candidate hashes. The linked machine report distinguishes frozen V4 evidence from the final-r2 rerun; results are not reassigned between source versions.

The [final-r2 native index](highres-native-final-r2.json) and [grouped V5 index](highres-native-grouped-v5.json) each retain the original231-check default report's one strict failure: one RGB channel differs by1 at one lighting-row pixel against the fast-path reference. Their144-check common-EE controls are exact. All sampled source indices, alpha, captures, post/wipes, adversarial minification phases and NoDepth model/color/fog/depth controls are exact in those reports. The original strict gate is not relaxed. The [combined6 index](highres-native-combined-v6.json) binds all20 guarded source hashes and closes those same gates:231/default with only the retained one-LSB exception,144/common exact,46/attributes exact,40/coordinates exact. Its additional197-check one-window witness is exact, observes12 successful direct branches with no masks/workspace, and rejects the full-repeat case. The halo negative still detects1559 pixels. Emulator32MiB and synthetic images do not establish compatibility or frame rates for all game maps.

`HWSTREAM` reports cumulative actual rectangles, mask sprites, triangle submissions and peak workspace; combined6 adds complete-window hits, box visits and pixel fallbacks. `raster_bbox_upper` is a conservative viewport-area submission bound, not a fragment counter. H.st.uploads/upload_bytes and GIF qwords reveal repeated tile transfers. The count-only host tool executes complete production classification, filling and packet allocation with a4MiB rolling recorder, so it can count hundreds of megabytes without retaining them all. It does not rasterize or measure PS2 frame time.

| Synthetic640×480 post grid | Final-r2 | Grouped V5 | Combined6 boxes/deferred masks |
| --- | ---: | ---: | ---: |
| Tile uploads | 17773 | 1096 | 1096 |
| Uploaded source bytes | 291192832 | 17956864 | 17956864 |
| Total packet bytes | 303704992 | 24729104 | 23486816 |
| Mask sprites | 95529 | 95529 | 68122 |
| Submitted original triangles | 18429 | 1096 | 1096 |

Combined6 visits57414 boxes for that full grid, with no pixel-key fallbacks. Its deliberately large synthetic SKY1-span triangle (U16/V128) visits5297 boxes, uploads81920B in20 windows, and emits617936 packet bytes. These are source-generation/packet counts, not actual game-frame timing. Normal SKY1 dome triangles span U16/V128 because the engine has64columns and only4rows: a whole64-texel window cannot cover most unclipped bands, so the box classifier is necessary. The capture reads FB once to EE; repeated tile uploads and primitive work, not per-run FB/Z copies, produced the earlier traffic. Whole-game water/heat, mode chains, RAM peaks and frame times remain the final integration gate.

## Measured V7 CPU changes

The actual V12 full-game instruction sampler collected46703samples. Software double/64-bit helpers accounted for54.10%; P8 tile filling17.14%. The original `(float)int64` corner conversion enters a software double chain twice per corner. A separate phase overlay confirmed the cause without new GS observations: one640×512 frame spent246898742Count units classifying map/sky boxes and192239214 classifying flats; their row fills cost53379914 and45579902. Existing DMA-wait counters were zero in those phases. Diagnostic `-hwstats1` separately added about0.31billion Count units in the sprite phase; that observer cost is not attributed to source leasing or ordinary gameplay.

V7 changes only the two measured sites. Its signed32-domain converter constructs the nearest-even float bits and preserves the original conversion outside that domain. Native485782-case bit comparison passes; a direct signed32 hardware cast fails150396cases because EE conversion can truncate. That rejected shortcut is retained only as negative evidence. Median163840-call Count changes125774508→11389047 (11.04× in the isolated helper); this is not a frame-rate multiplier. The [conversion report](TILE_I64_TO_FLOAT_20261116.md) binds that native evidence, wide fallback and the unchanged surrounding UV arithmetic.

P8/ALPHA8 non-screen rows reuse the64-entry source-X map to prepare at most four contiguous or constant runs. Rows copy those runs with memcpy/memset; more than four runs, screens and other formats retain the previous loop. The temporary descriptors add28B, with no persistent storage, heap allocation or VRAM. Independent normal/sanitized comparison covers10880complete uploads and178257920bytes, including ALPHA8, NPOT, wrap/clamp and short-width fallback; the candidate-only negative is detected. The actual EE context grows420→448B; the isolated non-LTO `-O3` loader frame changes624→592B, so payload size is not presented as a stack-frame increase.

```sh
python tools/ps2/opt17/tile_runs_hosttest.py --base tools/ps2/opt17/tile_cpu_reference/columns-candidate.inc --candidate src/ps2/hw/ps2_hw_tile.inc --out build/tile-host/row-runs --sanitize --ee-objects
```

The baseline is the tracked frozen column-map source; the candidate argument selects the current renderer, or can point at a frozen V7 file. `--negative` mutates only that candidate. The separate historical column fixture remains unchanged.

The [measured V7 native index](highres-native-measured-v7.json) binds all20 source hashes. Its231/default,144/common,46/attribute,40/coordinate,197/window and halo suites retain exactly the combined6 outcomes, including the explicit one-LSB reference failure and expected halo negative. Whole-game V7 frame time and RAM are still separate acceptance evidence.

## Indexed128 V8 window size

The post-V7 full-game sampler shows the software conversion cost falling from54.1% to0.2% of samples. Box classification now dominates: roughly45.8% of samples attributed to the inlined emitter point to corner finite/min/max checks, with another7.9% in the exact integer converter and4.7% in the UV plane helper. This measured profile motivates using the unused portion of the existing allocation for indexed sources. A128×128 PSMT8 image occupies the same16384B as64×64 CT32, so no extra GS reservation is needed.

Only region1 nonscreen PSMT8 sources select the larger extent. Region0 capture/sprite uploads remain64×64, and CT32/CT16 remain unchanged. The source-X table, row runs, wrap/clamp key, general core, output TBW and REGION_REPEAT mask carry the selected extent together. The cache layout key distinguishes region0,64general and128general transitions. Source TW/TH and all original GS STQ/RGB/XY/depth/scissor data remain unchanged. The original64 origin helper is byte-preserved; a64-core helper handles128 windows with the same signed-period and finite-coordinate fallback. The existing four-texel cancellation allowance is retained within the larger32-texel halo.

Normal and sanitized actual-source fill comparisons each pass12672uploads and346030080bytes:9856 exact64 regressions and2816 new128 scalar layouts. They include packed AP88 decoding, ALPHA8, indexed RGBA history, screen16/32 and repeat/clamp boundaries.198 actual extracted-loader checks cover cache transitions/hits and transfer byte ledgers. The128-only negative is detected. The complete target ABI compilation and416031 sanitized renderer checks pass. The context increases256B, no BSS/heap/record growth is introduced, and the isolated EE loader frame is880B. The source-bound native gates below close raster/addressing checks; actual game time and RAM acceptance remain pending for this source version.

| Synthetic U16/V128 source triangle | V7 indexed64 | V8 indexed128 |
| --- | ---: | ---: |
| Classifier boxes | 5297 | 3073 |
| Completed rectangles | 2649 | 1537 |
| Window uploads/submissions | 20 | 8 |
| Uploaded bytes | 81920 | 131072 |
| Mask sprites | 7482 | 4802 |
| Generated packet bytes | 617936 | 463056 |

These actual-source counts show the tradeoff explicitly:42% fewer boxes and36% fewer mask sprites, while transferred source bytes rise60%. The CT32 full-frame post counts are identical to V7. They are neither measured PS2 Count nor a frame-rate claim. The preceding integrated V7 PAL MAP01 smoke produced14frames and3valid captures without fatal/fallback/audio errors; sky/clear fell from about595million to182million Count, while the whole steady frame remained about190million Count (roughly0.64s). Its known40-skip gate remains red, and performance still blocks a shipping recommendation.

The portable extent fixture bundles only the required frozen V7 conversion functions, with a recorded reference hash. It extracts the candidate and actual packed-pixel accessor from the selected source; no private baseline is needed. Its128-only negative leaves all64 regressions exact.

```sh
python tools/ps2/opt17/tile_extent_hosttest.py --out build/tile-host/extent --sanitize
python tools/ps2/opt17/tile_extent_hosttest.py --out build/tile-host/bad-extent --negative
```

The [indexed V8 native index](highres-native-indexed-v8.json) binds all20 frozen sources. It retains231/default checks with the same one-pixel, one-channel difference of1 against the fast reference;144/common,46/attributes and40/coordinate checks are exact. The257-check window suite observes16 exact positive pairs, including125-texel raw spans, with no masks or workspace on those direct branches. A whole-repeat case still falls back. Both the128 indexed layout and64 direct layout assert64 physical GS blocks. The expanded20-check halo negative detects14632 wrong indexed pixels and1559 direct-colour pixels, confirming both sampler guards remain effective. No original tolerance was changed.

## Borrowed-wide V9 fast path

V9 adds one optional choice after the V8 complete-window test fails. A general indexed source may use128×256 PSMT8; a CT32 screen source may use128×64. Both shapes occupy32KiB. Raw positive-Q vertex bounds must fit the same explicit original-neighbor guard, and the existing packed-screen empty-box rejection runs before borrowing or upload. The original triangle then draws through ordinary Z24 with no masks, classifier or dynamic workspace. All other cases retain the V8 fallback. CT16 gets no new wide path.

The permanent16KiB cache keeps its high anchor A. Only a free64-block interval immediately below A can be borrowed; the grow operation cannot relocate or evict another image or take a nonadjacent hole. The physical record, block owners and H.used_blocks account for128blocks while borrowed. Ordinary eligibility and planner budgets still subtract only the permanent64blocks. If a normal free-range allocation fails, it first returns the borrowed prefix and retries, before allocation-failure counters, victims or screen spilling. Ordinary uploads continue to use A, including while the record has a borrowed prefix; wide uploads use A−64blocks. Shape changes and both resize directions invalidate the cache.

Every old wide draw and deferred overlay is copied into the ordered transport before the prefix can be returned. Subsequent uploads may overwrite it through the existing ordered GS reuse contract; the path adds no FINISH, fence, readback or CPU source copy. Full teardown frees the actual span. No new context field, SX entry, record byte or cache BSS is added. Target cache symbols remain44B of BSS plus4B of initialized data; the isolated generic loader frame is928B versus880B, while its constant-propagated variant is864B. HWSTREAM distinguishes permanent minimum, current borrowed and current actual scratch bytes.

The actual-source allocator fixture passes154070checks and180224000 exact converted bytes in normal and sanitized runs. Its10560cases include6688 fixed64,1760 V8 indexed128,1760 wide T8 and352 wide CT32 layouts. It verifies the PAL448-block ordinary request succeeds after returning the prefix, with no failure or eviction count. The maximal valid fragmented pool grows from8112 to8113 free intervals on return, below the8193-entry bound, with every physical block uniquely owned or free. Five candidate-only negatives detect unsafe claims, bad owner release, cache-shape aliasing, a stale ordinary anchor and a source DMA reference.

The separate queued-draw fixture passes262417 sanitized checks: a wide copied image and original triangle precede a real448-block ordinary upload that reuses the prefix, without an intervening fixture flush/fence/read. Earlier packet bytes remain immutable, the ordinary upload owns the returned blocks, and an occupied prefix forces the V8 classifier. A stale wide texture-base negative fails its named assertion. These are packet/allocator tests; native GS comparisons remain a separate gate.

```sh
python tools/ps2/opt17/tile_borrow_hosttest.py --out build/tile-host/borrow --sanitize
python tools/ps2/opt17/tile_wide_draw_hosttest.py --out build/tile-host/wide-draw --sanitize
python tools/ps2/opt17/tile_wide_draw_hosttest.py --out build/tile-host/bad-wide-reference --negative
```

| Actual-source synthetic packet count | V8 | V9 |
| --- | ---: | ---: |
| U16/V128 sky uploads | 8 | 1 |
| Sky uploaded bytes | 131072 | 32768 |
| Sky generated packet bytes | 463056 | 33152 |
| Sky classifier boxes / mask sprites | 3073 /4802 | 0 /0 |
|640×480 post uploads | 1096 | 81 |
|640×480 post uploaded bytes | 17956864 | 2654208 |
|640×480 post generated packet bytes | 23486816 | 2677792 |
|640×480 post classifier boxes / mask sprites | 57414 /68122 | 0 /0 |

The V9 post grid emits162 original triangles and has no dynamic workspace. The640×512 grid has the same81uploads and2654208 source bytes; its former V8 path needed1213uploads and19873792bytes. These counts assume the adjacent prefix is free and measure packet generation, not raster speed. The preceding V14 full-game indexed-V8 smoke reduced sky/clear to about140million Count and the warm frame to149million (about0.50s). Its three-map heat/wipe stress reported22.47MB peak and no fallback/fatal/audio errors. Those observations support keeping the EE capture representation; they do not replace V9 native, full-game or final RAM acceptance.

The current capture fixture also passes7949821 sanitized checks, including full native-byte backings, normal/tinted wipes and saved shared-owner restoration with a borrowed cache. Its post-image ledger now checks the actual64/128-block record, and each texture base is checked against its exact physical CLAMP shape. An initial fixture-only signedness compilation warning is retained in the artifacts; changing the literal to64u fixes that warning without changing renderer source or graphics thresholds.

The [V9 host index](highres-host-borrowed-v9.json) binds the final20source hashes, portable fixture hashes, target ABI, allocator, queued-draw and capture reports. It also retains the fixture compilation red separately from runtime results. Native rendering and whole-game timing are not inferred from this host index.

The [borrowed V9 native index](highres-native-borrowed-v9.json) binds the immutable20source hashes. Its318-check borrow suite passes, including both shapes, occupied-prefix fallback and eight queued real reclaim/overwrite pairs;1792000 complete RGBA/Z pixel comparisons are exact. The original six suites retain their previous outcomes and thresholds:231/default with the one-pixel RGB difference of1,144/common exact,46/attributes exact,40/coordinates exact,257/window exact and the two expected halo-negative failures.

The first borrow probe is preserved as262checks with20branch-assertion failures and24exact RGBA/Z pairs. Its screen input sat at the maximum125×61 guard boundary. EE normalization produced Vlo69.99999237(bits428bffff) and Vhi130.99998474(bits4302ffff); the legal integer-origin interval was empty (first85,last84), so the renderer correctly used the exact V8 fallback. The final fixture retains those four boundary fallback pairs and adds an interior124×60 positive case. Neither renderer source nor graphics tolerance changed.

The first actual V15 MAP01 warm frames remain heavy: clear takes roughly143–146million Count and a whole frame about153million, close to V14's140million/149million. The synthetic SKY U16/V128 result therefore does not establish an actual MAP01 sky or overall FPS gain. Actual heat/wipe timing, final memory/mode chains and final acceptance remain the parent integration report's responsibility.
