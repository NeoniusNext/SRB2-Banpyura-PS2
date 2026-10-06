// SONIC ROBO BLAST 2 - PS2 port
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// ile  ps2_sub.h
/// rief Fine-grained cycle probes inside engine functions (diagnostic builds only)

#ifndef __PS2_SUB__
#define __PS2_SUB__

// build.py --subprof defines PS2_SUBPROF: PS2SUB_B(n)/PS2SUB_E(n) measure the COP0 cycles between them (inclusive,
// nesting different ids is fine, one id must not overlap itself) and count the calls; src/ps2/ps2_prof.c prints a
// "SUB win=..." line per profile window and tools/ps2/opt2c_sub.py names the ids. Without PS2_SUBPROF (every normal
// build, the PC build too) the macros are empty: the probes stay in the sources.
#ifdef PS2_SUBPROF
extern unsigned long long ps2sub_cyc[128], ps2sub_calls[128];
static inline unsigned PS2Sub_Count(void) { unsigned v; __asm__ volatile("mfc0 %0,$9" : "=r"(v)); return v; }
#define PS2SUB_B(n) unsigned ps2sb_##n = PS2Sub_Count()
#define PS2SUB_E(n) do { ps2sub_cyc[n] += (unsigned)(PS2Sub_Count() - ps2sb_##n); ps2sub_calls[n]++; } while (0)
#define PS2SUB_N(n) (ps2sub_calls[n]++)
#define PS2SUB_ADD(n, v) (ps2sub_calls[n] += (unsigned long long)(v)) // counters (pixels, columns): the "calls" column
#else
#define PS2SUB_B(n) ((void)0)
#define PS2SUB_E(n) ((void)0)
#define PS2SUB_N(n) ((void)0)
#define PS2SUB_ADD(n, v) ((void)0)
#endif

#endif
