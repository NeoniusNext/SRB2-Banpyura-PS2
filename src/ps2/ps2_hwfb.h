// SRB2 PS2 port: running out of memory is not the end of the game (PS2-170, OPT11-STAB).
//
// The frame is drawn under a guard (z_zone.h, Z_GUARD_TRY). When an allocation finds no room after every eviction and reclaim hook, the allocator jumps back
// here instead of ending the program:
//  - hardware renderer: the half drawn frame is dropped, the GS driver is shut down and the game goes on in the software renderer (the same switch as
//    Options -> Video), the map is remembered as "does not fit in hardware"; at the next level the hardware renderer is tried again (back to Hardware)
//    when the map is not on that list;
//  - software renderer: every cache is flushed (the free space joins into big blocks again), the frame is drawn again; three failures in a row mean
//    the map really does not fit: the usual out-of-memory report and error.
// The same guard catches the resource failures of the GS driver (a stuck DMA, a texture that cannot be uploaded): "PS2 HW resource failure" used to be fatal.

#ifndef __PS2_HWFB_H__
#define __PS2_HWFB_H__

#include "../doomtype.h"

void PS2HWFB_Display(void (*display)(void)); // D_Display under the guard (d_main.c)
boolean PS2HWFB_LoadLevel(void);              // P_LoadLevel(false, false) under the guard (g_game.c, G_DoLoadLevel); FALSE: the map does not fit in memory, the caller leaves to the title
void PS2HWFB_PreLoad(UINT32 numssectors);    // the start of the map load (p_setup.c): a huge map goes to software before anything is loaded
void PS2HWFB_BuildLevel(void);               // the hardware part of a level load (p_setup.c, replaces HWR_LoadLevel): guarded, falls back to software
void PS2HWFB_LevelLoaded(void);              // the end of P_LoadLevel: back to the hardware renderer when it was given up for an earlier level
boolean PS2HWFB_ForcedSoftware(void);        // the user wants Hardware, the game runs Software because of memory
void PS2HWFB_Init(void);                     // command line (-hwfbtest N: leave the hardware renderer as if memory ran out at frame N)
void PS2HWFB_Report(void);                   // one line with the counters (end of the memory report)

#endif
