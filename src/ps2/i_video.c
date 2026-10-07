// SONIC ROBO BLAST 2 - PS2 port
//-----------------------------------------------------------------------------
// Copyright (C) 1993-1996 by id Software, Inc.
// Copyright (C) 1998-2000 by DooM Legacy Team.
// Copyright (C) 1999-2024 by Sonic Team Junior.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------
/// \file  i_video.c
/// \brief PS2 video interface: the software renderer draws the 320x200x8 index frame, the GS only displays it.

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <debug.h>

#include "../doomdef.h"
#include "../doomstat.h"
#include "../i_system.h"
#include "../v_video.h"
#include "../m_argv.h"
#include "../s_sound.h"
#include "../g_game.h"
#include "../p_tick.h" // leveltime (software-only build; the HW build gets it through hw_main.h)
#include "../f_finale.h"
#include "../i_video.h"
#include "../console.h"
#include "../command.h"
#include "../d_main.h" // D_PostEvent
#include "../keys.h"
#include "../netcode/d_netcmd.h" // cv_showping
#include "../netcode/tic_command.h" // simulated_lag

#include "ps2_gs.h"
#include "ps2_vmodes.h"
#include "ps2_boot.h"

#ifdef HWRENDER
#include "../hardware/hw_main.h"
#include "../hardware/hw_drv.h"
#include "../hardware/hw_glob.h"
#include "../z_zone.h"
#include "hw/ps2_hwd.h"
#include "hw/ps2_hwd_dbg.h"
#include "hw/ps2_hw_prof.h"
#endif
#include "ps2_prof.h"

rendermode_t rendermode = render_none;
rendermode_t chosenrendermode = render_none;

boolean allow_fullscreen = false;

// Off by default: D_SRB2Loop skips its frame-rate sleep when vid_wait is On and fpscap is "Match refresh rate", counting
// on the buffer swap to block. Here nothing blocks (35 Hz tics, no interpolation), so On would spin the main loop.
consvar_t cv_vidwait = CVAR_INIT ("vid_wait", "Off", CV_SAVE, CV_OnOff, NULL);

UINT8 graphics_started = 0; // Is used in console.c and screen.c

// The engine runs its logic on a 35 Hz timer; the display runs on vblank. Interpolation is off: 35 means "no frame
// cap above the tic rate" to R_GetFramerateCap(), so the renderer only draws on a new tic.
#define PS2_REFRESH_RATE 35

static RGBA_t video_palette[256];
static boolean video_palette_set;

// ---- video modes (docs/VIDEO_MODES.md): the internal size (vid_mode, ps2vm_mode_size) and the GS output format (vid_output) ----
static int gsmode = PS2GS_MODE_AUTO; // the output format of the command line (-ntsc, -pal, -720p, ...)
static boolean output_forced; // the command line chose it: it beats the saved vid_output
static boolean output_pending; // vid_output changed: applied at the start of the next I_FinishUpdate
static size_t video_buffer_bytes; // size of vid.buffer
static char output_note[100]; // why the output format in vid_output is not in effect (shown in Video Options)

static void Vid_OutputChanged(void);
static void Vid_FitChanged(void);
static void Vid_FilterChanged(void);
static void VID_Command_NumModes_f(void);
static void VID_Command_ModeList_f(void);
static void VID_Command_Mode_f(void);
static void VID_Command_Outputs_f(void);
static void VID_Command_Info_f(void);

static CV_PossibleValue_t vidoutput_cons_t[PS2VM_NUMOUT + 2]; // {0, "Auto"}, {outid + 1, name}, {0, NULL}; filled by Vid_InitCvars
static CV_PossibleValue_t vidfit_cons_t[] = {{PS2VM_FIT_43, "Fit 4:3"}, {PS2VM_FIT_STRETCH, "Stretch"}, {PS2VM_FIT_SQUARE, "Square Pixels"}, {PS2VM_FIT_INTEGER, "Integer"}, {0, NULL}};

consvar_t cv_vidoutput = CVAR_INIT ("vid_output", "Auto", CV_SAVE|CV_CALL, vidoutput_cons_t, Vid_OutputChanged);
consvar_t cv_vidfit = CVAR_INIT ("vid_fit", "Fit 4:3", CV_SAVE|CV_CALL, vidfit_cons_t, Vid_FitChanged);
consvar_t cv_vidfilter = CVAR_INIT ("vid_filter", "Off", CV_SAVE|CV_CALL, CV_OnOff, Vid_FilterChanged);

static INT32 Vid_WantedOutput(void)
{
	return cv_vidoutput.value - 1; // PS2GS_MODE_AUTO when 0
}

static const char *Vid_OutputProblem(int r)
{
	switch (r)
	{
		case PS2GS_E_BIOS: return "the BIOS does not know this mode (needs ROM version 2.20)";
		case PS2GS_E_VRAM: return "the frame buffers and the picture do not fit into the 4 MB of video memory";
		case PS2GS_E_ID: return "no such mode";
		default: return "the GS did not start";
	}
}

static void Vid_OutputChanged(void)
{
	if (output_forced && graphics_started && cv_vidoutput.value != gsmode + 1)
	{
		CV_StealthSetValue(&cv_vidoutput, gsmode + 1); // -ntsc, -720p, ... win over the saved value
		return;
	}
	output_pending = true;
}

static void Vid_FitChanged(void)
{
	ps2gs_set_fit(cv_vidfit.value);
}

static void Vid_FilterChanged(void)
{
	ps2gs_set_filter(cv_vidfilter.value || M_CheckParm("-linear"));
}

static void Vid_InitCvars(void)
{
	int i;

	vidoutput_cons_t[0].value = 0;
	vidoutput_cons_t[0].strvalue = "Auto";
	for (i = 0; i < PS2VM_NUMOUT; i++)
	{
		vidoutput_cons_t[i + 1].value = i + 1;
		vidoutput_cons_t[i + 1].strvalue = ps2vm_output(i)->name;
	}
	vidoutput_cons_t[PS2VM_NUMOUT + 1].value = 0;
	vidoutput_cons_t[PS2VM_NUMOUT + 1].strvalue = NULL;
	CV_RegisterVar(&cv_vidoutput);
	CV_RegisterVar(&cv_vidfit);
	CV_RegisterVar(&cv_vidfilter);
}

#ifdef HWRENDER
// The renderer driver of the hardware path knows only the first three formats; the rest fall back to the console region.
static INT32 Impl_HWVideo(void)
{
	INT32 id = ps2gs_is_up() ? ps2gs_mode() : Vid_WantedOutput();
	return (id <= PS2GS_MODE_480P) ? id : ps2gs_region_output();
}
#endif

static void Impl_SoftwareAcquire(INT32 mode)
{
	UINT32 rgb[256];
	int i, r;

	(void)mode; // the output format comes from vid_output (the command line was copied into it)
	ps2gs_set_source(vid.width > 0 ? vid.width : BASEVIDWIDTH, vid.height > 0 ? vid.height : BASEVIDHEIGHT);
	ps2gs_set_fit(cv_vidfit.value);
	ps2gs_set_filter(cv_vidfilter.value || M_CheckParm("-linear"));
	if (!ps2gs_is_up())
	{
		r = ps2gs_init(Vid_WantedOutput());
		if (r && Vid_WantedOutput() != PS2GS_MODE_AUTO)
		{
			CONS_Alert(CONS_WARNING, "Output format %s is not available: %s\n", cv_vidoutput.string, Vid_OutputProblem(r));
			CV_StealthSetValue(&cv_vidoutput, 0);
			r = ps2gs_init(PS2GS_MODE_AUTO);
		}
		if (r)
			I_Error("PS2 software GS init failed (%d: %s)", r, Vid_OutputProblem(r));
	}
	if (video_palette_set)
	{
		for (i = 0; i < 256; i++)
			rgb[i] = video_palette[i].s.red | ((UINT32)video_palette[i].s.green << 8) | ((UINT32)video_palette[i].s.blue << 16);
		ps2gs_set_palette(rgb);
	}
}

#ifdef HWRENDER
// Optional GS hardware renderer (docs/HW_INTEGRATION.md). The GS belongs to one owner at a time: the software path
// (ps2_gs.c, an index frame + CLUT) or the hwdriver (ps2_hwd*.c). A switch shuts one down and starts the other.
static boolean hwd_on; // the GS belongs to the hardware driver
static boolean hwd_filled; // HWD has been filled in
static INT32 hwdump_frame; // -hwdump N: write frame N of the hardware renderer to host:
static INT32 hwframes;
static INT32 hwtitleframes;
static INT32 hwstats_every; // -hwstats N: memory line every N hardware frames
static INT32 hwtoggle_every; // -hwtoggle N: switch software <-> hardware every N frames (test of the runtime switch)
static INT32 hwexit_frames; // -hwexit N: quit after N frames (either renderer)
static boolean hwtextest; // -hwtextest: PS2HWD_TextureTest() on the 12th level frame, then quit
static boolean hwprof; // -ps2prof: HWPROF lines (the phase profiler of ps2_prof.c is on)
static INT32 totalframes;

static void Impl_HWFailure(const char *message)
{
	if (Z_GuardThrow(message)) // PS2-170: the frame (or the level's hardware part) is abandoned and the game goes on in software; no return from here
		return;
	I_Error("PS2 HW resource failure: %s", message);
}

static boolean Impl_HWAcquire(void)
{
	if (hwd_on)
		return true;
	if (!hwd_filled)
	{
		PS2HWD_FillDriver(&HWD);
		hwd_filled = true;
	}
	PS2HWD_SetFatalHandler(Impl_HWFailure);
	{
		ps2hwd_config_t c;

		PS2HWD_GetConfig(&c);
		c.video = Impl_HWVideo(); // the video mode the software path resolved (region, -ntsc/-pal/-480p); the driver knows only these three
		c.screen_w = vid.width;
		c.screen_h = vid.height;
		c.linear = M_CheckParm("-linear") ? 1 : 0;
		if (M_CheckParm("-hwfbh") && M_IsNextParm())
			c.fbh = atoi(M_GetNextParm()); // PS2-HW-68 (diagnostics): internal frame buffer height (200 = one pixel per engine pixel, no 200->224 stretch: pixel-exact comparison with the PC picture)
		c.tex_adapt = 1; // PS2-HW-24: the footprint cap follows the working set ...
		if (M_CheckParm("-hwtexcap") && M_IsNextParm())
		{
			c.tex_cap_blocks = (unsigned int)atoi(M_GetNextParm()); // PS2-HW-20: ... unless a fixed footprint cap of one texture (256-byte blocks, 0 = none) is given
			c.tex_adapt = 0;
		}
		PS2HWD_Configure(&c);
		{
			INT32 trace = -1, dbg = 0; // PS2-HW-22: diagnostics (frame whose draws are traced, ps2hwd_dbg_flags). PS2-HW-60: one parameter after the other: the old call evaluated both
			// M_CheckParm()s before the M_GetNextParm()s (the argument order is unspecified; they share one cursor) and lost -hwdbg when -hwtrace was absent
			if (M_CheckParm("-hwtrace") && M_IsNextParm())
				trace = atoi(M_GetNextParm());
			if (M_CheckParm("-hwdbg") && M_IsNextParm())
				dbg = atoi(M_GetNextParm());
			if (M_CheckParm("-hwhash"))
				ps2hwd_hash_on = 1; // OPT10 HG: HWHASH lines (see ps2_hw_priv.inc)
			if (M_CheckParm("-hwvu1"))
				dbg |= 0x4000000; // PS2-HW-44/45 (OPT10 HG): the VU1 program transforms, clips and packs the polygons of a batch (HWDBG_VU1); validated on PCSX2 only, off by default
			PS2HWD_SetTrace(trace, dbg);
		}
	}
	ps2gs_shutdown();
	if (!HWD.pfnInit())
	{
		vid.glstate = VID_GL_LIBRARY_ERROR;
		I_Error("PS2 GS hardware renderer failed to start; Hardware request was not rendered");
		return false;
	}
	hwd_on = true;
	PS2HWD_SetScreenSize(vid.width > 0 ? vid.width : BASEVIDWIDTH, vid.height > 0 ? vid.height : BASEVIDHEIGHT);
	vid.glstate = VID_GL_LIBRARY_LOADED;
	if (video_palette_set)
		HWD.pfnSetTexturePalette(video_palette);
	HWR_Startup();
	CONS_Printf("HWE acquired driver=ps2_hwd renderer=Hardware experimental=1 size=%dx%d\n", (int)vid.width, (int)vid.height);
#ifdef PS2_PROFILE
	if (M_CheckParm("-hwvu0bench")) // OPT10 HG: cycles per vertex of the scalar / VU0 / paired VU0 transform (PS2HWD_TestVU0) and whether the pair gives the single results
	{
		ps2hwd_vu0test_t r;
		unsigned int seed, n = 0, cs = 0, cv = 0, cp = 0, pd = 0, od = 0;

		for (seed = 1; seed <= 4; seed++)
		{
			PS2HWD_TestVU0(4096, seed, &r);
			n += r.n;
			cs += r.cyc_scalar;
			cv += r.cyc_vu0;
			cp += r.cyc_vu0p;
			pd += r.pair_diff;
			od += r.oc_diff;
		}
		CONS_Printf("HWVU0 n=%u have=%d cycles/vertex scalar=%u vu0=%u vu0_pair=%u pair_diff=%u oc_diff=%u\n", n, (int)r.have_vu0, n ? cs / n : 0, n ? cv / n : 0, n ? cp / n : 0, pd, od);
	}
#endif
	return true;
}

static void Impl_HWRelease(void)
{
	if (!hwd_on)
		return;
	// Clear downloaded handles while every engine owner is still alive.
	HWR_ClearAllTextures();
	HWR_Shutdown();
	HWD.pfnShutdown();
	hwd_on = false;
	PS2HWD_SetFatalHandler(NULL);
	CONS_Printf("HWE released Hardware texture owners before GS shutdown\n");
}
#endif

static boolean Impl_VideoSetupBuffer(INT32 w, INT32 h)
{
	// vid.buffer is the only place the engine takes screens[] from (V_Init), so the alignment is decided here:
	// 64-byte base and a w*h slice (a multiple of 64 for every mode of ps2_vmodes.h), every screens[i] is aligned for DMA.
	// A new size frees the old block first (the biggest mode is 5 * 327680 bytes: no room for both) and gives the old
	// size back when the new one cannot be had, so a refused mode leaves the engine as it was. The contents are zero.
	const size_t need = (size_t)NUMSCREENS * (size_t)w * (size_t)h;
	const size_t old = video_buffer_bytes;
	INT32 i;

	if (vid.buffer && need == old)
		return true;
	free(vid.buffer);
	vid.buffer = NULL;
	video_buffer_bytes = 0;
	for (i = 0; i < NUMSCREENS; i++)
		screens[i] = NULL;
	vid.buffer = memalign(64, need);
	if (!vid.buffer)
	{
		if (!old)
			I_Error("%s", M_GetText("Not enough memory for video buffer\n"));
		vid.buffer = memalign(64, old);
		if (!vid.buffer)
			I_Error("%s", M_GetText("Not enough memory for video buffer\n"));
		memset(vid.buffer, 0, old);
		video_buffer_bytes = old;
		return false;
	}
	memset(vid.buffer, 0, need);
	video_buffer_bytes = need;
	return true;
}

// vid_output changed (menu, console, config): switch the GS to the new output format between two frames
static void Impl_ApplyOutput(void)
{
	const INT32 want = Vid_WantedOutput();
	const INT32 resolved = (want == PS2GS_MODE_AUTO) ? ps2gs_region_output() : want;
	INT32 cur;
	int r;

	output_pending = false;
#ifdef HWRENDER
	if (hwd_on)
	{
		// the hardware driver programs the GS itself and knows only the first three formats: the choice waits for Software
		if (resolved > PS2GS_MODE_480P)
			CONS_Alert(CONS_WARNING, "%s is shown by the Software renderer only; it applies when the renderer is Software\n", cv_vidoutput.string);
		return;
	}
#endif
	if (!ps2gs_is_up() || resolved == ps2gs_mode())
		return;
	cur = ps2gs_mode();
	r = ps2gs_check(want, vid.width, vid.height);
	if (!r)
	{
		ps2gs_shutdown();
		r = ps2gs_init(want);
		if (r)
			ps2gs_init(cur); // the old output again
	}
	if (r)
	{
		// the list keeps showing the choice, the menu note says why it is not in effect; the old output goes on
		CONS_Alert(CONS_WARNING, "Output format %s is not available: %s\n", cv_vidoutput.string, Vid_OutputProblem(r));
		snprintf(output_note, sizeof output_note, "Showing %s: %s", ps2vm_output(cur)->name, Vid_OutputProblem(r));
	}
	else
		output_note[0] = '\0';
	Impl_SoftwareAcquire(want); // the GS is up: source size, fit, filter and the palette again
	if (!r)
		CONS_Printf("Output format: %s, frame buffer %dx%d\n", ps2vm_output(ps2gs_mode())->name, ps2gs_fb_width(), ps2gs_fb_height());
}

// One line for the Video Options menu about the output format that is selected (NULL when there is nothing to say)
const char *PS2Video_OutputNote(void)
{
	static char note[120];
	const INT32 want = Vid_WantedOutput();
	const INT32 resolved = (want == PS2GS_MODE_AUTO) ? ps2gs_region_output() : want;
	const ps2vm_output_t *o = ps2vm_output(resolved);
	int r;

	if (output_note[0])
		return output_note;
	if (!o)
		return NULL;
	r = ps2gs_check(resolved, vid.width, vid.height);
	if (r)
	{
		snprintf(note, sizeof note, "%s: %s", o->name, Vid_OutputProblem(r));
		return note;
	}
#ifdef HWRENDER
	if (hwd_on && resolved > PS2GS_MODE_480P)
		return "The hardware renderer shows NTSC, PAL and 480p only";
#endif
	if (o->flags & PS2VM_VGA)
		return "VGA output: needs a VGA monitor, not shown by PCSX2";
	if (resolved >= PS2VM_NTSC_FRAME && resolved != PS2VM_480P_WIDE)
		return "Not checked in the emulator";
	return NULL;
}

// Output format words of the command line: -ntsc, -pal, -480p, -ntscframe, -palframe, -240p, -288p, -480pw, -576p, -720p,
// -1080i (the "arg" column of ps2_vmodes.h), -vesa=WxH[@Hz], -output NAME. Returns an outid, or -2 when none was given.
static INT32 Impl_ParseOutputArgs(void)
{
	char word[40];
	INT32 i, w, h, hz, p;

	if ((p = M_CheckParm("-output")) != 0)
	{
		const char *name;
		if (!M_IsNextParm())
			I_Error("-output requires a format name (ntsc, pal, 480p, 720p, 1080i, ...)");
		name = M_GetNextParm();
		if (!stricmp(name, "auto"))
			return PS2GS_MODE_AUTO;
		for (i = 0; i < PS2VM_NUMOUT; i++)
			if (!stricmp(name, ps2vm_output(i)->arg) || !stricmp(name, ps2vm_output(i)->name))
				return i;
		I_Error("-output: unknown PS2 output format '%s'", name);
	}
	for (i = 0; i < myargc; i++) // -vesa=1024x768 or -vesa=1024x768@70
		if (!strncmp(myargv[i], "-vesa=", 6))
		{
			hz = 0;
			if (sscanf(myargv[i] + 6, "%dx%d@%d", &w, &h, &hz) < 2)
				I_Error("-vesa expects WxH or WxH@Hz, e.g. -vesa=800x600@60");
			for (p = PS2VM_VESA_FIRST; p < PS2VM_NUMOUT; p++)
			{
				int ow, oh, ohz;
				if (sscanf(ps2vm_output(p)->arg, "vesa%dx%d@%d", &ow, &oh, &ohz) == 3 && ow == w && oh == h && (!hz || ohz == hz))
					return p;
			}
			I_Error("-vesa=%s: no such VESA mode (640x480@60/72/75/85, 800x600@56/60/72/75/85, 1024x768@60/70/75/85, 1280x1024@60/75)", myargv[i] + 6);
		}
	for (i = 0; i < PS2VM_NUMOUT; i++)
	{
		snprintf(word, sizeof word, "-%s", ps2vm_output(i)->arg);
		if (M_CheckParm(word))
			return i;
	}
	return -2;
}

void I_StartupGraphics(void)
{
	INT32 forced;

	if (dedicated)
	{
		rendermode = render_none;
		return;
	}
	if (graphics_started)
		return;

	CV_RegisterVar(&cv_vidwait);
	Vid_InitCvars();
	COM_AddCommand("vid_nummodes", VID_Command_NumModes_f, 0);
	COM_AddCommand("vid_modelist", VID_Command_ModeList_f, 0);
	COM_AddCommand("vid_mode", VID_Command_Mode_f, 0);
	COM_AddCommand("vid_outputs", VID_Command_Outputs_f, 0);
	COM_AddCommand("vid_info", VID_Command_Info_f, 0);

#ifdef HWRENDER
	// Renderer choice as in sdl/i_video.c: the command line beats the config, software is the default
	if (M_CheckParm("-renderer"))
	{
		INT32 i = 0;
		const char *modeparm;
		if (!M_IsNextParm())
			I_Error("-renderer requires Software or Hardware");
		modeparm = M_GetNextParm();
		while (cv_renderer_t[i].strvalue)
		{
			if (!stricmp(modeparm, cv_renderer_t[i].strvalue))
			{
				chosenrendermode = cv_renderer_t[i].value;
				break;
			}
			i++;
		}
		if (!cv_renderer_t[i].strvalue)
			I_Error("Unknown PS2 renderer: %s", modeparm);
	}
	else if (M_CheckParm("-software"))
		chosenrendermode = render_soft;
	else if (M_CheckParm("-opengl")) // the name is kept; it selects the GS hardware renderer here
		chosenrendermode = render_opengl;

	if (M_CheckParm("-nogl"))
	{
		vid.glstate = VID_GL_LIBRARY_ERROR;
		if (chosenrendermode == render_opengl)
			I_Error("Hardware was requested together with -nogl");
	}

	rendermode = (chosenrendermode != render_none) ? chosenrendermode : render_soft;
	if (M_CheckParm("-hwdump") && M_IsNextParm())
		hwdump_frame = atoi(M_GetNextParm());
	if (M_CheckParm("-hwstats"))
		hwstats_every = M_IsNextParm() ? atoi(M_GetNextParm()) : 70;
	if (M_CheckParm("-hwtoggle") && M_IsNextParm())
		hwtoggle_every = atoi(M_GetNextParm());
	if (M_CheckParm("-hwexit") && M_IsNextParm())
		hwexit_frames = atoi(M_GetNextParm());
	hwtextest = M_CheckParm("-hwtextest") != 0; // PS2-HW-69: texture conformance self-test (needs -hwfbh 200)
	hwprof = M_CheckParm("-ps2prof") != 0;
#else
	// Software is the only renderer there is
	chosenrendermode = render_soft;
	rendermode = render_soft;
#endif

	forced = Impl_ParseOutputArgs();
	if (forced != -2)
	{
		gsmode = forced;
		output_forced = true;
		CV_StealthSetValue(&cv_vidoutput, gsmode + 1);
	}
	if (M_CheckParm("-fit") && M_IsNextParm())
		CV_Set(&cv_vidfit, M_GetNextParm());

	vid.modenum = 0;
	vid.width = BASEVIDWIDTH;
	vid.height = BASEVIDHEIGHT;
	vid.bpp = 1;
	vid.rowbytes = vid.width * vid.bpp;
	vid.recalc = true;
	vid.direct = NULL;
	vid.WndParent = NULL;

	Impl_VideoSetupBuffer(vid.width, vid.height);

#ifdef HWRENDER
	// Bring up exactly the selected GS owner, before SCR_Startup draws loading patches.
	if (rendermode == render_opengl)
		VID_StartupOpenGL();
	else
#endif
		Impl_SoftwareAcquire(gsmode);
#ifdef HWRENDER
	CONS_Printf("HWE startup renderer=%s chosen=%d hardware=%d\n",
		rendermode == render_opengl ? "Hardware" : "Software", (int)chosenrendermode,
		(int)hwd_on
#else
	CONS_Printf("HWE startup renderer=Software chosen=%d hardware=0\n", (int)chosenrendermode);
#endif
#ifdef HWRENDER
	);
#endif

	// as the SDL path: sets vid.*, the buffer and the drawer function pointers right away
	VID_SetMode(VID_GetModeForSize(BASEVIDWIDTH, BASEVIDHEIGHT));

	graphics_started = true;
}

void I_ShutdownGraphics(void)
{
	rendermode = render_none;

	// Owner flags also cover errors during bootstrap before graphics_started is set.
	graphics_started = false;

	// vid.buffer stays: the engine may still touch screens[] while it quits
#ifdef HWRENDER
	if (hwd_on)
	{
		Impl_HWRelease();
		return;
	}
#endif
	ps2gs_shutdown();
}

void VID_StartupOpenGL(void)
{
#ifdef HWRENDER
	if (hwd_on)
		return;
	CONS_Printf("VID_StartupOpenGL()...\n");
	vid.glstate = Impl_HWAcquire() ? VID_GL_LIBRARY_LOADED : VID_GL_LIBRARY_ERROR;
	if (vid.glstate == VID_GL_LIBRARY_ERROR)
	{
		I_Error("GS hardware bootstrap failed");
	}
#endif
}

void VID_CheckGLLoaded(rendermode_t oldrender)
{
	(void)oldrender;
#ifdef HWRENDER
	if (vid.glstate == VID_GL_LIBRARY_ERROR) // it did not work the first time either
	{
		I_Error("Hardware requested while GS hardware driver is unavailable");
	}
#endif
}

void I_SetPalette(RGBA_t *palette)
{
	UINT32 rgb[256];
	size_t i;

	if (!palette)
		I_Error("I_SetPalette: NULL palette");
	memcpy(video_palette, palette, sizeof video_palette);
	video_palette_set = true;
#ifdef HWRENDER
	if (hwd_on)
	{
		HWD.pfnSetTexturePalette(palette);
		return;
	}
#endif
	for (i = 0; i < 256; i++)
		rgb[i] = palette[i].s.red | ((UINT32)palette[i].s.green << 8) | ((UINT32)palette[i].s.blue << 16);
	ps2gs_set_palette(rgb);
}

INT32 VID_NumModes(void)
{
	return PS2VM_NUMMODES;
}

// "WxH" - the video mode menu reads the two numbers back with sscanf
const char *VID_GetModeName(INT32 modeNum)
{
	static char names[PS2VM_NUMMODES][16];
	INT32 w, h;

	if (modeNum == -1)
		modeNum = vid.modenum;
	if (!ps2vm_mode_size(modeNum, &w, &h))
		return NULL;
	snprintf(names[modeNum], sizeof names[modeNum], "%dx%d", (int)w, (int)h);
	return names[modeNum];
}

// the exact size, or -1: the callers (config, command line) warn and keep the current mode
INT32 VID_GetModeForSize(INT32 w, INT32 h)
{
	return ps2vm_mode_for_size(w, h);
}

void VID_PrepareModeList(void)
{
}

static void VID_Command_NumModes_f(void)
{
	CONS_Printf(M_GetText("%d video mode(s) available(s)\n"), (int)VID_NumModes());
}

static void VID_Command_ModeList_f(void)
{
	INT32 i, w, h;

	for (i = 0; i < PS2VM_NUMMODES; i++)
	{
		ps2vm_mode_size(i, &w, &h);
		CONS_Printf("%2d: %dx%d%s\n", (int)i, (int)w, (int)h, i == vid.modenum ? " (current)" : "");
	}
}

static void VID_Command_Mode_f(void)
{
	INT32 modenum;

	if (COM_Argc() != 2)
	{
		CONS_Printf(M_GetText("vid_mode <modenum> : set video mode, current video mode %i\n"), (int)vid.modenum);
		return;
	}
	modenum = atoi(COM_Argv(1));
	if (modenum < 0 || modenum >= VID_NumModes())
		CONS_Printf(M_GetText("Video mode not present\n"));
	else
		setmodeneeded = modenum + 1; // request vid mode change
}

static void VID_Command_Outputs_f(void)
{
	INT32 i;

	CONS_Printf("vid_output: %s (current %s)\n", cv_vidoutput.string,
		ps2gs_is_up() ? ps2vm_output(ps2gs_mode())->name : "owned by the hardware renderer");
	for (i = 0; i < PS2VM_NUMOUT; i++)
	{
		const ps2vm_output_t *o = ps2vm_output(i);
		int r = ps2gs_check(i, vid.width, vid.height);
		CONS_Printf("%2d: %-18s -%s  fb %dx%d%s%s\n", (int)i, o->name, o->arg, (int)o->fbw, (int)o->fbh,
			(o->flags & PS2VM_VGA) ? " (VGA output)" : "", r ? " UNAVAILABLE: " : "");
		if (r)
			CONS_Printf("      %s\n", Vid_OutputProblem(r));
	}
}

static void VID_Command_Info_f(void)
{
	ps2vm_crtc_t c;
	ps2gs_stats_t st;
	int x, y, w, h;
	unsigned int fb0, fb1, tex, clutaddr;

	CONS_Printf("Internal mode %d: %dx%d, %lu bytes in %d screens\n", (int)vid.modenum, (int)vid.width, (int)vid.height,
		(unsigned long)video_buffer_bytes, NUMSCREENS);
	if (!ps2gs_is_up())
	{
		CONS_Printf("The GS is owned by the hardware renderer\n");
		return;
	}
	ps2gs_get_crtc(&c);
	ps2gs_get_dest(&x, &y, &w, &h);
	ps2gs_get_layout(&fb0, &fb1, &tex, &clutaddr);
	ps2gs_get_stats(&st);
	CONS_Printf("Output %s: frame buffer %dx%d CT32 x2, DISPLAY dx=%d dy=%d magh=%d magv=%d dw=%d dh=%d\n",
		ps2vm_output(ps2gs_mode())->name, ps2gs_fb_width(), ps2gs_fb_height(), c.dx, c.dy, c.magh, c.magv, c.dw, c.dh);
	CONS_Printf("Picture %dx%d at %d,%d (fit %s, filter %s), VRAM fb %05x/%05x tex %05x clut %05x\n", w, h, x, y,
		cv_vidfit.string, cv_vidfilter.string, fb0, fb1, tex, clutaddr);
	CONS_Printf("Frames %u dropped %u flips %u present max %u cycles\n", st.frames, st.dropped, st.flips, st.present_max);
}

boolean VID_CheckRenderer(void)
{
	boolean rendererchanged = false;
#ifdef HWRENDER
	rendermode_t oldrenderer = rendermode;
#endif

	if (dedicated)
		return false;

	if (setrenderneeded)
	{
#ifdef HWRENDER
		rendermode = setrenderneeded;
		rendererchanged = ((rendermode_t)setrenderneeded != oldrenderer);

		if (rendermode == render_opengl)
		{
			VID_CheckGLLoaded(oldrenderer);
			if (vid.glstate == VID_GL_LIBRARY_NOTLOADED)
				VID_StartupOpenGL(); // takes the GS; falls back to software by itself
			if (vid.glstate == VID_GL_LIBRARY_ERROR)
				rendererchanged = false;
			else if (!hwd_on && !Impl_HWAcquire()) // back from software: the GS goes to the driver again
			{
				vid.glstate = VID_GL_LIBRARY_ERROR;
				VID_CheckGLLoaded(oldrenderer);
				rendermode = render_soft;
				CV_StealthSetValue(&cv_renderer, render_soft);
				rendererchanged = false;
			}
		}
		else
		{
			rendermode = render_soft;
			Impl_HWRelease(); // the GS goes back to the index-frame path
			Impl_SoftwareAcquire(gsmode);
			CONS_Printf("HWE acquired renderer=Software\n");
		}
		setrenderneeded = 0;
#else
		rendererchanged = ((rendermode_t)setrenderneeded != render_soft || rendermode != render_soft);
		setrenderneeded = 0; // there is no other renderer to switch to
		rendermode = render_soft;
#endif
	}

	Impl_VideoSetupBuffer(vid.width, vid.height);
	SCR_SetDrawFuncs();

#ifdef HWRENDER
	if (rendermode == render_opengl && rendererchanged)
	{
		HWR_Switch();
		V_SetPalette(0);
	}
#endif

	return rendererchanged;
}

// screens[i] are the slices of vid.buffer (the same as V_Init, which runs later in SCR_Recalc)
static void Impl_SliceScreens(INT32 w, INT32 h)
{
	INT32 i;
	for (i = 0; i < NUMSCREENS; i++)
		screens[i] = vid.buffer + (size_t)i * w * h;
}

INT32 VID_SetMode(INT32 modeNum)
{
	INT32 w, h;
	const INT32 oldw = vid.width, oldh = vid.height;

	if (!ps2vm_mode_size(modeNum, &w, &h))
	{
		CONS_Alert(CONS_WARNING, "Video mode %d does not exist\n", (int)modeNum);
		return 0;
	}
	if (w != oldw || h != oldh || !vid.buffer)
	{
		// the GS texture first (VRAM), then the memory; a refusal at either step leaves the old mode as it was
		if (ps2gs_is_up() && ps2gs_set_source(w, h) != PS2GS_OK)
		{
			CONS_Alert(CONS_WARNING, "Video mode %dx%d does not fit into the video memory with this output format\n", (int)w, (int)h);
			return 0;
		}
		if (!Impl_VideoSetupBuffer(w, h))
		{
			CONS_Alert(CONS_ERROR, "Not enough memory for video mode %dx%d\n", (int)w, (int)h);
			if (ps2gs_is_up())
				ps2gs_set_source(oldw, oldh);
			Impl_SliceScreens(oldw, oldh);
			return 0;
		}
	}
	if (!ps2gs_is_up())
		ps2gs_set_source(w, h); // the hardware renderer owns the GS: remember the size for the next software start

	vid.recalc = 1;
	vid.bpp = 1;
	vid.width = w;
	vid.height = h;
	vid.rowbytes = vid.width * vid.bpp;
	vid.modenum = modeNum;
	Impl_SliceScreens(w, h);
#ifdef HWRENDER
	if (hwd_on)
		PS2HWD_SetScreenSize(w, h);
#endif

	VID_CheckRenderer();
	return 1;
}

// PS2-95: the rate the display can show new frames at: 50 Hz for the PAL outputs, 60 Hz otherwise. R_GetFramerateCap() answers
// this for "fpscap: Match refresh rate"; above the tic rate (35) the engine interpolates between tics (r_fps.c, d_main.c).
static UINT32 Impl_OutputHz(void)
{
	const int id = ps2gs_is_up() ? ps2gs_mode() : gsmode;
	return (id == PS2VM_PAL || id == PS2VM_PAL_FRAME || id == PS2VM_PAL_288P || id == PS2VM_576P) ? 50 : 60;
}

UINT32 I_GetRefreshRate(void)
{
	static INT32 forced = -1; // -ps2refresh N: any rate, for tests (0 or absent: the policy below)
	if (forced < 0)
		forced = (M_CheckParm("-ps2refresh") && M_IsNextParm()) ? atoi(M_GetNextParm()) : 0;
	if (forced)
		return (UINT32)forced;
#ifdef HWRENDER
	// the hardware renderer does not render pixels on the EE: it can afford the display rate (software cannot, see opt5-C.md)
	if (rendermode == render_opengl)
		return Impl_OutputHz();
#endif
	return PS2_REFRESH_RATE;
}

void I_UpdateNoBlit(void)
{
}

#ifdef HWRENDER
// -hwdump N: frame N of the hardware renderer as vid.width x vid.height RGB, through the driver's ReadScreenTexture
// (the same path as the engine's screenshot), to <HOME>/hwdump-N.ppm. Then the game quits, so a test run ends by itself.
static void Impl_DumpHW(void)
{
	char path[256];
	UINT8 *rgb = malloc((size_t)vid.width * vid.height * 3);
	FILE *f;
	ps2hwd_stats_t st;
	if (!hwd_on || rendermode != render_opengl || gamestate != GS_TITLESCREEN)
		I_Error("HWE title dump requested outside Hardware title (renderer=%d state=%d)", (int)rendermode, (int)gamestate);
	if (!rgb || !PS2HWD_ReadScreenRGB(HWD_SCREENTEXTURE_GENERIC2, rgb))
		I_Error("HWE screenshot readback failed");

	snprintf(path, sizeof path, "%s/hwdump-%d.ppm", I_GetEnv("HOME") ? I_GetEnv("HOME") : ".", (int)hwtitleframes);
	f = fopen(path, "wb");
	if (!f)
		I_Error("HWE cannot open screenshot: %s", path);
	if (fprintf(f, "P6\n%d %d\n255\n", (int)vid.width, (int)vid.height) < 0
		|| fwrite(rgb, 3, (size_t)vid.width * vid.height, f) != (size_t)vid.width * vid.height
		|| fclose(f) != 0)
		I_Error("HWE screenshot write failed: %s", path);
	PS2HWD_GetStats(&st, 0);
	CONS_Printf("HWE title readback renderer=Hardware driver=ps2_hwd title=1 frame=%d total_hw=%d gs_frames=%u polys=%u uploads=%u missing=%u timeouts=%u limitations=0x%x path=%s\n",
		(int)hwtitleframes, (int)hwframes, st.frames, st.polys, st.uploads, st.tex_missing, st.timeouts, st.unsupported_mask, path);
	free(rgb);
	// Completion follows successful readback/write; the wrapper also requires poweroff exit=0.
	CONS_Printf("HWE COMPLETE title Hardware frame=%d\n", (int)hwtitleframes);
	I_Quit();
}

// HWPROF: per-frame averages over a window of 105 hardware frames (docs/GATES/g1/opt3-H.md): the engine-side phases of
// HWR_RenderPlayerView (ps2_hw_prof.h) and the driver's own counters (ps2hwd_stats_t)
static void Impl_HWProf(void)
{
	static INT32 frames, windows;
	static unsigned int hwprof_vbl0; // vblank counter at the end of the previous window (st.vblanks is a running count)
	static UINT32 last_count;
	static UINT64 wall;
	UINT32 now = ps2hwp_now();
	ps2hwd_stats_t st;
	ps2hwd_info_t info;

	wall += (UINT32)(now - last_count);
	last_count = now;
	if (++frames < 105)
		return;
	if (windows == 1)
		PS2HWD_DumpWorkingSet();
	PS2HWD_GetStats(&st, 1);
	PS2HWD_GetInfo(&info);
	CONS_Printf("HWPROF win=%d frames=%d wall=%u clear=%u bsp=%u batch=%u sprites=%u nodes=%u post=%u | drv draw=%u tex=%u wait=%u flipwait=%u vbl=%u finishmax=%u | polys=%u vin=%u vout=%u clip=%u rej=%u qw=%u state=%u passes=%u bands=%u uploads=%u upbytes=%u evict=%u clut=%u kicks=%u dmawait=%u framewait=%u dropped=%u regen=%u missing=%u skipped=%u ws=%u/%u pool=%u/%u cap=%u pred=%u capchg=%u restamp=%u decim=%u\n",
		(int)windows, (int)frames, (unsigned)(wall / frames),
		(unsigned)(ps2hwp_cyc[HWP_CLEAR] / frames), (unsigned)(ps2hwp_cyc[HWP_BSP] / frames), (unsigned)(ps2hwp_cyc[HWP_BATCH] / frames),
		(unsigned)(ps2hwp_cyc[HWP_SPRITES] / frames), (unsigned)(ps2hwp_cyc[HWP_NODES] / frames), (unsigned)(ps2hwp_cyc[HWP_POST] / frames),
		st.cyc_draw / frames, st.cyc_tex / frames, st.cyc_wait / frames, st.cyc_flipwait / frames, st.vblanks - hwprof_vbl0, st.cyc_finish,
		st.polys / frames, st.verts_in / frames, st.verts_out / frames, st.clipped / frames, st.rejected / frames, st.qwords / frames,
		st.state_writes / frames, st.passes / frames, st.bands / frames, st.uploads, st.upload_bytes, st.evictions, st.clut_uploads, st.dma_kicks / frames,
		st.dma_waits, st.frame_waits, st.dropped, st.tex_regen, st.tex_missing, st.tex_skipped, st.ws_blocks / frames, st.ws_tex / frames, info.pool_used_blocks, info.pool_blocks,
		st.cap_blocks, st.pred_ws, st.cap_changes, st.tex_restamped, st.tex_decimated);
	hwprof_vbl0 = st.vblanks;
	// PS2-HW-40 (OPT9, HG): finer spans of hardware/hw_main.c (inclusive, some nested in the phases above)
	CONS_Printf("HWPROF2 win=%d setup=%u sky=%u seg=%u plane=%u addspr=%u subsec=%u light=%u sprsort=%u sprdraw=%u nodesort=%u nodedraw=%u\n", (int)windows,
		(unsigned)(ps2hwp_cyc[HWP_SETUP] / frames), (unsigned)(ps2hwp_cyc[HWP_SKY] / frames), (unsigned)(ps2hwp_cyc[HWP_SEG] / frames),
		(unsigned)(ps2hwp_cyc[HWP_PLANE] / frames), (unsigned)(ps2hwp_cyc[HWP_ADDSPR] / frames), (unsigned)(ps2hwp_cyc[HWP_SUBSEC] / frames),
		(unsigned)(ps2hwp_cyc[HWP_LIGHT] / frames), (unsigned)(ps2hwp_cyc[HWP_SPRSORT] / frames), (unsigned)(ps2hwp_cyc[HWP_SPRDRAW] / frames),
		(unsigned)(ps2hwp_cyc[HWP_NODESORT] / frames), (unsigned)(ps2hwp_cyc[HWP_NODEDRAW] / frames));
	PS2HWD_ProfExtra((unsigned int)frames); // OPT10 HG
	memset(ps2hwp_cyc, 0, sizeof ps2hwp_cyc);
	wall = 0;
	frames = 0;
	windows++;
}

static void Impl_HWStats(void)
{
	ps2hwd_stats_t hs;

	PS2HWD_GetStats(&hs, 0); // OPT5: the presentation counters; a picture that stops moving shows as flips not growing with frames
	CONS_Printf("HW flip frame %d: gs_frames=%u vblanks=%u flips=%u dropped=%u waits dma=%u frame=%u timeouts=%u flipwait=%u forced=%u wd=%u menu=%d paused=%d\n",
		(int)hwframes, hs.frames, hs.vblanks, hs.flips, hs.dropped, hs.dma_waits, hs.frame_waits, hs.timeouts, hs.flip_waits, hs.flip_forced, hs.wd_recoveries, (int)menuactive, (int)paused);
	CONS_Printf("HW state frame %d: gamestate=%d wipe=%d leveltime=%d gametic=%d totalframes=%d titlemap=%d menu=%d\n", (int)hwframes, (int)gamestate, (int)WipeInAction, (int)leveltime, (int)gametic, (int)totalframes, (int)titlemapinaction, (int)menuactive);
	CONS_Printf("HW mem frame %d: hwrcache %luK + unlocked %luK, patch data %luK, cache %luK, level %luK; C heap in use %luK; driver textures %d\n",
		(int)hwframes, (unsigned long)(Z_TagUsage(PU_HWRCACHE) >> 10), (unsigned long)(Z_TagUsage(PU_HWRCACHE_UNLOCKED) >> 10),
		(unsigned long)(Z_TagUsage(PU_PATCH_DATA) >> 10), (unsigned long)(Z_TagUsage(PU_CACHE) >> 10),
		(unsigned long)(Z_TagUsage(PU_LEVEL) >> 10), (unsigned long)((size_t)mallinfo().uordblks >> 10), (int)HWD.pfnGetTextureUsed());
}

// OglSdlFinishUpdate copies the frame into a screen texture and draws it again into the new back buffer, so that code which
// relies on "the previous screen" keeps working after the buffer swap. The GS driver does that itself, with one local
// copy inside VRAM that is skipped when the engine starts the frame with a colour clear (PS2-HW-04).
static void Impl_VidKeys(void);
static void Impl_VidShot(void);

static void Impl_FinishUpdateHW(void)
{
	if (!hwd_on || ps2gs_is_up())
		I_Error("PS2 HW frame has invalid GS ownership");
	// PS2-HW-15: no per-frame GENERIC2 copy (1280 GS blocks and a full-frame transfer): the driver reads the last completed frame
	// for an uncaptured GENERIC2 (screenshots, -hwdump), which is the same picture.
	HWD.pfnFinishUpdate(cv_vidwait.value);
	HWD.pfnGClipRect(0, 0, vid.width, vid.height, NZCLIP_PLANE);
	hwframes++;
	if (hwprof)
		Impl_HWProf();
	if (gamestate == GS_TITLESCREEN && !WipeInAction)
		hwtitleframes++;
	if (hwstats_every && hwframes % hwstats_every == 0)
		Impl_HWStats();
	if (hwdump_frame && gamestate == GS_TITLESCREEN && !WipeInAction && hwtitleframes == hwdump_frame)
		Impl_DumpHW();
	if (hwtextest && gamestate == GS_LEVEL && !WipeInAction && leveltime >= 12)
	{
		PS2HWD_TextureTest();
		I_Quit();
	}
	Impl_VidKeys(); // PS2-HW-17: -vidkeys / -vidshot also drive and photograph the hardware renderer (the frame just presented)
	Impl_VidShot();
}
#endif

void PS2Video_FatalError(const char *message)
{
	// I_Error already shut down the previous GS owner. Do not turn this into a game frame.
	printf("HWE FATAL: %s\n", message);
	fflush(stdout);
	if (!ps2boot.host)
	{
		init_scr();
		scr_printf("SRB2 PS2 fatal error\n\n%s\n\nPress Start to exit.\n", message);
	}
}

// -vidkeys 120:enter,130:down,...: post those key presses (frame number of I_FinishUpdate : key) to drive the menus of a run
// without a pad - the Video Options screens are tested this way. Keys: enter esc up down left right bs space f10 f11 tab
// lctrl lshift a s d w y n. A leading '+' / '-' posts only the key down / key up (held keys: 400:+up,900:-up).
static void Impl_VidKeys(void)
{
	static boolean parsed;
	static char spec[2048];
	static INT32 framen;
	const char *p;

	if (!parsed)
	{
		parsed = true;
		if (M_CheckParm("-vidkeys") && M_IsNextParm())
		{
			const char *arg = M_GetNextParm();

			if (!strncmp(arg, "file:", 5)) // -vidkeys file:NAME: the spec is read from <HOME>/NAME ('@' would be taken as a response file by M_FindResponseFile)
			{
				char path[256];
				FILE *kf;
				size_t got = 0;

				snprintf(path, sizeof path, "%s/%s", I_GetEnv("HOME") ? I_GetEnv("HOME") : ".", arg + 5);
				kf = fopen(path, "rb");
				if (!kf)
					I_Error("-vidkeys: cannot open %s", path);
				got = fread(spec, 1, sizeof spec - 1, kf);
				fclose(kf);
				spec[got] = '\0';
				while (got && (spec[got - 1] == '\n' || spec[got - 1] == '\r'))
					spec[--got] = '\0';
			}
			else
				strlcpy(spec, arg, sizeof spec);
		}
	}
	if (!spec[0])
		return;
	framen++;
	for (p = spec; *p;)
	{
		INT32 n = 0, key = 0;
		char name[12];
		const char *kn = name;
		size_t len = 0;
		boolean down = true, up = true;
		event_t ev;

		while (*p >= '0' && *p <= '9')
			n = n * 10 + (*p++ - '0');
		if (*p == ':')
			p++;
		while (*p && *p != ',' && len < sizeof name - 1)
			name[len++] = *p++;
		name[len] = '\0';
		while (*p && *p != ',')
			p++;
		if (*p == ',')
			p++;
		if (n != framen)
			continue;
		if (*kn == '+')
			up = false, kn++;
		else if (*kn == '-')
			down = false, kn++;
		if (!strcmp(kn, "enter")) key = KEY_ENTER;
		else if (!strcmp(kn, "esc")) key = KEY_ESCAPE;
		else if (!strcmp(kn, "up")) key = KEY_UPARROW;
		else if (!strcmp(kn, "down")) key = KEY_DOWNARROW;
		else if (!strcmp(kn, "left")) key = KEY_LEFTARROW;
		else if (!strcmp(kn, "right")) key = KEY_RIGHTARROW;
		else if (!strcmp(kn, "bs")) key = KEY_BACKSPACE;
		else if (!strcmp(kn, "space")) key = KEY_SPACE;
		else if (!strcmp(kn, "tab")) key = KEY_TAB;
		else if (!strcmp(kn, "lctrl")) key = KEY_LCTRL;
		else if (!strcmp(kn, "lshift")) key = KEY_LSHIFT;
		else if (!strcmp(kn, "f10")) key = KEY_F10;
		else if (!strcmp(kn, "f11")) key = KEY_F11;
		else if (!strcmp(kn, "console")) key = '`'; // PS2-HW-60: the console key
		else if (!strcmp(kn, "f1")) key = KEY_F1;
		else if (!strcmp(kn, "f2")) key = KEY_F2;
		else if (kn[0] >= 'a' && kn[0] <= 'z' && !kn[1]) key = kn[0];
		else
			I_Error("-vidkeys: unknown key '%s'", kn);
		memset(&ev, 0, sizeof ev);
		ev.key = key;
		if (down)
		{
			ev.type = ev_keydown;
			D_PostEvent(&ev);
		}
		if (up)
		{
			ev.type = ev_keyup;
			D_PostEvent(&ev);
		}
	}
}

// -vidshot t35,l70,f200: write the picture of the 35th title frame, the 70th level frame and the 200th frame of any kind to
// <HOME>/vidshot-<W>x<H>-<tag>.ppm (RGB through the palette the engine set) and quit after the last one. For looking at a
// video mode with your own eyes and for tests; costs nothing when the parameter is absent.
void SplitScreen_OnChange(void);
static void Command_HFSplit_f(void) // OPT10-HF: 'hf_split 1' = local splitscreen with a second player in a single player game (splitscreen viewports under -vidshot k20=hf_split~1,k300)
{
	splitscreen = COM_Argc() > 1 && atoi(COM_Argv(1)) != 0;
	SplitScreen_OnChange();
}

static void Impl_VidShot(void)
{
	static boolean parsed;
	static char spec[1536];
	static INT32 titlen, leveln, anyn, wipen, intern, left, done;
	static INT32 knext;
	static boolean klow = true;
	INT32 kord = 0;
	const char *p;

	if (!parsed)
	{
		parsed = true;
		if (M_CheckParm("-vidshot") && M_IsNextParm())
		{
			COM_AddCommand("hf_split", Command_HFSplit_f, 0);
			strlcpy(spec, M_GetNextParm(), sizeof spec);
			for (p = spec; *p;) // one shot per item 't35' / 'l70' / 'f200' (an optional '=command' follows the number)
			{
				left += (*p == 't' || *p == 'l' || *p == 'f' || *p == 'k' || *p == 'K' || *p == 'w' || *p == 'i');
				while (*p && *p != ',')
					p++;
				if (*p == ',')
					p++;
			}
		}
	}
	if (!left)
		return;
	anyn++;
	if (anyn == 3 && M_CheckParm("-vidcmd") && M_IsNextParm()) // OPT10-HF: -vidcmd 'con_hudlines~0;gr_filtermode~1': console commands ('~' = space, ';' = next command) on the third frame
	{
		char cmdline[160];
		size_t ci;

		strlcpy(cmdline, M_GetNextParm(), sizeof cmdline - 1);
		for (ci = 0; cmdline[ci]; ci++)
			cmdline[ci] = cmdline[ci] == '~' ? ' ' : cmdline[ci] == ';' ? '\n' : cmdline[ci];
		cmdline[ci++] = '\n';
		cmdline[ci] = '\0';
		COM_BufAddText(cmdline);
	}
	if (gamestate == GS_LEVEL && leveltime < 20)
		klow = true;
	if (WipeInAction)
		wipen++; // w5 = the 5th frame drawn while a screen wipe runs (PS2-HW-60)
	else if (gamestate == GS_TITLESCREEN)
		titlen++;
	else if (gamestate == GS_LEVEL)
		leveln++;
	else if (gamestate == GS_INTERMISSION)
		intern++; // OPT10-HF: i30 = the 30th intermission frame
	for (p = spec; *p;)
	{
		const char kind = *p++;
		INT32 n = 0, hit;
		char tag[24];
		char cmd[64];
		size_t cl = 0;

		while (*p >= '0' && *p <= '9')
			n = n * 10 + (*p++ - '0');
		cmd[0] = '\0';
		if (*p == '=') // PS2-HW-23: l100=map~2 -> console command ('~' = space) after the shot (several maps in one run)
		{
			for (p++; *p && *p != ',' && cl < sizeof cmd - 2; p++)
				cmd[cl++] = *p == '~' ? ' ' : *p;
			cmd[cl++] = '\n';
			cmd[cl] = '\0';
		}
		while (*p && *p != ',')
			p++;
		if (*p == ',')
			p++;
		hit = (kind == 'w' && WipeInAction && n == wipen)
			|| (!WipeInAction && ((kind == 't' && n == titlen) || (kind == 'l' && n == leveln) || (kind == 'f' && n == anyn) || (kind == 'i' && n == intern)))
			|| (!WipeInAction && (kind == 'k' || kind == 'K') && kord++ == knext && gamestate == GS_LEVEL && (INT32)leveltime >= n && (klow || kind == 'k')); // PS2-HW-60: k300 = first frame with leveltime >= 300, K300 = the same but only in a level that started after the previous shot; k/K items fire in order (the same tic as the PC reference at any frame rate)
		if (hit && (kind == 'k' || kind == 'K'))
		{
			knext++;
			klow = false; // the next K shot waits for the next level (leveltime restarts)
		}
		if (!hit)
			continue;
		snprintf(tag, sizeof tag, "%c%d", kind, (int)n);
		if (kind == 'k' || kind == 'K')
			snprintf(tag, sizeof tag, "k%d_%d", (int)n, (int)knext - 1); // the order of the shot: the same tic of several maps
		{
			char path[256];
			UINT8 *row = malloc((size_t)vid.width * 3);
			UINT8 *hwrgb = NULL;
			FILE *f;
			INT32 x, y;

#ifdef HWRENDER
			if (hwd_on)
			{
				hwrgb = malloc((size_t)vid.width * vid.height * 3);
				if (!hwrgb || !PS2HWD_ReadScreenRGB(HWD_SCREENTEXTURE_GENERIC2, hwrgb))
					I_Error("vidshot: hardware read-back failed");
			}
#endif

			snprintf(path, sizeof path, "%s/vidshot-%dx%d-%s.ppm", I_GetEnv("HOME") ? I_GetEnv("HOME") : ".", (int)vid.width, (int)vid.height, tag);
			f = fopen(path, "wb");
			if (!f || !row)
				I_Error("vidshot: cannot write %s", path);
			fprintf(f, "P6\n%d %d\n255\n", (int)vid.width, (int)vid.height);
			for (y = 0; y < vid.height; y++)
			{
				if (hwrgb)
				{
					fwrite(hwrgb + (size_t)y * vid.width * 3, 3, (size_t)vid.width, f);
					continue;
				}
				for (x = 0; x < vid.width; x++)
				{
					const RGBA_t c = video_palette[screens[0][(size_t)y * vid.rowbytes + x]];
					row[x * 3] = c.s.red;
					row[x * 3 + 1] = c.s.green;
					row[x * 3 + 2] = c.s.blue;
				}
				fwrite(row, 3, (size_t)vid.width, f);
			}
			if (fclose(f) != 0)
				I_Error("vidshot: write failed %s", path);
			free(row);
			free(hwrgb);
			CONS_Printf("VIDSHOT %s %dx%d output=%s fit=%s gamestate=%d saved %s\n", tag, (int)vid.width, (int)vid.height,
				ps2gs_is_up() ? ps2vm_output(ps2gs_mode())->name : "-", cv_vidfit.string ? cv_vidfit.string : "?", (int)gamestate, path);
			if (cmd[0])
				COM_BufAddText(cmd);
			if (++done >= left)
			{
				CONS_Printf("VIDSHOT COMPLETE %d\n", (int)done);
				I_Quit();
			}
		}
	}
}

void I_FinishUpdate(void)
{
	static boolean updating;
	if (rendermode == render_none || !graphics_started)
		return;
	if (updating)
		return; // console diagnostics inside GS operations must not recursively submit a frame
	updating = true;
#ifdef PS2_PROF_DIRECT
	PS2Prof_FrameEnd(); // PS2-94: LTO profile build, see ps2_prof.c
#endif

	if (output_pending)
		Impl_ApplyOutput();

#ifdef HWRENDER
	totalframes++;
	if (hwexit_frames && totalframes >= hwexit_frames)
		I_Quit();
	if (hwtoggle_every && totalframes % hwtoggle_every == 0) // the renderer cvar does the switch in the next D_Display
		COM_BufAddText(rendermode == render_opengl ? "renderer Software\n" : "renderer Hardware\n");
#endif

	SCR_CalculateFPS();

	if (marathonmode)
		SCR_DisplayMarathonInfo();

	// draw captions if enabled
	if (cv_closedcaptioning.value)
		SCR_ClosedCaptions();

	if (cv_ticrate.value)
		SCR_DisplayTicRate();

	if (cv_showping.value && (
		(netgame && consoleplayer != serverplayer)
		|| (simulated_lag != 0 && consoleplayer == serverplayer && Playing())
	))
		SCR_DisplayLocalPing();

#ifdef HWRENDER
	if (rendermode == render_opengl)
	{
		Impl_FinishUpdateHW();
		updating = false;
		return;
	}
#endif

	if (screens[0])
	{
		static UINT32 samples, minimum=UINT32_MAX, maximum;
		static UINT64 sum;
		UINT32 begin, end;
		static INT32 profile_arg = -1; // the command line does not change: one scan, not one per frame
		boolean profile;
		if (profile_arg < 0)
			profile_arg = M_CheckParm("-g1profile") != 0;
		profile = profile_arg != 0;
		if (profile)
			__asm__ volatile("mfc0 %0, $9" : "=r"(begin));
		Impl_VidKeys();
		Impl_VidShot();
		ps2gs_present(screens[0], cv_vidwait.value);
		if (profile)
		{
			__asm__ volatile("mfc0 %0, $9" : "=r"(end));
			end -= begin;
			if (end < minimum) minimum = end;
			if (end > maximum) maximum = end;
			sum += end;
			if (++samples % 350 == 0)
				CONS_Printf("G1 gs_submit samples=%u cop0_min=%u max=%u mean=%u\n",
					samples, minimum, maximum, (UINT32)(sum / samples));
		}
	}
	updating = false;
}

void I_UpdateNoVsync(void)
{
	INT32 real_vidwait = cv_vidwait.value;
	cv_vidwait.value = 0;
	I_FinishUpdate();
	cv_vidwait.value = real_vidwait;
}

void I_ReadScreen(UINT8 *scr)
{
	if (rendermode != render_soft)
		I_Error("I_ReadScreen: called while in non-software mode");
	else
		VID_BlitLinearScreen(screens[0], scr,
			vid.width*vid.bpp, vid.height,
			vid.rowbytes, vid.rowbytes);
}

// I_WaitVBL, I_BeginRead and I_EndRead live in i_system.c
