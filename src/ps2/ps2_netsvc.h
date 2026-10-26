// SRB2 PS2 port, OPT12 NET: the receive service thread of the game socket (PS2-NET-5). See ps2_netsvc.c.
#ifndef PS2_NETSVC_H
#define PS2_NETSVC_H

#include "../doomtype.h"
#include "../netcode/i_net.h" // MAXPACKETLENGTH

typedef struct
{
	UINT32 len;
	UINT32 from_addr;      // sin_addr.s_addr, network order
	UINT16 from_port;      // sin_port, network order
	UINT16 pad;
	UINT64 t;              // GetTimerSystemTime() at which the datagram was taken off the socket
	UINT8 data[MAXPACKETLENGTH];
} nsv_packet_t;

// Starts the thread for the (blocking) UDP socket `fd`. false: no thread (the caller keeps reading the socket itself).
boolean PS2NetSvc_Start(int fd);
// Ends the thread (before the socket is closed).
void PS2NetSvc_Stop(void);
boolean PS2NetSvc_Running(void);
// bytes of the C heap the ring and the stack take while the thread exists (for the memory budget)
UINT32 PS2NetSvc_HeapUse(void);

// The oldest datagram that has not been taken yet (NULL: none); Pop() releases it.
const nsv_packet_t *PS2NetSvc_Peek(void);
void PS2NetSvc_Pop(void);

// The game thread tells the thread whether it is a joined client (acknowledgements of the tics of the server may go out at once) and that it is alive.
void PS2NetSvc_SetClient(boolean joined);
// OPT13-IO (RS-09): the client is connected to a server (in any game state): while the game thread loads and does not poll, the thread sends the keep-alive of a fade
void PS2NetSvc_SetConnected(boolean connected);
void PS2NetSvc_MainBeat(void);

// Counters (-netdebug / NETLAT)
typedef struct
{
	UINT32 received, dropped, early_acks, early_ack_errors, max_depth, early_mis, load_keepalives; // load_keepalives: OPT13-IO RS-09
} nsv_stats_t;
void PS2NetSvc_GetStats(nsv_stats_t *out);

#endif
