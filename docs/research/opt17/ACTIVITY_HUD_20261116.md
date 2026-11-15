# CPU / GPU~ activity in the memory HUD — 2026-11-16

The existing `showmem` display now adds an integer activity line above its two
memory lines: `CPU 72%  GPU~ 38%`. Saved values remain compatible: Off=0,
On=1, RAM=2, VRAM=3. RAM and VRAM retain their existing details in each mode.
The menu label is “Show RAM/VRAM/CPU/GPU”. This reviewed change is applied to
the working tree; combined full-game native validation is pending.

## Meaning of the percentages

**CPU** is foreground EE activity over the sampled elapsed time: elapsed CP0
Count ticks minus the union of explicitly instrumented main-thread waits,
rounded to 0–100%. It excludes the frame-cap sleep and the software/hardware
display driver's DMA, FINISH and vblank waits. Nested sleeps inside a driver
wait count once. Audio/network worker sleeps do not count against the main
thread. This is a foreground wall-time measure, not an OS-wide measurement of
executing instructions or all EE threads.

**GPU~** is observed GS queue activity: the union from actual DMA submission
until an existing FINISH observation, divided by the same elapsed window and
rounded to 0–100%. It includes CPU feeding gaps, transfer/VU work and the delay
before the program observes FINISH. It is an upper queue-span estimate, not
the GS's unobservable exact busy fraction. It is not derived from CPU, FPS,
draw count, VRAM occupancy or a guessed workload ratio. Existing FINISH
observations can be as late as vblank. `GPU~ N/A` appears during initialization
and renderer switches until a known FINISH and a subsequent wholly observed
window establish coverage. Both rendering paths provide the same defined
queue-span estimate.

The text refreshes with the existing nine-tic HUD cadence. Percentages use
integer arithmetic, including bounded 64-bit intermediate products; source
and output values are clamped to 0–100. Counter deltas support CP0 Count wrap.
Interval tokens reject late FINISH observers after an IRQ has already closed
the old interval and started a new one. Generation tokens also reject wait
ends left behind by a renderer reset or abandoned frame.

## Instrumentation and ownership

`ps2_activity.c` contains arithmetic and state only: no clocks, allocation,
kernel calls, GS access, polling or timers. Callers supply existing-clock
samples. GPU update/snapshot/reset paths are serialized against their IRQ
consumers; CPU idle scopes identify the thread that enabled the display.
Standalone driver fixtures can omit the module: weak hooks are checked before
access, and their clock arguments are not evaluated while disabled.

The isolated source and exact ten-file before/after hashes are frozen under
`build/nov17-activity/source-proof.json`; the reviewed patch is
`build/nov17-activity/activity-hud.patch`, SHA256
`ddb892d2dc917761bdbb767598d0a478fe4ef96c4d36b794f593a4a1a0f39a97`.
`hook-audit.json` establishes that all existing GS_CSR expressions in the
software driver, hardware GS backend and driver entry point are unchanged.
The meter adds no FINISH command, fence, GS poll or timer. The PS2 build source
list adds the single activity translation unit.

## Memory, formatting and placement

The original 16-row bitmap/coverage/Doom-patch workspace is reused to compose
the two-line memory patch and the separate one-line activity patch. Its
**22192 bytes** are now optional allocation while the display is enabled,
and are freed with both cached patches when switched Off. Allocation failure
uses the direct-text path. Stable frames allocate nothing. The extra activity
line shares the existing right/bottom snapping and FPS/ping clearance.

The RAM detail string buffer grows from 40 to 64 bytes: maximum legal
128-MiB readings require 47 characters plus NUL. This preserves the detail
instead of truncating it or overflowing into width fields. Stock TNYFN font
widths, including the `~` and `%` glyphs, are checked. Three-line placement is
checked at 320×200 and 640×400 with FPS/ping combinations and all saved display
modes.

EE non-LTO object-section measurements are:

| Object | Text | Data | BSS |
|---|---:|---:|---:|
| Original memory HUD | 9267 B | 104 B | 22628 B |
| Updated memory HUD | 10387 B | 104 B | 784 B |
| Activity counters | 1056 B | 0 B | 60 B |

The measured BSS reduction for those objects is **21784 bytes**. This is not
a linked-engine peak-memory or performance claim. Enabled cached patches and
the optional workspace still consume RAM, and the enabled meter has runtime
cost.

## Validation

`tools/ps2/opt17/activity_hosttest.py` compiles the actual counter source and
extracts the actual formatting, bitmap composition, refresh, placement, draw
and Off paths. Engine allocations, drawing and clocks are simulated. Stock
asset font widths are used. Normal and ASan/UBSan runs pass **528016 checks**:

- Nested wait unions, worker-thread exclusion, open-window clipping, timer
  wrap, reset generations, serial wrap and late-IRQ FINISH rejection.
- 300 randomized 1000-event sequences against an independent interval-union
  reference, plus all integer percentages from 0 through 100.
- Every CPU/GPU percentage pair, string canaries, maximum legal memory detail,
  Doom-patch column/payload bounds, allocation-failure text fallback, stable
  frame allocation counts and release on Off.
- All three enabled modes, both renderer descriptions, both target sizes and
  FPS/ping combinations.

Four injected controls are detected: restarting the outer idle interval at a
nested wait, accepting an old GPU token, restoring the old 40-byte detail
buffer, and evaluating clock arguments while disabled. An unlinked optional
hook binary passes. All six changed C units pass EE syntax checking. The
public standalone HW_TEST fixture links without the activity module with no
compile/link diagnostics beyond the already allowed SDK ABI warning.

The [host evidence and committable-file manifest](ACTIVITY_HUD_HOST_RESULTS_20261116.json)
bind the source, tools, font asset, section measurements and fixture results.
The next gate is the combined shipping build in the 32-MiB emulator profile:
Hardware/Software switches, resolution changes, On/Off/RAM/VRAM screenshots
and FPS/ping placement. Physical-console utilization accuracy is not claimed.
