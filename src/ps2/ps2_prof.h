// SONIC ROBO BLAST 2 - PS2 port
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  ps2_prof.h
/// \brief Per-phase EE cycle profiler (COP0 Count), diagnostic builds only

#ifndef __PS2_PROF__
#define __PS2_PROF__

// Diagnostic build (PS2REF, tools/ps2/build.py --ps2ref): the phases are measured by linker wrappers (-Wl,--wrap=...)
// around the engine functions, so the engine sources carry no profiler code. Runtime switch: -ps2prof.
// Output: one "PROF ..." line per 105 rendered frames in the engine log (cycles are sums over the window; the
// phases are exclusive: a nested phase is not counted in its parent).

typedef enum
{
	PP_OTHER, // main loop, menus, everything not listed (also the idle wait)
	PP_GTICK, // G_Ticker without P_Ticker (menus' tickers, HUD tickers, level load triggers)
	PP_PTICK, // P_Ticker: thinkers, collisions, sector specials
	PP_RENDER, // R_RenderPlayerView without the three below (view setup, sprite sort, FOF setup, portals)
	PP_BSP, // R_RenderBSPNode: BSP walk, wall columns, visplane building
	PP_PLANES, // R_DrawPlanes: floor/ceiling spans, slopes, sky
	PP_MASKED, // R_DrawMasked: sprites, masked and translucent walls
	PP_HUD, // ST_Drawer, HU_Drawer, M_Drawer, CON_Drawer
	PP_GS, // I_FinishUpdate: palette, copy/DMA to the GS, wait for the flip
	PP_AUDIO, // I_UpdateSound, S_UpdateSounds
	PP_SLEEP, // I_Sleep / I_SleepDuration
	PP_COUNT
} ps2prof_phase_t;

void PS2Prof_Enter(ps2prof_phase_t phase);
void PS2Prof_Leave(void);
#ifdef PS2_PROF_DIRECT
void PS2Prof_FrameEnd(void); // frame counter of LTO profile builds (called from I_FinishUpdate)
#endif

#include "../ps2_sub.h"

#endif
