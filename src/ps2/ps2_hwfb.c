// SRB2 PS2 port: running out of memory is not the end of the game. See ps2_hwfb.h. PS2-170 (OPT11-STAB).

#include "../doomdef.h"
#include "../doomstat.h"
#include "../g_state.h"
#include "../g_game.h"
#include "../i_video.h"
#include "../i_system.h"
#include "../screen.h"
#include "../f_finale.h"
#include "../r_fps.h"
#include "../m_argv.h"
#include "../z_zone.h"
#include "../console.h"
#include "../p_setup.h"
#include "../command.h"
#include "ps2_hwfb.h"
#include "ps2_mem.h"
#ifdef HWRENDER
#include "../hardware/hw_main.h"
#include "../hardware/hw_batching.h"
#include "hw/ps2_hwd.h"
#endif

#ifdef PS2_PROFILE
void HWR_ReleaseBatching(void);
#endif

#define HWFB_MAX_FALLBACKS 6   // the hardware renderer is given up for good (until the player selects it again) after this many failures
#define HWFB_SOFT_RETRIES 3    // frames in a row abandoned in the software renderer before the out-of-memory error is reported as before
#define HWFB_SOFT_WINDOW 12    // ... within this many frames

static boolean hwfb_bad[NUMMAPS + 1]; // maps that did not fit in the hardware renderer (this session)
static UINT32 hwfb_fallbacks, hwfb_returns, hwfb_softretries, hwfb_thrown;
static boolean hwfb_gaveup;
static UINT32 hwfb_softframe[HWFB_SOFT_RETRIES]; // the frame numbers of the last abandoned software frames
static UINT32 hwfb_frame;                         // displayed frames
static INT32 hwfb_test = -1, hwfb_test_period;    // -hwfbtest N[,period]: an out-of-memory fallback at the Nth hardware frame (and every `period` frames after a return)
static UINT32 hwfb_test_next;
static char hwfb_last[128];

static INT32 MapSlot(void)
{
	if (gamestate == GS_LEVEL || (gamestate == GS_TITLESCREEN && titlemapinaction))
		return (gamemap >= 1 && gamemap <= NUMMAPS) ? gamemap : 0;
	return 0;
}

boolean PS2HWFB_ForcedSoftware(void)
{
#ifdef HWRENDER
	return rendermode == render_soft && vid.glstate != VID_GL_LIBRARY_ERROR
		&& (chosenrendermode != render_none ? chosenrendermode : (rendermode_t)cv_renderer.value) == render_opengl;
#else
	return false;
#endif
}

void PS2HWFB_Init(void)
{
	if (M_CheckParm("-hwfbtest") && M_IsNextParm())
	{
		const char *p = M_GetNextParm();

		hwfb_test = atoi(p);
		while (*p && *p != ',')
			p++;
		if (*p == ',')
			hwfb_test_period = atoi(p + 1);
		hwfb_test_next = hwfb_test > 0 ? (UINT32)hwfb_test : 0;
	}
}

void PS2HWFB_Report(void)
{
	I_OutputMsg("ps2_hwfb: fallbacks %lu returns %lu soft-retries %lu thrown %lu zone-jumps %lu gaveup %d last \"%s\"\n", (unsigned long)hwfb_fallbacks,
		(unsigned long)hwfb_returns, (unsigned long)hwfb_softretries, (unsigned long)hwfb_thrown, (unsigned long)Z_GuardRecovered(), (int)hwfb_gaveup, hwfb_last);
}

#ifdef HWRENDER
// The engine renders with the GS driver and gives up: everything of the hardware renderer that holds memory goes (the driver is shut down by the ordinary
// renderer switch), the game continues in software. The state the abandoned frame left behind is reset first.
static void ForceSoftware(const char *why, boolean mark)
{
	const INT32 slot = MapSlot();

	if (rendermode != render_opengl)
		return;
	snprintf(hwfb_last, sizeof hwfb_last, "%s", why);
	CONS_Alert(CONS_WARNING, "Hardware renderer: %s. Switching to the Software renderer%s.\n", why, mark && slot ? " for this map" : "");
	I_OutputMsg("ps2_hwfb: HARDWARE -> SOFTWARE (%s) map %d frame %lu\n", why, (int)gamemap, (unsigned long)hwfb_frame);
	PS2HWD_Abort();
#ifdef PS2_PROFILE
	currently_batching = false; // HWR_StartBatching stops with an error when the last frame never reached HWR_RenderBatches
	HWR_ReleaseBatching();
#endif
	R_RestoreLevelInterpolators(); // D_Display applied them for the view; the abandoned frame never restored them
	WipeInAction = false;          // F_RunWipe was left half way
	viewwindowy = 0;               // the lower view of a split screen
	if (mark && slot)
		hwfb_bad[slot] = true;
	hwfb_fallbacks++;
	if (hwfb_fallbacks >= HWFB_MAX_FALLBACKS && !hwfb_gaveup)
	{
		hwfb_gaveup = true;
		CONS_Alert(CONS_WARNING, "The Hardware renderer failed %lu times: it stays off (Options -> Video to try it again).\n", (unsigned long)hwfb_fallbacks);
	}
	setrenderneeded = render_soft;
	SCR_SetMode(); // the renderer switch of Options -> Video: HWR_ClearAllTextures, HWR_Shutdown, the driver goes down, the software path takes the GS
	Z_FreeTag(PU_HWRPLANE);         // the plane polygons of the level (no owner; they would live until the level ends)
}
#endif

static void FrameLanded(const zguard_t *g)
{
#ifdef HWRENDER
	if (rendermode == render_opengl)
	{
		char why[128];

		if (g->size)
			snprintf(why, sizeof why, "out of memory (%lu bytes, %s)", (unsigned long)g->size, PS2Mem_TagName(g->tag));
		else
			snprintf(why, sizeof why, "%s", g->reason);
		if (!g->size)
			hwfb_thrown++;
		ForceSoftware(why, true);
		return;
	}
#endif
	{
		// software: the half drawn frame is dropped; what the cache holds goes, the free space joins again, and the frame is drawn from scratch.
		UINT32 i, recent = 0;

		R_RestoreLevelInterpolators();
		WipeInAction = false;
		viewwindowy = 0;
		hwfb_softretries++;
		for (i = 0; i < HWFB_SOFT_RETRIES; i++)
			if (hwfb_softframe[i] + HWFB_SOFT_WINDOW > hwfb_frame && hwfb_softframe[i])
				recent++;
		for (i = HWFB_SOFT_RETRIES - 1; i > 0; i--)
			hwfb_softframe[i] = hwfb_softframe[i - 1];
		hwfb_softframe[0] = hwfb_frame ? hwfb_frame : 1;
		snprintf(hwfb_last, sizeof hwfb_last, "software frame dropped: %s", g->reason);
		if (recent + 1 >= HWFB_SOFT_RETRIES || !g->size)
		{
			// it did not help: this is the real size of what the picture needs
			Z_OutOfMemoryFatal(g->size, g->tag, 64);
		}
		CONS_Alert(CONS_WARNING, "Low memory: the frame is drawn again after the caches were emptied (%lu KB free).\n", (unsigned long)(Z_EmergencyFree() >> 10));
	}
}

// D_Display under the guard
void PS2HWFB_Display(void (*display)(void))
{
	zguard_t g;

	hwfb_frame++;
#ifdef HWRENDER
	if (hwfb_test > 0 && rendermode == render_opengl && hwfb_test_next && hwfb_frame >= hwfb_test_next)
	{
		// -hwfbtest: the allocator's jump is exercised without a real shortage: a throw from inside the frame
		hwfb_test_next = hwfb_test_period > 0 ? hwfb_frame + (UINT32)hwfb_test_period : 0;
		if (Z_GUARD_TRY(&g))
		{
			display();
			Z_GuardThrow("test: simulated memory failure");
			Z_GuardPop(&g);
			return;
		}
		Z_GuardLanded(&g);
		FrameLanded(&g);
		return;
	}
#endif
	if (Z_GUARD_TRY(&g))
	{
		display();
		Z_GuardPop(&g);
		return;
	}
	Z_GuardLanded(&g);
	FrameLanded(&g);
}

// The hardware part of a level load (HWR_LoadLevel). A level that does not fit in the hardware renderer leaves it before the first frame is drawn.
void PS2HWFB_BuildLevel(void)
{
#ifdef HWRENDER
	zguard_t g;
	const INT32 slot = MapSlot();

	if (rendermode != render_opengl)
		return;
	if (slot && hwfb_bad[slot])
	{
		ForceSoftware("this map did not fit before", false);
		return;
	}
	if (Z_GUARD_TRY(&g))
	{
		HWR_LoadLevel();
		Z_GuardPop(&g);
		return;
	}
	Z_GuardLanded(&g);
	{
		char why[128];

		if (g.size)
			snprintf(why, sizeof why, "out of memory building the level (%lu bytes, %s)", (unsigned long)g.size, PS2Mem_TagName(g.tag));
		else
			snprintf(why, sizeof why, "%s", g.reason);
		ForceSoftware(why, true);
	}
#else
	// no hardware renderer in this build
#endif
}

// The end of the level load: the hardware renderer was given up for an earlier level, this one is not known to be too big: the next frame tries it again
void PS2HWFB_LevelLoaded(void)
{
#ifdef HWRENDER
	const INT32 slot = MapSlot();

	if (!PS2HWFB_ForcedSoftware() || hwfb_gaveup)
		return;
	if (slot && hwfb_bad[slot])
		return;
	hwfb_returns++;
	I_OutputMsg("ps2_hwfb: trying the hardware renderer again for map %d\n", (int)gamemap);
	setrenderneeded = render_opengl; // D_Display (under the guard) makes the switch: HWR_Switch builds the level's plane polygons
#endif
}
