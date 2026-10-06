// PS2-150 (OPT9-K): the shared USB stack of the PS2 port. usbd.irx, ps2kbd.irx and ps2mouse.irx are embedded in the ELF (libps2_drivers) and
// started here, once per run, whoever asks first: the keyboard (ps2_kbd.c), the mouse (ps2_mouse.c) and the "mass:" add-on storage
// (ps2_addons.c, bdm/usbmass_bd start on top of usbd). PS2 only.
#ifndef PS2_USB_H
#define PS2_USB_H

#include "../doomtype.h"

#define PS2USB_STACK 1u // usbd.irx is resident: USB devices are enumerated by the IOP
#define PS2USB_KBD   2u // ps2kbd.irx is resident: libkbd (PS2KbdInit) may be called, it would wait forever for the server otherwise
#define PS2USB_MOUSE 4u // ps2mouse.irx is resident: libmouse (PS2MouseInit) may be called

// Loads usbd.irx, then ps2kbd.irx and ps2mouse.irx, the first time it is called (ps2_boot.c does it right after sio2man/padman/fileXio,
// the order PLAN 4.3a asks for; "-nousb" skips that call, a later "mass:" access still loads the stack). Idempotent: every later call returns
// the same mask without touching the IOP, a failed load is not retried. Returns the PS2USB_* mask of the modules that are resident.
unsigned PS2USB_Init(void);

// The mask of the last PS2USB_Init (0 before the first call); no side effects.
unsigned PS2USB_Modules(void);

// Number of IOP module loads PS2USB_Init has done so far (usbd counts once whatever the number of callers): the test that usbd is never loaded twice.
unsigned PS2USB_LoadCount(void);

#endif
