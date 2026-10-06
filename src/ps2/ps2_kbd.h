// PS2-151 (OPT9-K): USB keyboard through ps2kbd.irx (raw make/break mode) -> the engine's key and text events, as the SDL port posts them.
// PS2 only. The IRX is started by PS2USB_Init (ps2_usb.c); without it (no -nousb, no keyboard, no module) every call is a no-op.
#ifndef PS2_KBD_H
#define PS2_KBD_H

#include "../doomtype.h"

// Called from I_OsPolling once per game tic, after the pads: reads the raw events of the driver (one non-blocking RPC when nothing happened),
// posts ev_keydown / ev_keyup (KEY_* of keys.h) and, while I_GetTextInputMode() is on, ev_text with the typed character (US layout); runs the key
// repeat; refreshes shiftdown / ctrldown / altdown / capslock (always: they are cleared when there is no keyboard).
void PS2Kbd_Poll(void);

// Every key held is released (neutral key-up events), the driver is closed. I_Quit / I_Error path (PS2Joy_Shutdown is the pad twin).
void PS2Kbd_Shutdown(void);

// For tests and the "ps2kbd" console command: events seen from the driver since the start (a keyboard has been used when > 0).
unsigned PS2Kbd_RawCount(void);

#endif
