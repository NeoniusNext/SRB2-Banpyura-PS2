// SRB2 PS2 port: the network screen (PS2-330..349, OPT11 NETUI). See ps2_netui.h.
//
// Drawn like the server connection screen of client_connection.c (the same background, the same dimming, the same running bar) so that it looks like the
// rest of the menus, in Software and in Hardware. The frame is made the way D_Display makes one: 2D V_* calls, then I_UpdateNoVsync (the HW driver opens
// its frame by itself on the first draw and I_FinishUpdate presents it), inside PS2HWFB_Display (the zone guard: running out of memory in a Hardware frame
// leaves the hardware renderer instead of ending the game, and this screen goes on in Software).
#include "../doomdef.h"
#include "../doomstat.h"
#include "../command.h"
#include "../console.h"
#include "../d_event.h"
#include "../d_main.h"
#include "../f_finale.h"
#include "../g_game.h"
#include "../g_input.h"
#include "../g_state.h"
#include "../i_system.h"
#include "../i_time.h"
#include "../i_video.h"
#include "../keys.h"
#include "../m_argv.h"
#include "../m_menu.h"
#include "../s_sound.h"
#include "../screen.h"
#include "../v_video.h"
#include "../z_zone.h"
#include "ps2_hwfb.h"
#include "ps2_menuhints.h"
#include "ps2_netui.h"
#include "ps2_uiicons.h"

enum { UI_PROGRESS, UI_READY, UI_FAILED };

static struct
{
	boolean on;              // the screen is up
	boolean silent;          // no picture can be drawn from here: the waiting goes on without one
	boolean dhcp;
	INT32 mode;
	INT32 step;              // the step that runs now
	boolean done[NETUI_NUMSTEPS];
	UINT32 took_ms[NETUI_NUMSTEPS];
	UINT64 t_begin, t_step;
	INT32 limit_s;           // the time-out of the running step
	tic_t lastdrawn;
	INT32 frame;             // frames drawn since Begin (-vidshot nN)
	boolean cancel, confirm; // keys seen since the flags were cleared
	boolean reported;        // the player was told (failure window, cancel)
	char addr[24], mask[24], gw[24], dns[24];
	netui_fail_t fail;
	INT32 attempt;
	boolean can_retry;
	gamestate_t gs_saved;    // static: the guard may jump out of the frame while the game state is borrowed
} ui;

static INT32 opt_slow_ms = -1;   // -netslow MS: every step stays at least this long on the screen (tests and pictures)
static INT32 opt_ready_ms = -1;  // -netuiready MS: how long "Network ready" stays (default 2500)
static INT32 opt_fail_s = -1;    // -netuifail S: how long the failure window waits for a key (default 45)

static INT32 Opt(const char *name, INT32 def)
{
	return M_CheckParm(name) && M_IsNextParm() ? atoi(M_GetNextParm()) : def;
}

static UINT32 MsSince(UINT64 t0)
{
	return (UINT32)(((I_GetPreciseTime() - t0) * 1000) / I_GetPrecisePrecision());
}

static const char *MenuCode(void)
{
	static char code[2];

	code[0] = (char)(0x80 + ((MENUCOLOR & V_CHARCOLORMASK) >> V_CHARCOLORSHIFT)); // the chat colour code of the menu colour (client_connection.c does the same by table)
	code[1] = 0;
	return code;
}

// ---- drawing -------------------------------------------------------------------------------------------------------------------------------------------

static void DrawBackground(void)
{
	const gamestate_t gs = gamestate;

	if (gs == GS_TITLESCREEN || gs == GS_WAITINGPLAYERS)
	{
		// the sky and the logo of the connection screen: F_TitleScreenDrawer draws the sky for GS_WAITINGPLAYERS whatever the title map is doing
		gamestate = GS_WAITINGPLAYERS;
		F_MenuPresTicker();
		F_TitleScreenTicker(true);
		F_TitleScreenDrawer();
		gamestate = gs;
	}
	else if (curbgcolor >= 0)
		V_DrawFill(0, 0, BASEVIDWIDTH, BASEVIDHEIGHT, curbgcolor);
	else
		F_SkyScroll(curbgname); // no name (the start-up): black
	V_DrawFadeScreen(0xFF00, 16); // as CL_DrawConnectionStatus
}

// the 16 cell bar of the connection screen (CL_DrawConnectionStatus): palstart 96 green, 32 red, 48 orange
static void DrawBar(UINT8 palstart, boolean running)
{
	const INT32 animtime = running ? (((INT32)I_GetTime() / 4) & 15) + 16 : 31;
	INT32 i;

	M_DrawTextBox(BASEVIDWIDTH/2-128-8, BASEVIDHEIGHT-16-8, 32, 1);
	for (i = 0; i < 16; ++i)
		V_DrawFill((BASEVIDWIDTH/2-128) + (i * 16), BASEVIDHEIGHT-16, 16, 8, palstart + ((animtime - i) & 15));
}

// a 7x6 tick made of 2x2 blocks
static void DrawTick(INT32 x, INT32 y, INT32 c)
{
	V_DrawFill(x,     y + 3, 2, 2, c);
	V_DrawFill(x + 2, y + 5, 2, 2, c);
	V_DrawFill(x + 4, y + 3, 2, 2, c);
	V_DrawFill(x + 6, y + 1, 2, 2, c);
	V_DrawFill(x + 8, y - 1, 2, 2, c);
}

// the eight dots of a ring in a 8x8 box; the lit one runs round with a tail
static void DrawSpinner(INT32 x, INT32 y)
{
	static const UINT8 pos[8][2] = {{0, 0}, {3, 0}, {6, 0}, {6, 3}, {6, 6}, {3, 6}, {0, 6}, {0, 3}};
	static const UINT8 tail[4] = {0, 7, 14, 21}; // palette index of the grey ramp: white ... dark
	const INT32 head = ((INT32)I_GetTime() / 2) & 7;
	INT32 i;

	for (i = 0; i < 8; i++)
	{
		const INT32 age = (head - i) & 7;
		const INT32 c = age < 4 ? (age == 0 ? V_VMAPToPaletteIndex(MENUCOLOR) : tail[age]) : 26;

		V_DrawFill(x + pos[i][0], y + pos[i][1], 2, 2, c);
	}
}

static const char *StepLabel(INT32 step, boolean done)
{
	switch (step)
	{
		// short enough for the 208 px of the panel (the duration of a finished step stands at the right edge)
		case NETUI_STEP_MODULES: return done ? M_GetText("Network modules loaded") : M_GetText("Loading network modules");
		case NETUI_STEP_STACK: return done ? M_GetText("IP stack started") : M_GetText("Starting the IP stack");
		case NETUI_STEP_LINK: return done ? M_GetText("Ethernet link is up") : M_GetText("Waiting for Ethernet link");
		default:
			if (ui.dhcp)
				return done ? M_GetText("DHCP lease received") : M_GetText("Waiting for DHCP server");
			return done ? M_GetText("Static address applied") : M_GetText("Applying static address");
	}
}

#define PANEL_X 36 // M_DrawTextBox(36, 19, 29, 15): the filled panel is x 41..279, y 24..150
#define PANEL_Y 19
#define TEXT_X 48
#define TEXT_R 272

static void DrawPanelFrame(const char *title, const char *right, INT32 titleflags)
{
	const INT32 line = V_VMAPToPaletteIndex(MENUCOLOR);

	M_DrawTextBox(PANEL_X, PANEL_Y, 29, 15);
	V_DrawString(TEXT_X, 30, titleflags|MENUCAPS, title);
	if (right)
		V_DrawRightAlignedThinString(TEXT_R, 31, V_ALLOWLOWERCASE|V_GRAYMAP, right);
	V_DrawFill(TEXT_X, 40, TEXT_R - TEXT_X, 1, line); // the header line of the level platter (m_menu.c, M_DrawLevelPlatterHeader)
	V_DrawFill(TEXT_X, 41, TEXT_R - TEXT_X, 1, 26);
}

static void DrawStepRows(void)
{
	INT32 i;

	for (i = 0; i < NETUI_NUMSTEPS; i++)
	{
		const INT32 y = 47 + i * 12;
		const boolean done = ui.done[i], current = (i == ui.step && !done && ui.mode == UI_PROGRESS);
		char detail[24];

		if (done)
		{
			DrawTick(TEXT_X + 1, y + 1, V_VMAPToPaletteIndex(V_GREENMAP));
			V_DrawString(TEXT_X + 16, y, MENUCAPS, StepLabel(i, true));
			snprintf(detail, sizeof detail, "%u.%u s", (unsigned)(ui.took_ms[i] / 1000), (unsigned)((ui.took_ms[i] % 1000) / 100));
			V_DrawRightAlignedThinString(TEXT_R, y + 1, V_ALLOWLOWERCASE|V_GRAYMAP, detail);
		}
		else if (current)
		{
			DrawSpinner(TEXT_X + 1, y);
			V_DrawString(TEXT_X + 16, y, MENUCOLOR|MENUCAPS, StepLabel(i, false)); // the elapsed time of this step is above the gauge below the list
		}
		else
		{
			V_DrawFill(TEXT_X + 3, y + 3, 2, 2, 22);
			V_DrawString(TEXT_X + 16, y, V_GRAYMAP|MENUCAPS, StepLabel(i, false));
		}
	}
}

// what to try when a step lasts: the player sees it before the time-out ends
static const char *StepTip(INT32 step)
{
	if (step == NETUI_STEP_LINK)
		return M_GetText("No link yet: is the Ethernet cable plugged in?");
	if (step == NETUI_STEP_ADDRESS && ui.dhcp)
		return M_GetText("No answer yet: is the router (DHCP server) on?");
	return NULL;
}

static void DrawProgress(void)
{
	const char *tip = StepTip(ui.step);
	const UINT32 ms = MsSince(ui.t_step);

	DrawPanelFrame(M_GetText("Network"), ui.dhcp ? "DHCP" : M_GetText("Static address"), MENUCOLOR);
	DrawStepRows();
	if (ui.limit_s > 0)
	{
		// the time-out of the step as a thin gauge
		const INT32 w = TEXT_R - TEXT_X, filled = (INT32)min((UINT64)w, (UINT64)ms * (UINT64)w / (UINT64)(ui.limit_s * 1000));

		V_DrawRightAlignedThinString(TEXT_R, 101, V_ALLOWLOWERCASE|MENUCOLOR, va("%u / %d s", (unsigned)(ms / 1000), (int)ui.limit_s));
		V_DrawFill(TEXT_X, 111, w, 3, 26);
		if (filled > 0)
			V_DrawFill(TEXT_X, 111, filled, 3, V_VMAPToPaletteIndex(MENUCOLOR));
	}
	if (tip && ms >= 3000)
		V_DrawCenteredThinString(BASEVIDWIDTH/2, 122, V_ALLOWLOWERCASE, tip);
	PS2MenuHints_Log("netscreen", PS2I_CIRCLE " Cancel"); // PS2-341: the check for hints said twice
	V_DrawCenteredString(BASEVIDWIDTH/2, BASEVIDHEIGHT-16-16, MENUCOLOR|MENUCAPS, va("%s %s", PS2I_CIRCLE, M_GetText("Cancel")));
	DrawBar(96, true);
}

static void DrawReady(void)
{
	static const char *const names[4] = {"Address", "Netmask", "Gateway", "DNS"};
	const char *const vals[4] = {ui.addr, ui.mask, ui.gw, ui.dns};
	INT32 i;

	DrawPanelFrame(M_GetText("Network ready"), ui.dhcp ? M_GetText("Obtained by DHCP") : M_GetText("Static address"), V_GREENMAP);
	DrawStepRows();
	for (i = 0; i < 4; i++)
	{
		V_DrawString(TEXT_X + 16, 98 + i * 11, MENUCOLOR|MENUCAPS, M_GetText(names[i]));
		V_DrawString(TEXT_X + 88, 98 + i * 11, 0, vals[i][0] ? vals[i] : "-");
	}
	PS2MenuHints_Log("netscreen", PS2I_CROSS " Continue");
	V_DrawCenteredString(BASEVIDWIDTH/2, BASEVIDHEIGHT-16-16, MENUCOLOR|MENUCAPS, va("%s %s", PS2I_CROSS, M_GetText("Continue")));
	DrawBar(96, false);
}

// the message window of M_StartMessage / M_DrawMessageMenu: the same box, the same centred text
static void DrawFailed(void)
{
	const char *mc = MenuCode();
	const char *reason, *hint;
	char msg[512], keys[128], why[96];
	INT32 x, y;

	switch (ui.fail)
	{
		case NETUI_FAIL_ADAPTER:
			reason = M_GetText("No network adapter");
			hint = M_GetText("The network adapter did not start.\nConnect the PS2 Ethernet adapter\n(in PCSX2: enable the Ethernet\ndevice) and start the game again.");
			break;
		case NETUI_FAIL_LINK:
			reason = M_GetText("No Ethernet link");
			hint = M_GetText("Check that the Ethernet cable is\nplugged in at both ends,\nthen try again.");
			break;
		case NETUI_FAIL_DHCP:
			reason = M_GetText("No DHCP answer");
			hint = M_GetText("The router did not give an address.\nCheck its DHCP server or start the\ngame with -ip -netmask -gateway -dns\n(see the README).");
			break;
		default:
			reason = M_GetText("Configuration error");
			hint = M_GetText("The IP stack refused the settings.\nCheck -ip, -netmask and -gateway.");
			break;
	}
	if (ui.attempt > 1)
	{
		snprintf(why, sizeof why, "%s (%d)", reason, (int)ui.attempt);
		reason = why;
	}
	if (ui.can_retry)
		snprintf(keys, sizeof keys, "%s%s %s\x80      %s%s %s\x80", mc, PS2I_CROSS, M_GetText("Try again"), mc, PS2I_CIRCLE, M_GetText("Back"));
	else
		snprintf(keys, sizeof keys, "%s%s %s\x80", mc, PS2I_CIRCLE, M_GetText("Back"));
	PS2MenuHints_Log("netscreen", keys);
	snprintf(msg, sizeof msg, "%s%s\x80\n\n\x85%s\x80\n\n%s\n\n%s", mc, M_GetText("The network is not available"), reason, hint, keys);
	x = (INT32)((BASEVIDWIDTH - V_StringWidth(msg, 0) - 32) / 2);
	y = (INT32)((BASEVIDHEIGHT - V_StringHeight(msg, V_RETURN8)) / 2);
	M_DrawTextBox(x, y - 8, 2 + V_StringWidth(msg, 0)/8, V_StringHeight(msg, V_RETURN8)/8);
	V_DrawCenteredString(BASEVIDWIDTH/2, y, V_ALLOWLOWERCASE|V_RETURN8, msg);
}

static void DrawFrame(void)
{
	ui.gs_saved = gamestate;
	PS2MenuHints_FrameBegin(); // -iconcheck: the icons of this frame against its text (ps2_menuhints.c)
	DrawBackground();
	switch (ui.mode)
	{
		case UI_READY: DrawReady(); break;
		case UI_FAILED: DrawFailed(); break;
		default: DrawProgress(); break;
	}
	PS2MenuHints_FrameEnd(ui.mode == UI_READY ? "netscreen-ready" : ui.mode == UI_FAILED ? "netscreen-failed" : "netscreen-progress");
	ui.frame++;
	I_UpdateNoVsync(); // page flip or blit buffer (-vidshot nN takes its picture in there)
}

static void DrawNow(void)
{
	if (ui.silent)
		return;
	PS2HWFB_Display(DrawFrame);
	gamestate = ui.gs_saved; // the guard may have jumped out of DrawBackground with the state borrowed
}

// ---- input and the tic clock ----------------------------------------------------------------------------------------------------------------------------

static void Poll(void)
{
	I_OsPolling();
	for (; eventtail != eventhead; eventtail = (eventtail + 1) & (MAXEVENTS - 1))
	{
		const event_t *ev = &events[eventtail];

		G_MapEventsToControls(&events[eventtail]); // the keys held stay right for the game after this screen
		if (ev->type != ev_keydown)
			continue;
		if (ev->key == KEY_ESCAPE || ev->key == KEY_JOY1 + 1) // Circle = Escape (client_connection.c CL_GameKey, PS2-134)
			ui.cancel = true;
		else if (ev->key == KEY_ENTER || ev->key == KEY_JOY1 || ev->key == KEY_SPACE) // Cross = Enter
			ui.confirm = true;
	}
}

// one step of the loop: the tic clock, the pad, one picture per tic
static void Tick(boolean force)
{
	tic_t now;

	I_UpdateTime(cv_timescale.value);
	now = I_GetTime();
	if (ui.silent)
	{
		I_Sleep(10);
		return;
	}
	if (!force && now == ui.lastdrawn)
	{
		I_Sleep(1);
		return;
	}
	ui.lastdrawn = now;
	Poll();
	DrawNow();
	S_UpdateSounds();
	S_UpdateClosedCaptions();
}

// ---- the interface -------------------------------------------------------------------------------------------------------------------------------------

boolean PS2NetUI_Begin(boolean dhcp)
{
	memset(&ui, 0, sizeof ui);
	ui.dhcp = dhcp;
	ui.on = true;
	opt_slow_ms = Opt("-netslow", 0);
	opt_ready_ms = Opt("-netuiready", 2500);
	opt_fail_s = Opt("-netuifail", 45);
	// no picture: a dedicated server has no screen, a call from inside a frame (a Lua HUD hook asking for HTTP) must not draw a second frame in the first,
	// and a call from inside a Lua call must not run the title hooks of Lua in it
	ui.silent = rendermode == render_none || dedicated || !screens[0] || Z_GuardArmed() || PS2Lua_InCall();
	ui.t_begin = ui.t_step = I_GetPreciseTime();
	ui.limit_s = 0;
	if (!ui.silent)
		Tick(true);
	return !ui.silent;
}

void PS2NetUI_Step(netui_step_t step, INT32 limit_s)
{
	INT32 i;

	for (i = 0; i < (INT32)step && i < NETUI_NUMSTEPS; i++)
		if (!ui.done[i])
		{
			ui.done[i] = true; // a step that was skipped (the modules were up already) counts as done
			ui.took_ms[i] = 0;
		}
	for (i = (INT32)step; i < NETUI_NUMSTEPS; i++)
		ui.done[i] = false; // a repeated attempt waits for these again
	ui.step = step;
	ui.limit_s = limit_s;
	ui.t_step = I_GetPreciseTime();
	ui.mode = UI_PROGRESS;
	if (M_CheckParm("-netdebug"))
		CONS_Printf("NETUI step %d limit %d s at frame %d (silent %d)\n", (int)step, (int)limit_s, (int)ui.frame, (int)ui.silent);
	Tick(true);
}

void PS2NetUI_StepDone(void)
{
	const INT32 s = ui.step;

	if (s < 0 || s >= NETUI_NUMSTEPS)
		return;
	ui.done[s] = true;
	ui.took_ms[s] = MsSince(ui.t_step);
	while (opt_slow_ms > 0 && (INT32)MsSince(ui.t_step) < opt_slow_ms)
		Tick(false);
	Tick(true);
}

boolean PS2NetUI_Pump(void)
{
	if (!ui.on)
		return true;
	Tick(false);
	return !ui.cancel;
}

void PS2NetUI_Ready(const char *address, const char *netmask, const char *gateway, const char *dns, boolean dhcp)
{
	UINT64 t0;
	INT32 i;

	if (!ui.on || ui.silent)
		return;
	for (i = 0; i < NETUI_NUMSTEPS; i++)
		ui.done[i] = true;
	ui.dhcp = dhcp;
	snprintf(ui.addr, sizeof ui.addr, "%s", address);
	snprintf(ui.mask, sizeof ui.mask, "%s", netmask);
	snprintf(ui.gw, sizeof ui.gw, "%s", gateway);
	snprintf(ui.dns, sizeof ui.dns, "%s", dns);
	ui.mode = UI_READY;
	ui.cancel = ui.confirm = false;
	t0 = I_GetPreciseTime();
	CONS_Printf("NETUI ready at frame %d\n", (int)ui.frame + 1);
	Tick(true);
	while ((INT32)MsSince(t0) < opt_ready_ms && !ui.confirm && !ui.cancel)
		Tick(false);
}

boolean PS2NetUI_Failed(netui_fail_t why, INT32 attempt, boolean can_retry)
{
	UINT64 t0;

	if (!ui.on || ui.silent)
		return false;
	ui.reported = true;
	ui.mode = UI_FAILED;
	ui.fail = why;
	ui.attempt = attempt;
	ui.can_retry = can_retry;
	ui.cancel = ui.confirm = false;
	t0 = I_GetPreciseTime();
	CONS_Printf("NETUI failed (reason %d, attempt %d) at frame %d\n", (int)why, (int)attempt, (int)ui.frame + 1);
	Tick(true);
	while ((INT32)MsSince(t0) < opt_fail_s * 1000)
	{
		Tick(false);
		if (ui.cancel)
		{
			CONS_Printf("NETUI failed: back\n");
			return false;
		}
		if (ui.confirm && can_retry)
		{
			CONS_Printf("NETUI failed: try again\n");
			return true;
		}
		ui.confirm = false;
	}
	CONS_Printf("NETUI failed: window closed by itself after %d s\n", (int)opt_fail_s);
	return false;
}

void PS2NetUI_End(void)
{
	if (ui.on && !ui.silent)
		memset(gamekeydown, 0, NUMKEYS); // as the connection screen does when it is left: nothing stays pressed for the game
	ui.on = false;
	ui.frame = 0;
}

boolean PS2NetUI_Reported(void)
{
	return ui.reported || (ui.cancel && !ui.silent);
}

INT32 PS2NetUI_Frame(void)
{
	return ui.on ? ui.frame : 0;
}
