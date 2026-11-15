# High-resolution and same-level restart audit — 2026-11-16

The unchanged v7 renderer completes the tested Software/Hardware and resolution
changes on a real 32-MiB PCSX2 profile, but **fails the clean-render gate**:
640×480 GS storage leaves too little texture memory for SKY1, producing a black
sky. A separate actual-source host probe reproduces planner timestamps surviving
a GS restart while the driver frame counter returns to zero. The planner fix
described below is now applied to the working-tree renderer after review; its
integrated native run is pending. The high-resolution capacity failure remains.

The source under test is `build/nov16-hwopt/candidate-v7/source`, and the native
ELF is `build/nov16-hwopt/candidate-v7/hardware/SRB2.ELF`. Hashes of the ELF,
staged ELF, assets, source, profile, command arguments, logs and screenshots are
in [the native report](HIGHRES_NATIVE_RESULTS_20261116.json). The profile and
engine log both establish 32 MiB. This is not physical PS2 validation.

## Mode and memory contract

`ps2_vmodes.h` enumerates exactly 11 logical modes. Indices are stable:

| Index | Logical resolution | Five EE screen buffers |
|---:|---:|---:|
| 0 | 320×200 | 320000 B |
| 1 | 320×224 | 358400 B |
| 2 | 320×240 | 384000 B |
| 3 | 320×256 | 409600 B |
| 4 | 400×300 | 600000 B |
| 5 | 512×384 | 983040 B |
| 6 | 512×448 | 1146880 B |
| 7 | 640×400 | 1280000 B |
| 8 | 640×448 | 1433600 B |
| 9 | 640×480 | 1536000 B |
| 10 | 640×512 | 1638400 B |

`VID_NumModes`, `VID_GetModeName`, `VID_GetModeForSize`, `VID_SetMode` and
`vid_mode` all use that table. `MAXVIDWIDTH/HEIGHT` are 640×512. 800×600 is a
VESA **output** format with a 400×300 software framebuffer; it is not an 800×600
logical mode. The current Hardware driver supports NTSC, PAL and 480p and
resolves the other output selections to console region. This audit adds no
resolution, lowers no requested resolution and changes no colour format.

All 640-wide logical modes use the following existing CT32 GS layouts:

| Output | Actual GS size | Each FB | Z24 layout | CLUT reservation | Remaining pool |
|---|---:|---:|---:|---:|---:|
| NTSC | 640×448 | 1146880 B | 1146880 B | 131072 B | 622592 B |
| PAL | 640×512 | 1310720 B | 1310720 B | 131072 B | 131072 B |
| 480p | 640×480 | 1228800 B | 1228800 B | 131072 B | 376832 B |

Two FBs + Z + CLUT + pool sum to exactly 4194304 bytes. Z24 uses the 32-bit
layout. Logical modes taller than the chosen output's GS size already resample:
640×480/512 on NTSC and 640×512 on 480p. Exact 640×480 GS rendering was observed
on 480p; 640×512 PAL rendering is source/host checked, not native checked here.
The primary allocation authority is the GS manual, catalog M5 §§2/8 in
[the existing architecture catalog](../../PS2_ARCHITECTURE_AND_OPTIMIZATION_SOURCES_20261115.md).

A screen capture currently requires one contiguous FB-sized pool allocation.
That is impossible in all 12 of the 640-wide/output combinations and in PAL
modes 4–6: **15/33 layouts**. The actual `scr_tex_get`/pool allocator confirms
this, not just arithmetic. EE spilling cannot fix an object larger than the
entire pool. Water/heat (`hw_main.c` `HWR_DoPostProcessor`), wipes and last-level
captures consume this path. Default-mode capture tests do not cover this limit.

## Native same-level transitions

One unchanged-v7 run used the official locked `opt_run.py`/`run_pcsx2.py` path,
MAP01, 480p, `-zsingle -zck`, and these ordered `-vidshot` actions:

```text
l12=vid_mode~7,l24=renderer~Software,l36=vid_mode~9,
l48=renderer~Hardware,l60=vid_mode~8,l72=vid_mode~0,l84
```

Observed order: H320×200 → H640×400 → S640×400 → S640×480 → H640×480 →
H640×448 → H320×200. All seven complete PPM headers/payload sizes match the
requested logical sizes. The driver logs show 320×240 then 640×480 CT32 GS
storage, including return to 320×240. The 640×480 → 640×448 logical change uses
the same physical GS storage. Exit was zero, `VIDSHOT COMPLETE 7` appeared, and
the runner reported zero engine errors. `-hwnofb` was diagnostic and disabled
normal fallback; this run does not establish normal guard behaviour.

Nevertheless, two high-resolution starts log SKY1 (1024×512 P8, at least
524288 bytes) failing an empty 376832-byte pool. `imm_prepare(NULL)` explicitly
requests full resolution for the sky. The later 640×480 Hardware screenshot
has a black sky interior: illustrative fixed ROI [288,65,352,90] contains
1302/1600 black pixels versus 0/1600 in the earlier Software screenshot. Those
shots use different tics and renderers; this is not a pixel-parity metric.
GFS_CLD4 also fails admission on the first high-resolution frame.

Only the first four texture failures per driver initialization are logged, so
the **eight logged failures are a lower bound**, not a total. The 40
`selected while collecting` HWSKIP lines are the inherited normal-mode
diagnostic already recorded by the parent investigation. No clean-render,
performance or full-map coverage claim follows from successful termination.

## Actual-source host checks and isolated timestamp fix

`tools/ps2/opt17/highres_state_hosttest.py` compiles the frozen size chooser,
`PS2HWD_SetScreenSize`, transforms, allocator, capture code and VU material keys.
It checks 33 layouts, 456 alternating same-FB changes and 254 different-FB
pairs. It verifies full viewport/scissor refresh, logical/internal scale,
palette/filter/shader restoration, owner-handle clearing, GS coordinate bounds
and VU key rejection after viewport changes. GS bring-up/shutdown/fencing are
simulated; texture/cache cleanup is production code. Normal and ASan/UBSan runs pass.
All three injected stale-scale, stale-viewport and palette-loss controls fail
their expected checks. Evidence is `build/nov17-highres/host-final/results.json`.

The separate `highres_epoch_probe.py` uses actual `plan_key`, `plan_reset`,
`tex_want` and the exact Init/Shutdown state-reset statements. Engine-owned
texture metadata survives the modeled same-level restart. An old
`ps2_planfr == 1` then matches new frame zero and returns NULL from an empty
plan table; an expired `ps2_full_fr == 1` also revives and asks for L0 when the
new plan asks for L3. Frozen-source output is:

```text
stale_registration=1 stale_full_hint=1 planned_lod_without_stale_hint=3
```

The probe also covers a texture last used in the old driver's final frame
(stamp 201), delayed revival when reset frame numbers catch that old stamp,
and an actual 640×400 → 640×480 same-storage `PS2HWD_SetScreenSize` call. The
initial global-frame-clock prototype is rejected: it preserves the recent
old full-size/immediate hints and changes frame diagnostics.

The reviewed fix uses a separate four-byte planner clock. Only the four
engine-owned planner stamp interfaces use it; non-planner frame consumers keep
their existing counter. `plan_reset` advances the clock by 153, exceeding both
the 150-frame full-size lifetime and two-frame immediate hint lifetime. Stamp
zero is reserved, and clock advancement skips it on wrap. A same-storage
logical resize first uses checked `PS2HWD_Sync`, clears the deferred immediate
selection and resets the plan. `dma_fence` alone can return without submitting
an open VU chunk when there are no referenced texture payloads; Sync closes and
submits the packet/VU chunk, drains DMA and waits for GS FINISH.

The exact five-file patch and before/after hashes are frozen in
`build/nov17-highres/planner-clock/{prototype.patch,source-proof.json}`. All
five working-tree renderer files match the after hashes at handoff. Candidate
reports are in `build/nov17-highres/planner-clock/host-final/results.json` and
`additional-results.json`: all stale-state checks pass, 322 tick/reset cases
near UINT_MAX pass, ASan/UBSan passes, and injected short-gap and removed-wrap
guard controls fail. EE GCC syntax checking passes. The layout/viewport harness
also exercises this isolated source; see its `state-host` evidence. Native
integration into the combined build remains pending. This fix changes no
texture/capture capacities or colour format.

The complete, hashed inventory of the 104 original `H.frame_no` references is
[retained separately](HIGHRES_FRAME_CONSUMERS_20261116.json). It includes texture
LRU/working-set/done/LOD stamps, scan direction, VU/sprite material keys, palette
and lighting gates, model budget, touch/upload caches, tracing and validation.
Those independent frame consumers are outside the planner timestamp fix.

The adaptive-cap retry concern raised during the alpha audit is dormant in this
frozen production configuration: Init forcibly sets `H.cap_adapt = 0`, and the
allocation loop exits before `cap_step`. Explicit `-hwtexcap` starts without
cached planned-level source selection. Re-enabling adaptive caps is not a fix
established by this audit.

Lossless high-resolution completion still requires tiled capture/texture
residency and compositing, or a coordinated FB/Z layout. Reducing logical/GS
resolution, changing CT32 to CT16S, skipping features, or removing allocation
checks would not satisfy the user request.
