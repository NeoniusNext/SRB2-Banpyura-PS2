// PS2 network bring-up (PS2-120): DEV9 + SMAP + netman IRX and the EE lwIP stack (libps2ip) through libps2_drivers.
// Nothing is loaded at boot: the single-player game never pays for it. src/netcode/i_tcp.c calls PS2Net_Up() when a socket is opened.
#ifndef __PS2_NET_H__
#define __PS2_NET_H__

#include "../doomtype.h"

// Brings the stack up once (modules, link, DHCP or the static address of -ip/-netmask/-gateway). false: no adapter or no address.
boolean PS2Net_Up(void);
// Dotted local address ("" before PS2Net_Up succeeded)
const char *PS2Net_Address(void);
// Dotted gateway address (the discard-port datagram of the PCSX2 inbound-UDP workaround goes there)
const char *PS2Net_Gateway(void);
// Bytes of C heap the stack added (modules' EE side, lwIP pools), for the memory budget
UINT32 PS2Net_HeapUse(void);

// -netcmd "N:command|..." (diagnostic): console commands at displayed frame N; called once per frame
void PS2Net_Frame(void);

#endif
