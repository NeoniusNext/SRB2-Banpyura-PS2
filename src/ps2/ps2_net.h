// PS2 network bring-up (PS2-120): DEV9 + SMAP + netman IRX and the EE lwIP stack (libps2ip) through libps2_drivers.
// Nothing is loaded at boot: the single-player game never pays for it. src/netcode/i_tcp.c calls PS2Net_Up() when a socket is opened.
#ifndef __PS2_NET_H__
#define __PS2_NET_H__

#include "../doomtype.h"

// Brings the stack up once (modules, link, DHCP or the static address of -ip/-netmask/-gateway). false: no adapter or no address, or the player cancelled.
// PS2-330: while it waits it shows the network screen (ps2_netui.c, both renderers; Circle/Escape cancels); a failure is explained there in a message window
// ("Try again" asks for another go inside the same call), and a refused call is not repeated for a few seconds (callers that retry by themselves).
boolean PS2Net_Up(void);
// true when the last PS2Net_Up() already told the player what went wrong (the failure window) or the player cancelled: the caller need not add a message of its own
boolean PS2Net_Reported(void);
// Dotted local address ("" before PS2Net_Up succeeded)
const char *PS2Net_Address(void);
// Dotted gateway address (the discard-port datagram of the PCSX2 inbound-UDP workaround goes there)
const char *PS2Net_Gateway(void);
// Bytes of C heap the stack added (modules' EE side, lwIP pools), for the memory budget
UINT32 PS2Net_HeapUse(void);

// PS2-139: unlink() that also works where the device cannot delete (PCSX2's host: refuses: "Can't delete host:/.srb2/$$$.sav" after every join, and
// the next "connect" would end in I_Error): the file is emptied instead. 0 = gone or emptied, -1 = neither worked.
int PS2Net_Unlink(const char *path);

// -netcmd "N:command|..." (diagnostic): console commands at displayed frame N; called once per frame
void PS2Net_Frame(void);

#endif
