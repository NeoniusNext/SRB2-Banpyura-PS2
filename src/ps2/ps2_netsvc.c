// SRB2 PS2 port, OPT12 NET: the receive service thread of the game socket (PS2-NET-5).
//
// Before: the game thread read the socket itself, once per tic and only while it was not busy with a frame (select() + recvfrom() at the top of the tic). lwIP keeps a handful of
// datagrams per socket (the receive buffer cannot be enlarged: SO_RCVBUF is not supported), so a server that sends a burst (16 file fragments per tic, the join) lost most of it
// while the game thread was drawing, and a tic of the server waited for the next pass of the loop before the game even saw it, let alone acknowledged it.
//
// Now: a thread of its own (priority 5: above lwIP's threads at 6 and the game thread at 8) sits in a blocking recvfrom() on the socket (no time-out: nothing of the SDK's alarm
// library is involved, see PS2_SleepUs), takes every datagram at once, stamps it with the time of arrival and queues it in a ring of NSV_SLOTS packets. The game thread's SOCK_Get
// takes the packets out of the ring, in order, exactly as it took them from the socket. Nothing of the game's state is touched by the thread, with one exception:
//
//   Why above lwIP (found with the add-on download: ZT.pk3, 2 MB, 12.1 s at priority 6, 3.9 s at 5 and at 4): at lwIP's own priority the thread was not woken until the tcpip
//   thread had finished the burst it was processing (a server sends 16 fragments of 1 KB in one go), and the UDP mailbox of an lwIP socket holds a handful of datagrams: the rest
//   of the burst was dropped (6415 fragments sent for 2023 needed, 68 % lost). Above it, every datagram is taken out of the mailbox as it arrives.
//
//   (-netsvcprio N sets another priority for a test.)
//
//   Early acknowledgement. A joined client answers every PT_SERVERTICS packet that brings the tics it was waiting for with a PT_NODEKEEPALIVE packet whose resendfrom is the new first tic
//   it needs (what the next PT_CLIENTCMD would say, 10 bytes, no ticcmd): the server learns at once that the tics have arrived, instead of after the client's next tic and next frame
//   (PT_NODEKEEPALIVE is part of the original protocol: the server only moves the node's tic and keeps its time-out alive). The thread does this only while the game thread polls
//   (it was in SOCK_Get within the last 150 ms): during a level load the game thread acknowledges nothing, and neither does the thread, so no tic can be acknowledged and then lost
//   from the ring.
#include <kernel.h>
#include <timer.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "../doomdef.h"
#include "../doomstat.h"
#include "../m_argv.h"
#include "../netcode/d_clisrv.h"
#include "../netcode/protocol.h"
#include "ps2_netsvc.h"
#include "ps2_sys.h"

#define NSV_SLOTS 64 // a power of two; 1.4 KiB each (the heap, not the bss): 1.8 s of tics, or 90 KiB of file fragments
#define NSV_PRIO 5 // above lwIP's threads (6)
#define NSV_STACK 16384
#define NSV_MAIN_ALIVE_MS 150
#define NSV_LOAD_MS 500      // OPT13-IO (RS-09): the game thread has not looked at the socket for this long: it is loading a level or an add-on
#define NSV_KEEPALIVE_MS 1000 // and then the thread tells the server once a second that the client is still there

static struct
{
	nsv_packet_t *ring;
	volatile UINT32 head, tail;
	volatile boolean stop, in_recv, joined;
	volatile boolean connected; // a client in CL_CONNECTED in any game state (the intermission before a level load too): the keep-alive of a load may go out
	volatile UINT64 main_beat;
	int fd, tid;
	nsv_stats_t st;
	tic_t rx_needed; // the first tic the client still needs, as far as the thread knows (never behind the game thread's neededtic)
	UINT64 ka_t;     // when the last keep-alive of the load went out
	boolean ka_off;  // -netnokeepalive: the A/B switch of the test
} nsv = {NULL, 0, 0, false, false, false, false, 0, -1, -1, {0, 0, 0, 0, 0, 0, 0}, 0, 0, false};

static nsv_packet_t scratch; // where a datagram goes when the ring is full (it is dropped, as lwIP would have)
static UINT8 *nsv_stack; // from the heap while a game socket is open (not 16 KiB of bss for the single-player game)
extern void *_gp;

#define BARRIER() __asm__ volatile("sync" ::: "memory")

// the checksum of the original protocol (d_net.c NetbufferChecksum): bytes 4.. weighted by their position
static UINT32 Checksum(const UINT8 *pkt, size_t len)
{
	UINT32 c = 0x1234567;
	size_t i;

	for (i = 4; i < len; i++)
		c += pkt[i] * (UINT32)(i - 3);
	return c;
}

static void EarlyAck(const nsv_packet_t *p, const struct sockaddr_in *from)
{
	const doomdata_t *d = (const doomdata_t *)p->data;
	doomdata_t a;
	tic_t start, end, need, cap;
	size_t len;
	boolean missing = false;

	if (!nsv.joined || p->len < BASEPACKETSIZE + 6 || d->packettype != PT_SERVERTICS)
		return;
	if (nsv.main_beat == 0 || (INT64)(p->t - nsv.main_beat) > (INT64)NSV_MAIN_ALIVE_MS * 147456)
		return; // the game thread is not looking at the network (a level load): it acknowledges when it is back
	if (Checksum(p->data, p->len) != d->checksum)
		return;
	start = d->u.serverpak.starttic;
	end = start + d->u.serverpak.numtics;
	cap = gametic + CLIENTBACKUPTICS; // PT_ServerTics: realend = min(realend, gametic + CLIENTBACKUPTICS)
	if (end > cap)
		end = cap;
	need = neededtic; // the game thread's own count can be ahead of ours
	if (need > nsv.rx_needed)
		nsv.rx_needed = need;
	if (start <= nsv.rx_needed && end > nsv.rx_needed)
		nsv.rx_needed = end;
	else if (start > nsv.rx_needed)
		missing = true; // a hole: the packet(s) in front of this one were lost; ask for them at once (PT_NODEKEEPALIVEMIS = "resend from resendfrom", as PT_CLIENTMIS does)
	else
		return; // tics that are known already: nothing new to say
	memset(&a, 0, BASEPACKETSIZE + 2);
	a.packettype = missing ? PT_NODEKEEPALIVEMIS : PT_NODEKEEPALIVE;
	a.u.clientpak.client_tic = (UINT8)(gametic & UINT8_MAX);
	a.u.clientpak.resendfrom = (UINT8)(nsv.rx_needed & UINT8_MAX);
	len = BASEPACKETSIZE + 2;
	a.checksum = Checksum((const UINT8 *)&a, len);
	if (sendto(nsv.fd, &a, len, 0, (const struct sockaddr *)from, sizeof *from) < 0)
		nsv.st.early_ack_errors++;
	else if (missing)
		nsv.st.early_mis++;
	else
		nsv.st.early_acks++;
}

// OPT13-IO (RS-09): the game thread loads a level (3..9 s from a disc or a stick) or an add-on and does not poll the network; the server drops a node that is silent for
// cv_nettimeout (350 tics = 10 s) and the early acknowledgements above are off during a load. Here the thread answers a tic packet of the server with the packet the
// original client sends during a fade (CL_SendClientKeepAlive: PT_BASICKEEPALIVE, a bare header, the server only moves the time-out of the node), at most once a second, and
// only while the game thread is not polling and the client is joined. No tic is acknowledged: the protocol is not touched, and the packets are the ones the original sends.
static void LoadKeepAlive(const nsv_packet_t *p, const struct sockaddr_in *from)
{
	const doomdata_t *d = (const doomdata_t *)p->data;
	doomdata_t a;

	if (!nsv.connected || nsv.ka_off || p->len < BASEPACKETSIZE + 6 || d->packettype != PT_SERVERTICS)
		return;
	if (nsv.main_beat == 0 || (INT64)(p->t - nsv.main_beat) <= (INT64)NSV_LOAD_MS * 147456)
		return; // the game thread is looking at the network: it sends its own
	if (nsv.ka_t && (INT64)(p->t - nsv.ka_t) < (INT64)NSV_KEEPALIVE_MS * 147456)
		return;
	if (Checksum(p->data, p->len) != d->checksum)
		return;
	memset(&a, 0, BASEPACKETSIZE);
	a.packettype = PT_BASICKEEPALIVE;
	a.checksum = Checksum((const UINT8 *)&a, BASEPACKETSIZE);
	nsv.ka_t = p->t;
	if (sendto(nsv.fd, &a, BASEPACKETSIZE, 0, (const struct sockaddr *)from, sizeof *from) < 0)
		nsv.st.early_ack_errors++;
	else
		nsv.st.load_keepalives++;
}

static void SvcThread(void *arg)
{
	(void)arg;
	while (!nsv.stop)
	{
		const UINT32 head = nsv.head;
		const boolean full = (UINT32)(head - nsv.tail) >= NSV_SLOTS;
		nsv_packet_t *p = full ? &scratch : &nsv.ring[head & (NSV_SLOTS - 1)];
		struct sockaddr_in from;
		socklen_t fl = (socklen_t)sizeof from;
		int n;

		nsv.in_recv = true;
		n = recvfrom(nsv.fd, p->data, MAXPACKETLENGTH, 0, (struct sockaddr *)&from, &fl);
		nsv.in_recv = false;
		if (n < 0)
		{
			if (nsv.stop || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
				break;
			PS2_SleepUs(500); // the socket is non-blocking after all: poll
			continue;
		}
		p->t = GetTimerSystemTime();
		p->len = (UINT32)n;
		p->from_addr = from.sin_addr.s_addr;
		p->from_port = from.sin_port;
		if (full)
		{
			nsv.st.dropped++;
			continue;
		}
		BARRIER();
		nsv.head = head + 1;
		nsv.st.received++;
		if ((UINT32)(head + 1 - nsv.tail) > nsv.st.max_depth)
			nsv.st.max_depth = (UINT32)(head + 1 - nsv.tail);
		EarlyAck(p, &from);
		LoadKeepAlive(p, &from);
	}
	nsv.tid = -1; // the thread ends itself (the game thread waits for this in Stop)
	ExitDeleteThread();
}

boolean PS2NetSvc_Start(int fd)
{
	ee_thread_t t;
	int flags;

	if (nsv.tid >= 0 || M_CheckParm("-netnosvc"))
		return false;
	if (!nsv.ring)
	{
		nsv.ring = (nsv_packet_t *)memalign(64, sizeof(nsv_packet_t) * NSV_SLOTS);
		if (!nsv.ring)
			return false;
	}
	if (!nsv_stack)
	{
		nsv_stack = (UINT8 *)memalign(16, NSV_STACK);
		if (!nsv_stack)
			return false;
	}
	nsv.head = nsv.tail = 0;
	nsv.stop = false;
	nsv.joined = false;
	nsv.connected = false;
	nsv.main_beat = 0;
	nsv.rx_needed = 0;
	nsv.ka_t = 0;
	nsv.ka_off = M_CheckParm("-netnokeepalive") != 0;
	nsv.fd = fd;
	memset(&nsv.st, 0, sizeof nsv.st);
	flags = fcntl(fd, F_GETFL, 0);
	if (flags != -1 && (flags & O_NONBLOCK))
		fcntl(fd, F_SETFL, flags & ~O_NONBLOCK); // the thread blocks in recvfrom; the game thread no longer reads the socket
	memset(&t, 0, sizeof t);
	t.func = (void *)SvcThread;
	t.stack = nsv_stack;
	t.stack_size = NSV_STACK;
	t.gp_reg = &_gp;
	t.initial_priority = NSV_PRIO;
	if (M_CheckParm("-netsvcprio") && M_IsNextParm())
		t.initial_priority = atoi(M_GetNextParm()); // a test: the priority of the thread against lwIP's (6), see docs/GATES/g1/opt12-NET.md
	nsv.tid = CreateThread(&t);
	if (nsv.tid < 0)
		return false;
	if (StartThread(nsv.tid, NULL) < 0)
	{
		DeleteThread(nsv.tid);
		nsv.tid = -1;
		return false;
	}
	return true;
}

void PS2NetSvc_Stop(void)
{
	int tid = nsv.tid, waited;

	if (tid < 0)
		return;
	nsv.stop = true;
	// the thread is either in recvfrom() (waiting on the socket's mailbox: it can be removed without harm) or busy with a datagram (inside lwIP: let it come back first)
	for (waited = 0; waited < 300 && !nsv.in_recv && nsv.tid >= 0; waited++)
		PS2_SleepUs(1000);
	if (nsv.tid >= 0)
	{
		TerminateThread(tid);
		DeleteThread(tid);
		nsv.tid = -1;
	}
	nsv.joined = false;
	nsv.connected = false;
	if (nsv_stack)
	{
		free(nsv_stack); // the thread is deleted: nothing runs on it any more
		nsv_stack = NULL;
	}
}

UINT32 PS2NetSvc_HeapUse(void)
{
	return (nsv.ring ? (UINT32)(sizeof(nsv_packet_t) * NSV_SLOTS) : 0) + (nsv_stack ? NSV_STACK : 0);
}

boolean PS2NetSvc_Running(void)
{
	return nsv.tid >= 0;
}

const nsv_packet_t *PS2NetSvc_Peek(void)
{
	if (nsv.tail == nsv.head)
		return NULL;
	return &nsv.ring[nsv.tail & (NSV_SLOTS - 1)];
}

void PS2NetSvc_Pop(void)
{
	BARRIER();
	nsv.tail++;
}

void PS2NetSvc_SetClient(boolean joined)
{
	if (joined && !nsv.joined)
		nsv.rx_needed = 0;
	nsv.joined = joined;
}

void PS2NetSvc_SetConnected(boolean connected)
{
	nsv.connected = connected;
}

void PS2NetSvc_MainBeat(void)
{
	nsv.main_beat = GetTimerSystemTime();
}

void PS2NetSvc_GetStats(nsv_stats_t *out)
{
	*out = nsv.st;
}
