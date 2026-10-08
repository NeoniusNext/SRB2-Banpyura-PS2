// PS2 network screen (PS2-330..349, OPT11 NETUI): what the player sees while ps2_net.c brings the Ethernet link and the IP stack up.
//
// The bring-up is a blocking call (up to 2 x 15 s) that is made from places that are not the main loop: a console command, a menu key, the start-up,
// the HTTP client of the master server / the add-on download. The screen is therefore a small modal loop of its own, drawn in BOTH renderers the way
// the server connection screen is (CL_ConnectToServer): the title sky and logo behind a dimmed panel, M_DrawTextBox panels, the running 16 cell bar,
// MENUCOLOR/MENUCAPS text, one frame per tic through I_UpdateNoVsync (Software: screens[0] to the GS; Hardware: the PS2HWD frame, drawn under the
// out-of-memory guard of ps2_hwfb.c like D_Display). The pad is read while it runs (Circle / Escape cancel, Cross / Enter confirm).
#ifndef __PS2_NETUI_H__
#define __PS2_NETUI_H__

#include "../doomtype.h"

typedef enum
{
	NETUI_STEP_MODULES, // the IRX modules (DEV9, NETMAN, SMAP)
	NETUI_STEP_STACK,   // the IP stack, the link mode, the IP configuration
	NETUI_STEP_LINK,    // the Ethernet link
	NETUI_STEP_ADDRESS, // the DHCP lease (or the static address)
	NETUI_NUMSTEPS
} netui_step_t;

typedef enum
{
	NETUI_FAIL_ADAPTER, // the network modules did not start (no adapter)
	NETUI_FAIL_LINK,    // no Ethernet link
	NETUI_FAIL_DHCP,    // no DHCP answer
	NETUI_FAIL_CONFIG   // the stack refused the configuration
} netui_fail_t;

// Starts the screen. false: no picture can be drawn here (dedicated server, no graphics, inside a frame / a Lua call): the caller goes on without it
// (PS2Net_Pump still waits). `dhcp`: the address comes from a DHCP server (else the static address of -ip).
boolean PS2NetUI_Begin(boolean dhcp);
// The step that runs now (the earlier ones are done); `limit_s` = its time-out in seconds (0 = none)
void PS2NetUI_Step(netui_step_t step, INT32 limit_s);
// One call per ~1..100 ms: draws one frame per tic, reads the pad, keeps the sound alive. false = the player cancelled (Circle / Escape).
boolean PS2NetUI_Pump(void);
// The step is finished: with -netslow it is held on the screen for a while (tests); otherwise it returns at once
void PS2NetUI_StepDone(void);
// Success: the "Network ready" panel with the addresses for a couple of seconds (a key skips it). mode: 1 DHCP, 0 static.
void PS2NetUI_Ready(const char *address, const char *netmask, const char *gateway, const char *dns, boolean dhcp);
// Failure: the message window with the reason and what to do. true = the player chose "Try again", false = "Back" (or the window timed out).
boolean PS2NetUI_Failed(netui_fail_t why, INT32 attempt, boolean can_retry);
// Takes the screen down (the keys held are forgotten)
void PS2NetUI_End(void);

// The player was told what happened (a failure window was shown or the player cancelled): the caller need not show a message of its own
boolean PS2NetUI_Reported(void);
// -vidshot nN: the number of the frame of the network screen that was drawn last (0 while it is not up)
INT32 PS2NetUI_Frame(void);

#endif
