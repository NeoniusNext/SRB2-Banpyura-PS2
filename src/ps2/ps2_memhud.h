// SRB2 PS2 port: showmem, the RAM / VRAM counter drawn like the FPS counter (OPT11-MEM, PS2-HW-300..).
//
// cv_showmem Off / On (RAM and VRAM) / RAM / VRAM. Off costs one compare in I_FinishUpdate: nothing is sampled, formatted or drawn.
// The numbers are read from counters the engine keeps anyway (zone arena totals, the C heap break, the GS texture pool block count), a few
// times a second; the text is built with integer arithmetic into static buffers (no allocation, no float printf) and drawn every frame with
// the small font in the lower right corner above the FPS counter (and above the ping display when that is shown).

#ifndef __PS2_MEMHUD_H__
#define __PS2_MEMHUD_H__

#include "../command.h"

extern consvar_t cv_showmem;

typedef struct
{
	unsigned ram_total, ram_used, ram_free; // KiB: the EE RAM (32 MiB; 128 MiB on a development machine), what is used, what is free (zone + what the C heap can still take)
	unsigned zone_free, zone_big, zone_peak, libc_free; // KiB: free bytes of the zone arena, its largest free block, its highest use since the start, the C heap's room
	unsigned vram_total, vram_used; // KiB: the GS memory (4 MiB) and what is taken
	unsigned vram_fb, vram_z, vram_clut; // KiB: frame buffers (both), Z buffer (hardware renderer), CLUT area (hardware) / CLUT page (software)
	unsigned vram_tex_total, vram_tex_used, vram_ws; // KiB: hardware renderer: the texture pool, its allocation, the last frame's working set; software: the source texture (used = total)
	int hw; // the hardware renderer's driver is up (vram_tex_* describe its pool)
	int ram_level, vram_level; // 0 fine, 1 getting tight (yellow), 2 critical (red)
} ps2memhud_t;

// Reads the counters (what the display does every ~0.28 s; also for tests and the log line).
void PS2MemHud_Sample(ps2memhud_t *out);
// Draws the counter for the frame being finished (call when cv_showmem.value != 0).
void PS2MemHud_Draw(void);
// The end of a -zquit run: "SHOWMEM now" line with a fresh reading (when showmem is on), for the comparison with the "[zmem] final" and ZSTAT lines.
void PS2MemHud_Check(void);
// "HWPROF60 showmem ..." line of the profile window and reset of its counters (the hardware profile).
void PS2MemHud_ProfLine(unsigned frames);

#endif
