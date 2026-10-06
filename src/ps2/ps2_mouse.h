// PS2-155 (OPT9-M): USB mouse through ps2mouse.irx -> the engine's mouse events, as the SDL port posts them (ev_mouse / ev_mouse2, KEY_MOUSE1..,
// KEY_MOUSEWHEELUP/DOWN). PS2 only. The IRX is started by PS2USB_Init (ps2_usb.c); without it (no mouse, -nousb, -nomouse, no module) every call
// is a no-op and costs nothing.
#ifndef PS2_MOUSE_H
#define PS2_MOUSE_H

#include "../doomtype.h"

// The cvar callbacks (use_mouse / use_mouse2 -> I_StartupMouse / I_StartupMouse2): cheap, no IOP traffic; turning a mouse off lets go of the
// buttons it holds.
void PS2Mouse_Startup(INT32 player);

// Called from I_OsPolling once per game tic: counts the mice (one RPC per second while nothing is plugged in), reads the motion / buttons / wheel
// accumulated since the last call (one blocking RPC, ~15 000 EE cycles in PCSX2, only while a mouse is present and a use_mouse cvar is on) and posts
// the events. Also drives the -usbtest replay.
void PS2Mouse_Poll(void);

// Every button held is released (neutral key-up events). I_Quit / I_Error path (PS2Kbd_Shutdown is the keyboard twin).
void PS2Mouse_Shutdown(void);

// I_GetCursorPosition: a virtual pointer, the motion of the mouse of player 1 summed up and clamped to the screen (Lua input.getCursorPosition).
void PS2Mouse_GetCursor(INT32 *x, INT32 *y);

// For tests: the number of mice the driver reported at the last count (0 = none / no driver), and the events posted so far.
unsigned PS2Mouse_Count(void);
unsigned PS2Mouse_EventCount(void);

#endif
