// SONIC ROBO BLAST 2 - PS2 port
//-----------------------------------------------------------------------------
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  ps2_loadprof.c
/// \brief PS2-LOAD-1 (OPT12-LOAD): load-time profiler (EE COP0 Count), see ps2_loadprof.h

#include "../doomdef.h"
#include "../m_argv.h"
#include "../i_system.h"
#include "ps2_loadprof.h"
#include "ps2_prof.h"

#if defined(PS2) && defined(PS2_PROFILE)

boolean ps2lp_on;

static const char *const slot_ids[LP_COUNT] = {
#define X(id, name) #id,
	PS2LP_SLOTS(X)
#undef X
};
static const char *const slot_names[LP_COUNT] = {
#define X(id, name) name,
	PS2LP_SLOTS(X)
#undef X
};

static UINT64 slot_cyc[LP_COUNT];
static UINT32 slot_calls[LP_COUNT];
static UINT32 lap_last;
static boolean inited;

void PS2LP_Init(UINT32 t0) // t0: the Count value at the start of main()
{
	if (inited)
		return;
	inited = true;
	ps2lp_on = M_CheckParm("-loadprof") != 0;
	if (ps2lp_on)
		lap_last = t0;
}

void PS2LP_Add(int slot, UINT32 t0)
{
	slot_cyc[slot] += (UINT32)(PS2LP_Now() - t0);
	slot_calls[slot]++;
}

void PS2LP_Lap(int slot)
{
	const UINT32 now = PS2LP_Now();

	slot_cyc[slot] += (UINT32)(now - lap_last);
	slot_calls[slot]++;
	lap_last = now;
}

// "LP <label> <id> <cycles> <calls> |<description>" for every slot used since the last report; the slots are cleared
void PS2LP_Report(const char *label)
{
	int i;

	for (i = 0; i < LP_COUNT; i++)
		if (slot_calls[i])
		{
			I_OutputMsg("LP %s %s %llu %u |%s\n", label, slot_ids[i], (unsigned long long)slot_cyc[i], (unsigned)slot_calls[i], slot_names[i]);
			slot_cyc[i] = 0;
			slot_calls[i] = 0;
		}
	lap_last = PS2LP_Now();
}

void PS2LP_HashPrint(const char *label, const ps2lp_hash_t *hs)
{
	I_OutputMsg("LHASH %s %08x%08x\n", label, (unsigned)hs->h[0], (unsigned)hs->h[1]);
}

#ifdef PS2_SAMPLE
void PS2Prof_SampleBegin(void); // ps2_prof.c
void PS2Prof_SampleEnd(void);
#endif

// -ps2sample -lpsamp N (build.py --sample): see the table below. ev: 0 main, 1 end of D_SRB2Main, 2 level start, 3 level end, 4 R_Init start, 5 end, 6 HU_Init, 7 end of command registration
void PS2LP_SampleEvent(int ev)
{
#ifdef PS2_SAMPLE
	static int mode = -1, done;

	if (mode < 0)
	{
		mode = 0;
		if (M_CheckParm("-lpsamp") && M_IsNextParm())
			mode = atoi(M_GetNextParm());
	}
	if (done || !mode)
		return;
	// mode N samples between its two events (2N-2, 2N-1): 1 main .. end of D_SRB2Main, 2 the first P_LoadLevel, 3 R_Init, 4 HU_Init .. command registration, 5 R_PrecacheLevel, 6 P_LoadMapFromFile, 7 the old level freed, 8 P_LoadTextmap, 9 the extra files of the command line (-file), 10 TextmapCount, 11 Tokenizer_Open, 12 R_LoadTextures, 13 R_InitSprites
	if (ev == 2 * mode - 2)
		PS2Prof_SampleBegin();
	else if (ev == 2 * mode - 1)
	{
		PS2Prof_SampleEnd();
		done = 1;
	}
#else
	(void)ev;
#endif
}

#endif
