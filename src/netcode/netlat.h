// SRB2 PS2 port, OPT12 NET: latency probes of the game connection (PS2-NET-1). Diagnostic only: compiled in PS2 builds and in PC builds made with
// -DNETSYNC_DIAG, silent unless the engine runs with -netlat. The server measures the round trip of every game tic (made -> acknowledged by the client),
// the client measures when the tics of the server arrive, how long they wait before they are run, and how often the socket is looked at.
#ifndef __NETLAT_H__
#define __NETLAT_H__

#include "../doomtype.h"

#if defined (PS2_PROFILE) || defined (NETSYNC_DIAG)
#define NETLAT 1

// server: tic `tic` is made (SV_Maketic)
void NetLat_TicMade(tic_t tic);
// server: a client packet from `node` arrived; the client now has the tics before `acked` (old value `was`)
void NetLat_ClientPacket(INT32 node, tic_t was, tic_t acked);
// client: a PT_SERVERTICS packet brought the tics [first, end)
void NetLat_ServerTics(tic_t first, tic_t end, tic_t neededtic_before);
// client: the tic `tic` starts to run (backlog = tics received and not run yet)
void NetLat_TicRun(tic_t tic, INT32 backlog);
// both: the packet reader looked at the socket
void NetLat_Poll(void);
// PS2: a datagram that the receive thread took off the socket at `arrival` (GetTimerSystemTime) is handed to the game now
void NetLat_RxAge(UINT64 arrival);
// client: a pass of the main loop ran tics (the time since the previous such pass is the "run-gap")
void NetLat_RunPass(void);
// client: the kind of the pass that is about to run tics (0 = the clock ticked, 1 = early) and its realtics (-netlattrace)
void NetLat_Pass(INT32 type, INT32 realtics);
// server: the tic cmd of a player was not there when the tic was made (the previous one is repeated); client: a packet of tics arrived with a hole in front of it
void NetLat_CmdMissed(INT32 player);
void NetLat_TicHole(void);
// both: once per main loop pass (printing, frame time)
void NetLat_Frame(void);
// both: a packet of the game connection went out / came in (type, size)
void NetLat_Sent(INT32 type, INT32 bytes);
#else
#define NetLat_TicMade(t) ((void)0)
#define NetLat_ClientPacket(n, w, a) ((void)0)
#define NetLat_ServerTics(f, e, n) ((void)0)
#define NetLat_TicRun(t, b) ((void)0)
#define NetLat_Poll() ((void)0)
#define NetLat_RxAge(a) ((void)0)
#define NetLat_RunPass() ((void)0)
#define NetLat_CmdMissed(p) ((void)0)
#define NetLat_TicHole() ((void)0)
#define NetLat_Pass(t, r) ((void)0)
#define NetLat_Frame() ((void)0)
#define NetLat_Sent(t, b) ((void)0)
#endif

#endif
