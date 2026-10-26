// SONIC ROBO BLAST 2 - PS2 port
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  ps2_prof.c
/// \brief Per-phase EE cycle profiler (COP0 Count), see ps2_prof.h

#include "../doomdef.h"
#include "../m_argv.h"
#include "../console.h"
#include "../g_game.h"
#include "../doomstat.h" // gametic
#include "../p_tick.h"
#include "../r_main.h"
#include "../r_bsp.h"
#include "../r_plane.h"
#include "../r_things.h"
#include "../st_stuff.h"
#include "../hu_stuff.h"
#include "../m_menu.h"
#include "../i_video.h"
#include "../i_sound.h"
#include "../s_sound.h"
#include "../i_system.h"
#include "../i_time.h"
#include "ps2_prof.h"

#define WINDOW_FRAMES 105
#define MAXNEST 8


#ifdef PS2_MEMPROF
// build.py --memprof: linker wrappers of memcpy/memset/memmove count calls, bytes and callers (return addresses); the MC lines
// of each window (top callers by bytes) are resolved with addr2line. The wrappers call the real functions.
void *__real_memcpy(void *, const void *, size_t);
void *__real_memset(void *, int, size_t);
void *__real_memmove(void *, const void *, size_t);
#define MC_SLOTS 256
static struct { UINT32 ra; UINT32 calls, bytes; UINT8 kind; } mc_tab[MC_SLOTS];
static UINT32 mc_total[3][2];

static inline void MC_Note(int kind, UINT32 ra, size_t n)
{
	UINT32 h = ((ra >> 2) ^ (ra >> 10) ^ (UINT32)kind * 977u) & (MC_SLOTS - 1);
	int tries;
	for (tries = 0; tries < MC_SLOTS; tries++, h = (h + 1) & (MC_SLOTS - 1))
	{
		if (mc_tab[h].calls == 0 || (mc_tab[h].ra == ra && mc_tab[h].kind == kind))
		{
			mc_tab[h].ra = ra;
			mc_tab[h].kind = (UINT8)kind;
			mc_tab[h].calls++;
			mc_tab[h].bytes += (UINT32)n;
			break;
		}
	}
	mc_total[kind][0]++;
	mc_total[kind][1] += (UINT32)n;
}

void *__wrap_memcpy(void *d, const void *s, size_t n)
{
	MC_Note(0, (UINT32)__builtin_return_address(0), n);
	return __real_memcpy(d, s, n);
}
void *__wrap_memset(void *d, int c, size_t n)
{
	MC_Note(1, (UINT32)__builtin_return_address(0), n);
	return __real_memset(d, c, n);
}
void *__wrap_memmove(void *d, const void *s, size_t n)
{
	MC_Note(2, (UINT32)__builtin_return_address(0), n);
	return __real_memmove(d, s, n);
}

static void MC_Report(UINT32 windows)
{
	int i, k;
	I_OutputMsg("MC win=%u cpy=%u/%u set=%u/%u mov=%u/%u\n", (unsigned)windows, mc_total[0][0], mc_total[0][1], mc_total[1][0], mc_total[1][1], mc_total[2][0], mc_total[2][1]);
	for (k = 0; k < 24; k++)
	{
		int best = -1;
		for (i = 0; i < MC_SLOTS; i++)
			if (mc_tab[i].calls && (best < 0 || mc_tab[i].bytes > mc_tab[best].bytes))
				best = i;
		if (best < 0)
			break;
		I_OutputMsg("MCR %u %d %u %u\n", mc_tab[best].ra, (int)mc_tab[best].kind, mc_tab[best].calls, mc_tab[best].bytes);
		mc_tab[best].calls = 0;
	}
	__real_memset(mc_tab, 0, sizeof mc_tab);
	__real_memset(mc_total, 0, sizeof mc_total);
}
#endif
static const char *const names[PP_COUNT] = { "other", "gtick", "ptick", "render", "bsp", "planes", "masked", "hud", "gs", "audio", "sleep" };

#ifdef PS2_SUBPROF
unsigned long long ps2sub_cyc[128], ps2sub_calls[128];
#endif
static boolean inited, enabled;
static ps2prof_phase_t stack[MAXNEST];
static int sp;
static ps2prof_phase_t cur = PP_OTHER;
static UINT32 last;
static UINT64 acc[PP_COUNT], calls[PP_COUNT];
static UINT32 frames, tics, windows;

static inline UINT32 count(void)
{
	UINT32 v;
	__asm__ volatile("mfc0 %0,$9" : "=r"(v));
	return v;
}

static boolean Active(void)
{
	if (!inited)
	{
		if (!myargc)
			return false; // the command line is not known yet
		inited = true;
		enabled = M_CheckParm("-ps2prof") != 0;
#ifdef PS2_PROF_DIRECT
		if (M_CheckParm("-ps2noquick") && M_IsNextParm())
			ps2_noquick = (UINT32)atoi(M_GetNextParm());
#endif
		last = count();
	}
	return enabled;
}

void PS2Prof_Enter(ps2prof_phase_t phase)
{
	UINT32 now;
	if (!Active())
		return;
	now = count();
	acc[cur] += (UINT32)(now - last);
	if (sp < MAXNEST)
		stack[sp++] = cur;
	cur = phase;
	calls[phase]++;
	last = now;
}

void PS2Prof_Leave(void)
{
	const UINT32 now = count();
	if (!enabled)
		return;
	acc[cur] += (UINT32)(now - last);
	cur = sp ? stack[--sp] : PP_OTHER;
	last = now;
}

#ifdef PS2_FPROF
// Function-level profile (build.py --fprof): every engine function is compiled with -finstrument-functions; the hooks
// below keep exclusive COP0 cycles per function (time in callees is subtracted). Reported with the window as "FP" lines:
// "FP <address> <calls> <exclusive cycles>", resolved to names by tools/ps2/fprof_report.py. The hooks cost ~60 cycles per
// call: the ranking is meaningful, tiny and very frequent functions are over-weighted.
#define FP_SLOTS 4096
#define FP_STACK 512
typedef struct { void *fn; UINT32 calls; UINT64 excl; } fpslot_t;
static fpslot_t fp_table[FP_SLOTS];
static struct { void *fn; UINT32 t0; UINT64 child; } fp_stack[FP_STACK];
static int fp_sp, fp_over;
static UINT64 fp_total;

void __cyg_profile_func_enter(void *fn, void *site) __attribute__((no_instrument_function));
void __cyg_profile_func_exit(void *fn, void *site) __attribute__((no_instrument_function));

void __cyg_profile_func_enter(void *fn, void *site)
{
	(void)site;
	if (!enabled)
		return;
	if (fp_sp >= FP_STACK)
	{
		fp_over++;
		return;
	}
	fp_stack[fp_sp].fn = fn;
	fp_stack[fp_sp].child = 0;
	fp_stack[fp_sp].t0 = count();
	fp_sp++;
}

void __cyg_profile_func_exit(void *fn, void *site)
{
	UINT32 now, h, el;
	UINT64 excl;
	(void)site;
	now = count();
	if (!enabled)
		return;
	if (fp_over)
	{
		fp_over--;
		return;
	}
	if (fp_sp <= 0)
		return;
	fp_sp--;
	el = now - fp_stack[fp_sp].t0;
	excl = (UINT64)el > fp_stack[fp_sp].child ? (UINT64)el - fp_stack[fp_sp].child : 0;
	if (fp_sp)
		fp_stack[fp_sp - 1].child += el;
	fp_total += excl;
	h = ((UINT32)(uintptr_t)fn >> 2) & (FP_SLOTS - 1);
	while (fp_table[h].fn && fp_table[h].fn != fn)
		h = (h + 1) & (FP_SLOTS - 1);
	fp_table[h].fn = fn;
	fp_table[h].calls++;
	fp_table[h].excl += excl;
}

static void FProf_Report(void)
{
	int i, n = 0, j;
	static int top[64];

	for (i = 0; i < FP_SLOTS; i++)
		if (fp_table[i].fn)
		{
			for (j = n < 64 ? n++ : 63; j > 0 && fp_table[top[j - 1]].excl < fp_table[i].excl; j--)
				top[j] = top[j - 1];
			top[j] = i;
		}
	I_OutputMsg("FPTOTAL %llu\n", (unsigned long long)fp_total);
	for (i = 0; i < n; i++)
		I_OutputMsg("FP %08x %u %llu\n", (unsigned)(uintptr_t)fp_table[top[i]].fn, (unsigned)fp_table[top[i]].calls, (unsigned long long)fp_table[top[i]].excl);
	I_OutputMsg("FPEND\n");
	memset(fp_table, 0, sizeof fp_table);
	fp_total = 0;
}
#else
static void FProf_Report(void) {}
#endif

#ifdef PS2_SAMPLE
// Statistical PC sampler (build.py --sample, run time -ps2sample): EE timer 1 interrupt every SAMP_PERIOD bus clocks,
// the INTC handler's third argument is the interrupted PC (PCSX2 raises the interrupt at the end of the running basic
// block, so a sample is the first PC of the block that followed it: per-function and per-loop ranking, not instruction
// exact). Sampling covers the profile windows 1..9; the hash table (PC -> count) is printed as "SM <pc> <count>"
// lines before the PROF line of window 9; tools/ps2/sample_report.py resolves them against the ELF.
#include <kernel.h>
#include <timer.h>
#define SAMP_SLOTS 32768
#define SAMP_PERIOD 8000
static UINT32 samp_pc[SAMP_SLOTS];
static UINT32 samp_cnt[SAMP_SLOTS];
static volatile UINT32 samp_total, samp_dropped;
static int samp_id = -1;
static boolean samp_on;

static s32 Samp_Handler(s32 cause, void *arg, void *addr)
{
	UINT32 pc = (UINT32)(uintptr_t)addr, h;
	(void)cause;
	(void)arg;
	*T1_MODE |= (1 << 10); // clear the compare flag
	samp_total++;
	h = (pc >> 2) & (SAMP_SLOTS - 1);
	for (int n = 0; n < 64; n++, h = (h + 1) & (SAMP_SLOTS - 1))
	{
		if (samp_pc[h] == pc)
		{
			samp_cnt[h]++;
			return 0;
		}
		if (!samp_pc[h])
		{
			samp_pc[h] = pc;
			samp_cnt[h] = 1;
			return 0;
		}
	}
	samp_dropped++;
	return 0;
}

static void Samp_Start(void)
{
	UINT32 period = SAMP_PERIOD;
	if (samp_on || !M_CheckParm("-ps2sample"))
		return;
	if (M_IsNextParm())
		period = (UINT32)atoi(M_GetNextParm());
	samp_id = AddIntcHandler2(INTC_TIM1, Samp_Handler, -1, NULL);
	*T1_COUNT = 0;
	*T1_COMP = period;
	*T1_MODE = (1 << 7) | (1 << 8) | (1 << 6); // bus clock, count up, compare interrupt, reset on compare
	EnableIntc(INTC_TIM1);
	samp_on = true;
}

// PS2-310: the audio decoder benchmark (i_sound.c -abench) samples only its own loop: -ps2sample, report with sample_report.py
void PS2Prof_SampleBegin(void) { Samp_Start(); }
void PS2Prof_SampleEnd(void);
static void Samp_Stop(void)
{
	int i;
	if (!samp_on)
		return;
	DisableIntc(INTC_TIM1);
	*T1_MODE = 0;
	RemoveIntcHandler(INTC_TIM1, samp_id);
	samp_on = false;
	I_OutputMsg("SMTOTAL %u dropped %u\n", (unsigned)samp_total, (unsigned)samp_dropped);
	for (i = 0; i < SAMP_SLOTS; i++)
		if (samp_pc[i])
			I_OutputMsg("SM %08x %u\n", (unsigned)samp_pc[i], (unsigned)samp_cnt[i]);
	I_OutputMsg("SMEND\n");
}
void PS2Prof_SampleEnd(void) { Samp_Stop(); }
#endif

static void Report(void)
{
	const UINT32 now = count();
	int i;
	UINT64 total = 0;
#ifdef PS2_SAMPLE
	if (windows == 9)
		Samp_Stop();
#endif
	acc[cur] += (UINT32)(now - last);
	last = now;
	// Exclusive phase intervals retain Count wraps over long windows.
	for (i = 0; i < PP_COUNT; i++) total += acc[i];
	I_OutputMsg("PROF win=%u frames=%u tics=%u total=%llu", (unsigned)windows, (unsigned)frames, (unsigned)tics, (unsigned long long)total);
	for (i = 0; i < PP_COUNT; i++)
	{
		I_OutputMsg(" %s=%llu/%llu", names[i], (unsigned long long)acc[i], (unsigned long long)calls[i]);
		acc[i] = 0;
		calls[i] = 0;
	}
	I_OutputMsg("\n");
#ifdef PS2_SUBPROF
	I_OutputMsg("SUB win=%u", (unsigned)windows);
	for (i = 0; i < 128; i++)
	{
		if (ps2sub_calls[i])
			I_OutputMsg(" s%d=%llu/%llu", i, ps2sub_cyc[i], ps2sub_calls[i]);
		ps2sub_cyc[i] = ps2sub_calls[i] = 0;
	}
	I_OutputMsg("\n");
#endif
#ifdef PS2_MEMPROF
	MC_Report(windows);
#endif
	FProf_Report();
	windows++;
#ifdef PS2_SAMPLE
	if (windows == 1)
		Samp_Start();
#endif
	frames = 0;
	tics = 0;
}

// ps2prof_sleep_cyc (the cycles asleep in I_Sleep / I_SleepDuration, PS2-HW-440) is defined in i_system.c

#ifdef PS2_PROF_DIRECT
// PS2-94: LTO profile build (no linker wrappers): I_FinishUpdate reports every displayed frame; everything is "other"
UINT32 ps2prof_interp; // frames drawn with 0 < rendertimefrac < FRACUNIT (d_main.c counts them)
UINT32 ps2prof_c_tickmax; // OPT13 IQ: the longest TryRunTics call (cycles) of the window
UINT64 ps2prof_c_tick, ps2prof_c_disp, ps2prof_c_snd; // PS2-200: cycles in TryRunTics / D_Display / S_UpdateSounds+LUA_Step (d_main.c)
UINT32 ps2prof_real; // tics the clock asked for (the sum of realtics handed to TryRunTics)
UINT64 ps2prof_cyc[16]; // OPT12-CORE: PS2_CYC_* accumulators (p_local.h)
UINT32 ps2_noquick; // set by PS2Prof_Init from -ps2noquick
UINT32 ps2prof_cnt[8]; // OPT12-CORE: PS2_CNT(n) event counters (p_local.h), printed per window as the CNT line
void PS2Prof_FrameEnd(void)
{
	static tic_t lasttic;
	static UINT32 lastframe, maxframe, over5t;
	static UINT32 bin[4]; // frame intervals in 60 Hz periods (16.67 ms): shorter than 0.5, 0.5..1.5, 1.5..2.5, longer
	static UINT64 isum, isq; // sum and sum of squares of the intervals in units of 64 cycles (the spread of the pacing: core_tick.py)
	if (Active())
	{
		const UINT32 now = count();
		const UINT32 el = now - lastframe;
		lastframe = now;
		if (frames && el > maxframe)
			maxframe = el;
		if (frames)
		{
			const UINT32 period = 294912000u / 60u;
			bin[el < period / 2 ? 0 : el < period * 3 / 2 ? 1 : el < period * 5 / 2 ? 2 : 3]++;
			over5t += el > 294912000u / 35u * 5u; // longer than five tics (the lag clamp of TryRunTics)
			isum += el >> 6;
			isq += (UINT64)(el >> 6) * (el >> 6);
		}
	}
	if (Active() && ++frames >= WINDOW_FRAMES)
	{
		tics = (UINT32)(gametic - lasttic);
		lasttic = gametic;
		I_OutputMsg("INTERP win=%u frames=%u fractional=%u\n", (unsigned)windows, (unsigned)frames, (unsigned)ps2prof_interp);
		// TICK: tic logic per window (cycles), tics run, tics the clock asked for (lost = real - run), the longest frame, frames over two tics
		I_OutputMsg("TICK win=%u frames=%u tics=%u real=%u tickcyc=%llu dispcyc=%llu sndcyc=%llu maxframe=%u over5tics=%u p0=%u p1=%u p2=%u p3=%u isum=%llu isq=%llu tickmax=%u\n", (unsigned)windows, (unsigned)frames,
			(unsigned)tics, (unsigned)ps2prof_real, (unsigned long long)ps2prof_c_tick, (unsigned long long)ps2prof_c_disp, (unsigned long long)ps2prof_c_snd,
			(unsigned)maxframe, (unsigned)over5t, (unsigned)bin[0], (unsigned)bin[1], (unsigned)bin[2], (unsigned)bin[3], (unsigned long long)isum, (unsigned long long)isq, (unsigned)ps2prof_c_tickmax);
		I_OutputMsg("CNT win=%u c0=%u c1=%u c2=%u c3=%u c4=%u c5=%u c6=%u c7=%u\n", (unsigned)windows, (unsigned)ps2prof_cnt[0], (unsigned)ps2prof_cnt[1], (unsigned)ps2prof_cnt[2], (unsigned)ps2prof_cnt[3],
			(unsigned)ps2prof_cnt[4], (unsigned)ps2prof_cnt[5], (unsigned)ps2prof_cnt[6], (unsigned)ps2prof_cnt[7]);
		memset(ps2prof_cnt, 0, sizeof ps2prof_cnt);
		I_OutputMsg("CYC win=%u c0=%llu c1=%llu c2=%llu c3=%llu c4=%llu c5=%llu c6=%llu c7=%llu c8=%llu c9=%llu c10=%llu c11=%llu c12=%llu c13=%llu c14=%llu c15=%llu\n", (unsigned)windows,
			(unsigned long long)ps2prof_cyc[0], (unsigned long long)ps2prof_cyc[1], (unsigned long long)ps2prof_cyc[2], (unsigned long long)ps2prof_cyc[3], (unsigned long long)ps2prof_cyc[4],
			(unsigned long long)ps2prof_cyc[5], (unsigned long long)ps2prof_cyc[6], (unsigned long long)ps2prof_cyc[7], (unsigned long long)ps2prof_cyc[8], (unsigned long long)ps2prof_cyc[9],
			(unsigned long long)ps2prof_cyc[10], (unsigned long long)ps2prof_cyc[11], (unsigned long long)ps2prof_cyc[12], (unsigned long long)ps2prof_cyc[13], (unsigned long long)ps2prof_cyc[14],
			(unsigned long long)ps2prof_cyc[15]);
		memset(ps2prof_cyc, 0, sizeof ps2prof_cyc);
		ps2prof_c_tick = ps2prof_c_disp = ps2prof_c_snd = 0;
		ps2prof_c_tickmax = 0;
		ps2prof_real = 0;
		maxframe = over5t = 0;
		bin[0] = bin[1] = bin[2] = bin[3] = 0;
		isum = isq = 0;
		ps2prof_interp = 0;
		Report();
	}
}
#else
#define WRAP_VOID0(ret, name, phase) \
	ret __real_##name(void); \
	ret __wrap_##name(void) { PS2Prof_Enter(phase); __real_##name(); PS2Prof_Leave(); }

void __real_G_Ticker(boolean run);
void __wrap_G_Ticker(boolean run)
{
	PS2Prof_Enter(PP_GTICK);
	__real_G_Ticker(run);
	if (enabled)
		tics++;
	PS2Prof_Leave();
}

void __real_P_Ticker(boolean run);
void __wrap_P_Ticker(boolean run)
{
	PS2Prof_Enter(PP_PTICK);
	__real_P_Ticker(run);
	PS2Prof_Leave();
}

void __real_R_RenderPlayerView(player_t *player);
void __wrap_R_RenderPlayerView(player_t *player)
{
	PS2Prof_Enter(PP_RENDER);
	__real_R_RenderPlayerView(player);
	PS2Prof_Leave();
}

void __real_R_RenderBSPNode(INT32 bspnum);
void __wrap_R_RenderBSPNode(INT32 bspnum)
{
	PS2Prof_Enter(PP_BSP);
	__real_R_RenderBSPNode(bspnum);
	PS2Prof_Leave();
}

void __real_R_DrawMasked(maskcount_t *masks, INT32 nummasks);
void __wrap_R_DrawMasked(maskcount_t *masks, INT32 nummasks)
{
	PS2Prof_Enter(PP_MASKED);
	__real_R_DrawMasked(masks, nummasks);
	PS2Prof_Leave();
}

WRAP_VOID0(void, R_DrawPlanes, PP_PLANES)
WRAP_VOID0(void, ST_Drawer, PP_HUD)
WRAP_VOID0(void, HU_Drawer, PP_HUD)
WRAP_VOID0(void, M_Drawer, PP_HUD)
WRAP_VOID0(void, CON_Drawer, PP_HUD)
WRAP_VOID0(void, I_UpdateSound, PP_AUDIO)
WRAP_VOID0(void, S_UpdateSounds, PP_AUDIO)

void __real_I_FinishUpdate(void);
void __wrap_I_FinishUpdate(void)
{
	PS2Prof_Enter(PP_GS);
	__real_I_FinishUpdate();
	PS2Prof_Leave();
	if (enabled && ++frames >= WINDOW_FRAMES)
		Report();
}

void __real_I_Sleep(UINT32 ms);
void __wrap_I_Sleep(UINT32 ms)
{
	PS2Prof_Enter(PP_SLEEP);
	__real_I_Sleep(ms);
	PS2Prof_Leave();
}

void __real_I_SleepDuration(precise_t duration);
void __wrap_I_SleepDuration(precise_t duration)
{
	PS2Prof_Enter(PP_SLEEP);
	__real_I_SleepDuration(duration);
	PS2Prof_Leave();
}

#endif
