# PS2 architecture and lossless optimization sources — 2026-11-15

This is a checked research catalog for the SRB2 PS2 **Hardware** renderer's RAM,
VRAM, resource residency and submission work. It is a broad categorized search,
not a claim to have read all Internet material or enumerated every possible
optimization. This research changed no production source, compiler, emulator,
runtime test, commit or dependency version.

The inspected repository revision is
`85bd9716b1ed114d46f95831c64a87623038eeaa`. Read-only hashes are in
[the source map](ps2-research-20261115/README.md#repository-source-map).
This document does not extend that revision's validation claims.

The constraint is unchanged graphics, features, physics, numeric precision,
legal input ranges and resource capacities. An idea below is a **candidate to
investigate**, not an accepted optimization or measured saving. Fewer cached
bytes at a late checkpoint, a smaller ELF, shorter packets and lower emulated
cycle counts are different measurements.

## Source and access policy

All links were checked on **2026-11-15 UTC**. Technical authorities are original
vendor documents/papers, maintainer source, or an original author's documented
implementation/experiment. General explainers, forums and search snippets
helped discovery but are not architectural authorities here.

* **Archival vendor manual:** original SCE/Toshiba content on an attributable
  mirror, not a current Sony support endpoint. SCE v6 manuals retain their
  original confidentiality notices. Link and briefly paraphrase them; do not
  vendor manuals or large extracts.
* **Public developer paper:** the named developer's original work; an archive
  can preserve it without being its author.
* **Maintainer implementation:** actual upstream source at a pinned commit;
  it describes that implementation, not every hardware restriction.
* **Original observation:** keep the author's date, test scope and uncertainty
  separate from hardware specifications.

The web reader rejected GitHub raw PDFs as `application/octet-stream`; direct
HTTP GET succeeded. `pdfinfo`/`pdftotext` inspection used a temporary directory.
[Manual checks](ps2-research-20261115/README.md#manuals) retain
HTTP status, size, SHA256 and actual page count. PDFs/full manual text were
**not** copied into this repository. The archive README's counts are one higher
than the actual file for several manuals; checked counts below are used.
[Paper checks](ps2-research-20261115/README.md#papers) retain
download evidence; [maintainer checks](ps2-research-20261115/README.md#maintainers)
bind inspected source paths and Git blobs. The final
[link report](ps2-research-20261115/README.md#link-checks) checks
local paths and external HEAD reachability. The Toronto A1 mirror rejected a
HEAD request through the execution proxy, while the web reader opened and
parsed its eight-page PDF; that access difference is retained.

## Architecture reference shelf

Pages below are printed manual locators; relevant numbered pages equal the
one-based PDF pages in these SCE copies. All eight SCE documents identify
**Version 6.0, April 2002** internally.

| ID | Checked original document | Useful locator and role |
|---|---|---|
| M1 | [SCE EE Overview](https://raw.githubusercontent.com/ninjadynamics/PS2Docs/main/EE_Overview_Manual.pdf), 64 PDF pages | §§1.2–1.5 pp12–17: bandwidth, parallel engines, memory. §§2.2–2.6 pp24–47: core/VPU/IPU/GIF/SIF. §3 pp50–63: DMA, VIF buffering and GIF formats. Start here for producer/consumer boundaries. |
| M2 | [SCE EE User's Manual](https://raw.githubusercontent.com/ninjadynamics/PS2Docs/main/EE_Users_Manual.pdf), 218 pages | §5 pp41–80: DMAC chains, arbitration, MFIFO, restrictions. §6.3 pp86–101/§6.4: VIF alignment, UNPACK, buffering, flush commands. §7 pp147–170: PATH priority, PACKED/REGLIST/IMAGE. Read before changing overlap or packet ownership. |
| M3 | [SCE EE Core User's Manual](https://raw.githubusercontent.com/ninjadynamics/PS2Docs/main/EE_Core_Users_Manual.pdf), 180 pages | §1.3.6 p24: issue combinations. §6 pp129–142: cache, scratchpad, UCAB. §7 pp143–152: performance counters/events. §8 pp155–165: FPU, rounding, IEEE differences. Use for scheduling and physical cache measurements. |
| M4 | [SCE EE Core Instruction Set Manual](https://raw.githubusercontent.com/ninjadynamics/PS2Docs/main/EE_Core_Instruction_Set_Manual.pdf), 408 pages | §2: scalar definitions, including SYNC p121. §3 pp137 onward: MMI packing/shifts/arithmetic. §5: COP1. Check lane order, signedness, saturation and alignment instruction by instruction. |
| M5 | [SCE GS User's Manual](https://raw.githubusercontent.com/ninjadynamics/PS2Docs/main/GS_Users_Manual.pdf), 176 pages | §2 pp19–31: color/Z/texture/CLUT layout, including indexed TBW p27. §3.4 pp50–63: sampling, CLUT, texture cache. §4: transfers, start/width restrictions p76, packed data p79. §6 pp93–95: SIGNAL/FINISH/LABEL. §8 pp161–175: pages/blocks/columns/occupied areas. Allocation and synchronization authority. |
| M6 | [SCE GS User's Manual Supplement](https://raw.githubusercontent.com/ninjadynamics/PS2Docs/main/GS_Users_Manual_Supplement.pdf), 22 pages | §§1.1–1.4 pp6–10: drawing order, page breaks, pipelines, texture reloads. §2 pp11–20: AA1 coverage/order. §3 p22: input FIFO. Useful when packet/primitive changes alter GS work. |
| M7 | [SCE VU User's Manual](https://raw.githubusercontent.com/ninjadynamics/PS2Docs/main/VU_Users_Manual.pdf), 368 pages | §1.1 pp16–19: registers/memories. §2 pp25–29: numerics. §3.4 pp44–52: hazards/pipelines. XGKICK p196. §5.4 pp210–218: macro dependencies/Q synchronization. VU, FPU and host arithmetic differ. |
| M8 | [SCE SPU2 Overview](https://raw.githubusercontent.com/ninjadynamics/PS2Docs/main/SPU2_Overview_Manual.pdf), 80 pages | §1 pp9–17: cores/local memory/waveforms. §2.2 pp28–29: stream input. §2.5 pp33–34: effects workspace. TSA p55, waveform/loop addresses pp56–58. Sound RAM has a real format/transfer contract. |
| M9 | [Toshiba TX79 Core Architecture Rev.2.0](https://www.lukasz.dk/files/tx79architecture.pdf), April 2001, 727 pages | §2.2: issue/pipelines; §7: caches/coherency; §9: counters. Original vendor companion for the CPU family. Use SCE manuals for complete EE/console integration. |

The basic budget is **32 MiB EE RAM**, **16 KiB I-cache**, **8 KiB D-cache**
and **16 KiB scratchpad**. VU0 has 4 KiB each of micro/data memory, VU1 16 KiB
each. These are distinct memories, not interchangeable extra heap. The EE data
cache has 64-byte lines and no bus snooping; explicit coherency matters.
See M1 §§1.5/2.2/2.3 and M3 §6.1.

IOP work RAM and SPU2 local sound RAM are separate 2 MiB budgets, not unused
extensions of the EE arena. SIF transfers/RPC, storage drivers and streaming
buffers consume that capacity too (R1 memory map, M8 §§1.1–1.2, S3–S9).

The GS has **4 MiB shared local memory** for color, depth, textures and CLUTs.
Source bytes per texel do not determine allocated footprint; use format-specific
page/block layout. Z24 uses the 32-bit layout. M5 §§2/8 are the basis;
[HW_RENDERER.md](HW_RENDERER.md#real-memory-budgets) describes the port layout.

DMA completion releases a source only after DMA stops reading it; it does not
establish completed GS drawing. Framebuffer readback/overwrite has a separate
completion requirement. M2 §5 and M5 §6 describe different boundaries.

## Original architecture and developer papers

| ID | Original author, checked document and date | Relevant pages; application and limits |
|---|---|---|
| A1 | Kunimatsu et al., Toshiba/SCE, [Vector Unit Architecture for Emotion Synthesis](https://www.eecg.utoronto.ca/~moshovos/ACA08/readings/EmotionEngine.pdf), IEEE Micro March/April 2000, pp40–47 (8 PDF pages) | pp40–43 explain VU roles/interfaces and parallel dataflow; later pages cover implementation/pipelining. Useful for VIF/VU/GIF overlap. Peak rates are not SRB2 FPS predictions. |
| A2 | Tago et al., Toshiba/SCE, [CPU for PlayStation®2](https://www.cecs.uci.edu/~papers/compendium94-03/papers/2001/date01/pdffiles/09e_1.pdf), DATE 2001 | Mirror contains a **one-page abstract**, not the full paper. Confirms original authorship/integration; use manuals for programming details. |
| A3 | Robin Green, SCEA R&D, [Procedural Rendering on PlayStation 2](https://www.lukasz.dk/files/rgreen_procedural_renderinggdc2001.pdf), public GDC March 2001 version, 32 pages | pp10–14: dataflow, alignment, scratchpad. pp17–21: single/double/quad/circular buffer tradeoffs. Useful ownership/overlap patterns. Procedurally approximating SRB2 geometry is outside this task. |
| A4 | George Bain, SCEE Technology Group, [PS2 Programming Optimisations](https://www.lukasz.dk/files/PS2Optimisations.pdf), AGDC 2002, 39 slides | Slides 3–16: analysis/DMA/GIF; 19–20: geometry representation; 24–26: page behavior; 32–37: MMI/cache/SPR/disc. LOD, reduced precision and fewer effect passes elsewhere in this talk are **not permitted**. Historical bandwidth examples are not port benchmarks. |
| A5 | Colin Hughes, SCEE, [PlayStation 2 Clipping](https://www.lukasz.dk/files/final-gdc2002-clippingps2.pdf), GDC 2002, 36 slides | Slides 18–27: homogeneous clipping, VU flags, guard bands/full clipping; 31–33: interpolation/output. Prove early rejection against the actual driver, including near crossings. Guard bands do not authorize dropping intersecting triangles. |
| A6 | George Bain, SCEE Technology Group, [Texture and Geometry Syncing](https://www.lukasz.dk/files/TextureGeometrySyncing.pdf), undated, 4 pages | pp1–2: PATHs/intermittent transfer; pp3–4: FLUSHA/MSKPATH3 coordination/conditions. Scheduling reference, not permission to remove waits or assume PATH3 always wins. |
| A7 | James Lee (jbit), [How to draw pixel perfect sprites on the PS2](https://www.lukasz.dk/files/gs-perfect-sprite.pdf), 2006-06-06, 5 pages | Worked sampling/coordinate analysis. Useful for tiled captures and sprite splitting; visually similar edges are not exactness proof. |
| A8 | BigBoss; layout/additions Jules, [Playstation 2 Vector Unit Instruction Manual](https://raw.githubusercontent.com/ninjadynamics/PS2Docs/main/vu-instruction-manual.pdf), cover date `1/7/2003`, 128 pages | Community author reference, **not SCE documentation** despite the archive location. Convenient lookup; use M7 for numeric/pipeline restrictions. Ambiguous numeric cover date is intentionally unexpanded. |

[Łukasz's archive index](https://www.lukasz.dk/playstation-2-programming/archive/)
attributes the archived papers to their original developers; their covers were
also inspected. No complete commercial SDK or middleware is needed here.

## Maintainer source catalog

Inspected upstream tips are PS2SDK
[b04e8973c8a9839d0653d87a8d30cc67fd3c3f05](https://github.com/ps2dev/ps2sdk/commit/b04e8973c8a9839d0653d87a8d30cc67fd3c3f05)
(2026-11-14) and gsKit
[8ef73d05f022d14b3ded7c70cba670fe333ed61a](https://github.com/ps2dev/gsKit/commit/8ef73d05f022d14b3ded7c70cba670fe333ed61a)
(2026-09-16). These do **not** establish the port's installed SDK revision.
Bind experiments to the headers/libraries actually linked.

| ID | Pinned primary implementation | Locator and relevance |
|---|---|---|
| S1 | [PS2SDK dma.c](https://github.com/ps2dev/ps2sdk/blob/b04e8973c8a9839d0653d87a8d30cc67fd3c3f05/ee/dma/src/dma.c#L159) | `dma_channel_send_packet2`, chain/normal send/receive/waits. Chain payload flushing differs from tag-range flushing. Audit every REF target. |
| S2 | [PS2SDK kernel.S](https://github.com/ps2dev/ps2sdk/blob/b04e8973c8a9839d0653d87a8d30cc67fd3c3f05/ee/kernel/src/kernel.S#L698) | `sceSifWriteBackDCache`, `opDCache`, `_SyncDCache`, lines 698–818: real cache operations/barriers. |
| S3 | [PS2SDK sifrpc.c](https://github.com/ps2dev/ps2sdk/blob/b04e8973c8a9839d0653d87a8d30cc67fd3c3f05/ee/kernel/src/sifrpc.c#L180) | Request/response writeback, `sceSifCallRpc`, server DMA/lifetime. Read before sharing EE/IOP staging. |
| S4 | [PS2SDK audsrv EE RPC](https://github.com/ps2dev/ps2sdk/blob/b04e8973c8a9839d0653d87a8d30cc67fd3c3f05/ee/rpc/audsrv/src/audsrv_rpc.c#L254) | `audsrv_play_audio` and ADPCM upload: actual transfer limits/synchronization. |
| S5 | [PS2SDK audsrv header](https://github.com/ps2dev/ps2sdk/blob/b04e8973c8a9839d0653d87a8d30cc67fd3c3f05/ee/rpc/audsrv/include/audsrv.h#L95) | `wait_audio`, `play_audio`, `available`: queue semantics/accepted counts. Preserve short/zero writes/sample order. |
| S6 | [PS2SDK audsrv IOP](https://github.com/ps2dev/ps2sdk/blob/b04e8973c8a9839d0653d87a8d30cc67fd3c3f05/iop/sound/audsrv/src/audsrv.c#L278) | Availability, ring writes and stream service. Accepted bytes are limited to available space. |
| S7 | [PS2SDK fileXio RPC](https://github.com/ps2dev/ps2sdk/blob/b04e8973c8a9839d0653d87a8d30cc67fd3c3f05/ee/rpc/filexio/src/fileXio_rpc.c#L586) | `Read`, `WaitAsync` at 1061, `SetBlockMode` at 1099; shared RPC state/cache writeback constrain overlap. |
| S8 | [PS2SDK CDVD ncmd.c](https://github.com/ps2dev/ps2sdk/blob/b04e8973c8a9839d0653d87a8d30cc67fd3c3f05/ee/rpc/cdvd/src/ncmd.c#L184) | `sceCdRead`, alignment callback/NOWAIT RPC. Sector and unaligned-edge handling affect bounce-buffer lifetime. |
| S9 | [PS2SDK block cache](https://github.com/ps2dev/ps2sdk/blob/b04e8973c8a9839d0653d87a8d30cc67fd3c3f05/iop/fs/libbdm/src/bd_cache.c#L8) | Block constants, fill, overlap invalidation/destruction. IOP caching can duplicate EE windows; budget both. |
| S10 | [PS2SDK packet2 VIF](https://github.com/ps2dev/ps2sdk/blob/b04e8973c8a9839d0653d87a8d30cc67fd3c3f05/ee/packet2/include/packet2_vif.h#L34) | UNPACK/DIRECT open/close and STCYCL. Independent packet construction reference; M2 restrictions still apply. |
| S11 | [gsKit gsTexture.c](https://github.com/ps2dev/gsKit/blob/8ef73d05f022d14b3ded7c70cba670fe333ed61a/ee/gs/src/gsTexture.c#L31) | EE payload versus GS footprint; send at 233/upload at 425. T4/T8/CLUT handling; source size is not GS allocation size. |
| S12 | [gsKit gsCore.c](https://github.com/ps2dev/gsKit/blob/8ef73d05f022d14b3ded7c70cba670fe333ed61a/ee/gs/src/gsCore.c#L21) | VRAM allocation, `sync_flip` at 68, `finish` at 124 and queue submission. |
| S13 | [gsKit gsInit.c](https://github.com/ps2dev/gsKit/blob/8ef73d05f022d14b3ded7c70cba670fe333ed61a/ee/gs/src/gsInit.c#L331) | Color/depth allocation. Preserve modes, formats and page alignment. |
| S14 | [gsKit gsTexManager.c](https://github.com/ps2dev/gsKit/blob/8ef73d05f022d14b3ded7c70cba670fe333ed61a/ee/gs/src/gsTexManager.c#L90) | Split/merge, bind at 225, invalidate/free at 295/305. Compare contracts with the port's richer allocator; do not blindly replace it. |
| S15 | [PS2SDK libvux vuhw.c](https://github.com/ps2dev/ps2sdk/blob/b04e8973c8a9839d0653d87a8d30cc67fd3c3f05/ee/libvux/src/vuhw.c) | VU helper assembly reference; not an equivalence proof for current expression order. |
| S16 | Yann Collet, [LZ4 Block Format](https://github.com/lz4/lz4/blob/6b589e8053bd9c524138d78adec78fcaefa35cab/doc/lz4_Block_format.md), internally revised 2026-10-26 | Literals/matches, external size metadata, implementation limits. Exact byte compression; no media quantization implied. Current cooked reader remains the port authority. |
| S17 | [PCSX2 GSClut.cpp](https://github.com/PCSX2/pcsx2/blob/0ab00c49b157be667a8a2f0bf4cda83f75a9198e/pcsx2/GS/GSClut.cpp#L235), checked tip 2026-11-14 | Original emulator implementation, not a hardware specification. P4 loader takes its source from CBP and applies CSA to the internal destination; full P8 load visits source bands at 552–561. Corroborates M5 §3.4.7: CSA is not a source-band offset. |

The [gsKit README](https://github.com/ps2dev/gsKit/blob/8ef73d05f022d14b3ded7c70cba670fe333ed61a/README.md#important-notes)
documents scratchpad availability since v0.3 and queue considerations. That
does not prove all other libraries/threads in this port leave SPR/VU state free.

## Original reverse-engineering observations

| ID | Original source and date | Useful interpretation and limits |
|---|---|---|
| R1 | PSI-Rockin, [ps2tek](https://psi-rockin.github.io/ps2tek/), [source project](https://github.com/PSI-Rockin/ps2tek) | Searchable sections EE memory/cache, DMAC, GIF, VIF, GS, IOP, SIF. Describes itself as incomplete; uncertainty markers matter. Inspired by no$cash, not authored by no$cash. Confirm critical behavior against M1–M8 or hardware. |
| R2 | Ty Lamontagne, [PS2 DMAC Basics](https://fobes.dev/ee/2024/02/02/ps2-dmac-basics.html), 2024-02-02 | Worked original transfers including scratchpad/chains. Useful for small ownership tests, not an SRB2 benchmark. |
| R3 | Ty Lamontagne, [Fragmenting memory with the CT24 Format](https://fobes.dev/ps2/fragmenting-memory-with-the-ct24-format), 2024-06-01 | High-byte indexed formats alongside CT24/Z24. Research lead for bitplane-aware allocation. Its framebuffer suggestion assumes alpha is unnecessary; that is false for this port's general CT32 wipe/capture behavior. |
| R4 | Ty Lamontagne, [The VU0 Pipeline](https://fobes.dev/ps2/detecting-emu-vu0-pipeline), 2024-06-05 | Original Q-pipeline experiment; explicitly discusses PCSX2 1.7.5865. Retain correct dependencies/waits. Not a current survey of emulator revisions. |
| R5 | Ty Lamontagne, [When 1*X does not equal X](https://fobes.dev/ps2/detecting-emu-vu-floats), 2024-06-07 | Original VU multiply operand-order experiment. Algebraic identities alone cannot prove an exact VU replacement. |
| R6 | Ty Lamontagne, [GS Backpressure](https://fobes.dev/ps2/detecting-emu-gs-backpressure), 2024-06-17 | Original FINISH/Count test separates transfer and drawing cost. Emulator observations are dated; physical timing needs physical validation. |
| R7 | Ty Lamontagne, [PlayStation 2 Data Cache Mapping](https://fobes.dev/ps2/ps2-cache-mapping), 2024-12-09 | Hardware experiment with way locking/unmapped addresses. Sacrifices normal cache capacity and has no ordinary DMA backing. **Not** an acceptable extra-RAM technique here. |

## Visibility, geometry and distance sources

| ID | Checked primary source | Locator and boundary |
|---|---|---|
| V1 | Assarsson/Möller, [Optimized View Frustum Culling Algorithms for Bounding Boxes](https://fileadmin.cs.lth.se/cs/Personal/Tomas_Akenine-Moller/pubs/vfcullbox.pdf.gz), March 1999/revised February 2000; [author index](https://fileadmin.cs.lth.se/cs/Personal/Tomas_Akenine-Moller/publications.html) | Author-linked gzip expands to 10 PDF pages. §§3.1–3.5 pp3–6: plane tests, last-plane coherence, masking, translation/rotation bounds; §5 restrictions. Web reader failed gzip/redirects; direct GET/decompression succeeded. |
| V2 | Sýkora/Jelínek, [Efficient View Frustum Culling](https://old.cescg.org/CESCG-2002/DSykoraJJelinek/), CESCG 2002 | Original implementation/evaluation, HTML §§2–5. Compare hierarchy/coherence cost. PDF encoding is poor; readable HTML was consulted. Probabilistic erroneous rejection discussed there is unacceptable here. |
| V3 | Seth Teller, [Visibility Computations in Densely Occluded Polyhedral Environments](https://www2.eecs.berkeley.edu/Pubs/TechRpts/1992/6250.html), UCB/CSD-92-708, 1992; [thesis](https://www2.eecs.berkeley.edu/Pubs/TechRpts/1992/Archive/CSD-92-708.pdf) | Institution abstract describes conservative cell visibility, opaque separators and runtime view culling. Thesis is 184 scanned pages without extracted text; detailed body algorithms were **not** claimed read. Portal/PVS brings storage/preprocessing costs. |
| V4 | Greene/Kass/Miller, [Hierarchical Z-Buffer Visibility](https://www.cs.princeton.edu/courses/archive/spr01/cs598b/papers/greene93.pdf), SIGGRAPH 1993, 7-page mirror | §§3.1–3.3: object hierarchy, depth pyramid, temporal coherence. Web reader returned 403; direct GET/local text inspection succeeded. Software coverage/depth hierarchy needs its own budget. |
| V5 | Bittner/Wimmer/Piringer/Purgathofer, [Coherent Hierarchical Culling](https://www.cg.tuwien.ac.at/research/vr/chcull/bittner-eg04-chcull.pdf), Eurographics 2004 pp615–624; [author project](https://www.cg.tuwien.ac.at/research/vr/chcull/) | §§3–5: query scheduling/coherence; §1 static-PVS limits. Native hardware-query assumption is not a drop-in facility of this GS driver. Last frame's invisible answer is not a current proof. |
| V6 | id Software, [DOOM r_bsp.c](https://github.com/id-Software/DOOM/blob/master/linuxdoom-1.10/r_bsp.c) | Original `R_CheckBBox`, solid-span clipping and front-to-back BSP traversal. Engine-lineage reference. SRB2 slopes,3D floors, skybox views and translucency require current-engine rules. |

A conservative test may retain extra invisible geometry; it must never reject
geometry contributing to the reference output. Entity bounds must include the
actual sprite/model transform, frame extents, shadow, attachments, interpolation
and effect footprint. Cover every active view, including skybox/camera and
splitscreen cases. Preserve visible primitive order, depth ties, transparency
and resource ownership. These are port-specific proof obligations, not
performance claims from V1–V6.

**There is no universal lossless distance cutoff for entities.** The projection
defines `FAR_CLIPPING_PLANE 32768.0f` in
[ps2_hw_xform.inc](../src/ps2/hw/ps2_hw_xform.inc); PC GL uses the same nominal
value. An early rejection would need a conservative bound wholly outside the
**existing camera clip volume**, not a center farther than 32768 Euclidean map
units. Large objects can cross the plane; camera depth differs from radial
distance. Keep exact fallback for uncertain/boundary cases. Do not shorten the
projection or use fog, screen size or LOD to justify a cutoff.

Visibility does not authorize removing entities, stopping thinker/physics,
suppressing sound, changing RNG advancement or skipping network/game state.
Residency changes need a complete reload/lifetime contract preserving visible
output, capacities and failure behavior.

## What this repository already does

These are existing optimizations, not new discoveries. Source hashes bind this
reading; older report numbers remain historical fixture results, not current
whole-game budgets.

| Area | Source/report to read before designing another candidate |
|---|---|
| GS formats/CLUT/layout | [ps2_hw_tex.inc](../src/ps2/hw/ps2_hw_tex.inc), [ps2_hw_priv.inc](../src/ps2/hw/ps2_hw_priv.inc), [HW_RENDERER.md](HW_RENDERER.md). Exact palette paths/CT32 fallbacks, keyed/filtering RGB rules, block footprints and sorted/merged free ranges exist. `VRAM_RANGES` preserves worst-case 8193 ranges in the full 4 MiB pool. |
| Residency/uploads | `upload_ref`, `dcent_t`, `rel_poll`, `rel_room` in [texture source](../src/ps2/hw/ps2_hw_tex.inc). Zero-copy uploads pin sources through DMA, share keyed/plain indices and support legal reclamation. |
| Packet/GS ownership | [ps2_hw_gs.inc](../src/ps2/hw/ps2_hw_gs.inc), [ps2_hwd.c](../src/ps2/hw/ps2_hwd.c), [ps2_hw_vif.inc](../src/ps2/hw/ps2_hw_vif.inc). Ordered ring, range writeback, completion tracking, VIF ordering and separate FINISH handling. Short-fan REGLIST is in [the earlier GS report](PS2_ARCH_GS_20261004.md). |
| VU rendering | [ps2_hw_vu1.inc](../src/ps2/hw/ps2_hw_vu1.inc), [microcode](../src/ps2/hw/vu1/ps2_hw_vu1.vsm). Eligible transforms/clipping/submission already use bounded double-buffered VU data and fallback. |
| Early geometry/entity culls | [hw_front.inc](../src/hardware/hw_front.inc), [ps2_hw_fx2.inc](../src/ps2/hw/ps2_hw_fx2.inc), [hw_main.c](../src/hardware/hw_main.c). Existing five-plane side/near, plane/subsector/wall, sphere tests/exemptions. Far-plane addition is separate proof work. |
| Batches/derived geometry | [hw_batching.c](../src/hardware/hw_batching.c), [PS2_MEMORY.md](PS2_MEMORY.md). Chunked submission/reclaimable derived caches already address selected storage. Do not lower collection/polygon limits. |
| Captures | [ps2_hw_screen.inc](../src/ps2/hw/ps2_hw_screen.inc), [memory budgets](HW_RENDERER.md#real-memory-budgets). Full-size capture, EE spill/restore, protected active wipe resources already exist. Higher-resolution single-object fit is separate. |
| Audio/VU0 coexistence | [ps2_vu0a.h](../src/ps2/vorbis/ps2_vu0a.h). Renderer/audio share VU0; state preservation across threads already matters. A new kernel cannot assume private ownership. |
| Pack/IO scratch | [w_pack.c](../src/w_pack.c), [third-pass scratch report](PS2_PACK_SCRATCH_NEXT3_20261114.md). Lifetime-proven reuse removed a separate 64 KiB LZ4 buffer. Non-reentrancy, retry, callbacks and music-thread boundaries must survive. |
| Measurement | [ps2_prof.c](../src/ps2/ps2_prof.c), [ps2_loadprof.c](../src/ps2/ps2_loadprof.c), GS reports and zone/cache traces. Identify actual simultaneous lifetimes and reconstruction cost. |

The [eleventh-pass slope report](PS2_MEMORY_NEXT11_20261115.md) records
**33,296 B** saved at the byte-budget/73-slot slopes checkpoint. Both that
candidate and its preceding header/64-slot experiment worsened the matched
game cache-pressure workload and were rejected. Roughly 240 KiB less late used
zone represented different cache occupancy, not fixed resident saving. The
[768-byte pack overlay](PS2_IO_NEXT11_20261115.md) and
[CLUT permutation](PS2_GS_NEXT11_20261115.md) were rejected too. Component
equality and smaller storage did not establish an acceptable integrated result.

## Candidate families and exactness gates

These are engineering inferences, **unmeasured and unaccepted** by this catalog.

| Candidate | Potential benefit | Necessary preservation and cost checks |
|---|---|---|
| Eliminate duplicate live representations | EE RAM/peak | Prove nonoverlapping lifetimes, aliases, callbacks, failure/retry publication and DMA users. Account for zone/libc/stack/alignment; preserve reserves/capacities. Existing pack reuse proves only that specific buffer. |
| Share identical immutable derived data | EE storage/reconstruction | Complete keys/generations, pointer identity consumers, teardown/reload. Metadata can exceed payload savings; partial hash equality is insufficient. |
| Lazy exact derivation/compressed residency | EE live set | Canonical bytes, dimensions/counts and reload behavior remain complete. Decode before first use. Measure transient peak, IO, evictions/failures/CPU. S16 is byte compression, not lossy media conversion. |
| PSMT4 for a representable index subset | Payload/upload/possibly GS footprint | At most 16 required **index identities**, exact mapping and all palette/translation generations, chroma key, alpha-zero RGB/filtering. Retain T8/CT32 fallback and limits. Prove legal transfer padding/TBW and internal CLUT loading (CSA is not a source-band offset). Measure padded blocks plus CLUT/metadata. M5/S11/S17 establish mechanisms, not eligibility. |
| Unused high bits of existing Z24 storage | GS occupancy | Research only (R3/M5). Prove bitplane/address ownership through every clear/draw/transfer/readback/mode/reset. Preserve depth precision and CT32 alpha. Do not switch general color buffers to CT24 or assume all writers preserve spare bits. |
| Lossless tiled captures/textures | Single-object fit/transient VRAM | Preserve exact texels/dimensions, wrap/filter edges, ordering/effects; tiles reconstruct the continuous image. Budget extra EE/packet state/latency. Spill alone cannot make an oversized single allocation fit. |
| Earlier bounds/hierarchy plane masks | Avoid geometry/resource work | V1/V2; reuse existing BSP/plane data first. Dynamic bounds/all views; cached reject plane may be tested first, but cached invisibility needs a valid current proof. Added arrays may exceed avoided storage. |
| Portal/PVS or software conservative occlusion | Avoid hidden construction | V3/V4. Moving sectors/doors, 3D floors, holes, masks/translucency and alternate views invalidate naive static occluders. Unknown stays visible. A few covered samples do not prove full occlusion. Include hierarchy/PVS/pyramid cost. |
| Smaller exact packets/MMI/dirty state | CPU/traffic/transient bytes | Preserve register values, side effects/kick order, padding, ownership. Existing REGLIST/state work must not be duplicated. Compare lane/edge cases and actual target code. No FP algebra shortcut. |
| Scratchpad/locality/VU scheduling | Physical bus/cache efficiency | Bounded storage/ownership, library/thread context, Q hazards, numeric order. No cache-way capacity reduction, unknown physical-address tricks, narrowed geometry or changed precision. M3/M7/A3/R4/R5 motivate hardware checks. |
| DMA/path/IO overlap/read grouping | Reduced stalls/reconstruction | S1–S10/A6: source/destination lifetimes, cache-line edges, completion/error order, unchanged queues/streams. Device-specific measurements; more in-flight buffers can increase RAM/pinned bytes. |
| Audio staging reuse | EE/IOP RAM | Identical PCM/decoder position/rate/voices, short writes/queue depth. M8/S4–S6 do not authorize ADPCM conversion, downsampling or a numerically different mixer. |

No fixed RAM/VRAM saving follows from these sources. Changing allocation
addresses can alter cache pressure; complete measured runs decide net results.

## Measurement before retaining a candidate

Bind the preceding accepted control and candidate's exact ELF, default LTO code,
layout, assets, configuration, mode and matched input/seed. Keep production
RNG/physics unchanged. Compare output/state first, including failure/retry,
repeated map/mode changes and shutdown. Inspect actual target instructions and
data, not only a host helper.

Record EE arena available to the game; used/peak zone; largest free block;
allocation/failure counts; cache bytes/evictions/reconstruction; libc/spill/stack
high-water; GS padded blocks/CLUT/capture peaks; upload/readback bytes and waits.
Preserve capacities and quality-warning/error behavior. Distinguish static
bytes, simultaneous live bytes, peak and changed cache occupancy.

Separate CPU submission, transfer and completed drawing. M3 §7 counters on a
physical console are needed for cache/bus claims; report events/setup/overhead
and repeatability. FINISH timing needs a clear boundary without accidentally
adding a critical-path wait. PCSX2 functional and COP0 comparisons remain
useful, but dated R4–R6 experiments cannot certify physical VU/cache/GS timing.
**This research ran no performance tests.**

## Coverage and gaps

Covered: EE core/cache/SPR/UCAB/MMI/FPU, VU0/VU1/VIF, GIF/DMAC/coherency,
GS formats/CLUT/layout/Z/transfers, IOP/SIF/SPU2/IO, exact residency,
profiling, BSP/frustum/entity bounds, portal/PVS and occlusion approaches.

Not established: complete Sony manual/errata history, proprietary Performance
Analyser availability, all hardware revisions, every installed library's
private SPR use, current emulator timing coverage, physical performance, every
SRB2/custom-map visibility case or a universal distance threshold. The scanned
Teller body was not fully read. Historical sources sometimes need an archive,
gzip or direct HTTP reader; access limits are recorded instead of treating
snippets as full documents. This catalog ships no RAM/VRAM change.
