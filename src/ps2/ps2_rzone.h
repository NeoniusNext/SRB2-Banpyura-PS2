// SONIC ROBO BLAST 2 - PS2 port
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  ps2_rzone.h
/// \brief Renderer zone profiler (exclusive COP0 cycles per named zone). Diagnostic only: PS2_RZONE is not defined
///        in normal builds and the macros vanish; nothing in the engine depends on it.

#ifndef __PS2_RZONE__
#define __PS2_RZONE__

// -DPS2_RZONE is passed by tools/ps2/rzone_build.py only; the instrumented copies include this header.

#ifdef PS2_RZONE
typedef enum
{
	RZ_NONE,
	RZ_ADDLINE, RZ_CHECKBBOX, RZ_SUBSECTOR, RZ_CLIPSOLID, RZ_CLIPPASS,
	RZ_STOREWALL, RZ_SEGLOOP, RZ_WALLCOL, RZ_FINDPLANE, RZ_CHECKPLANE,
	RZ_DRAWPLANES, RZ_MAKESPANS, RZ_MAPPLANE, RZ_SPAN, RZ_TILTSPAN, RZ_SLOPESETUP, RZ_SKY,
	RZ_PROJECT, RZ_CLIPSPR, RZ_SORT, RZ_MASKEDCOL, RZ_SPRCOL, RZ_MASKEDSEG, RZ_VISSPRITE, RZ_DRAWNODES,
	RZ_SPLAT, RZ_THICK, RZ_USER1, RZ_USER2, RZ_USER3, RZ_USER4,
	RZ_COUNT
} rzone_t;

extern unsigned long long rz_acc[RZ_COUNT];
extern unsigned rz_calls[RZ_COUNT];
extern unsigned char rz_stack[16];
extern int rz_sp;
extern unsigned rz_last;

static inline unsigned rz_now(void)
{
	unsigned v;
	__asm__ volatile("mfc0 %0,$9" : "=r"(v));
	return v;
}

static inline void RZ_Enter(rzone_t z)
{
	const unsigned now = rz_now();
	rz_acc[rz_stack[rz_sp]] += now - rz_last;
	rz_stack[++rz_sp] = (unsigned char)z;
	rz_calls[z]++;
	rz_last = rz_now();
}

static inline void RZ_Leave(void)
{
	const unsigned now = rz_now();
	rz_acc[rz_stack[rz_sp]] += now - rz_last;
	rz_sp--;
	rz_last = rz_now();
}
#define RZ_ENTER(z) RZ_Enter(z)
#define RZ_LEAVE() RZ_Leave()
#else
#define RZ_ENTER(z) ((void)0)
#define RZ_LEAVE() ((void)0)
#endif

#endif
