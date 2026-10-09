// PS2 network bring-up (PS2-120): see ps2_net.h.
#include <kernel.h>
#include <timer.h>
#include <delaythread.h>
#include <unistd.h>
#include <sifrpc.h>
#include <malloc.h>
#include <netman.h>
#include <ps2_eeip_driver.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <arpa/inet.h>

#include "../doomdef.h"
#include "../d_main.h"
#include "../i_system.h"
#include "../i_video.h"
#include "../m_argv.h"
#include "../v_video.h"

#include "ps2_net.h"
#include "ps2_menuhints.h"
#include "ps2_netui.h"
#include "ps2_netsvc.h"
#include "../netcode/d_clisrv.h"
#include "../netcode/client_connection.h"
#include "../doomstat.h"
#include "ps2_uiicons.h"

static INT32 netstate; // 0 not up (a new attempt is allowed), 1 up, -1 the network modules did not start (permanent)
static char netaddr[24], netgw[24], netmask[24], netdns[24];
static UINT32 heap_before, heap_after;

// PS2-330 (OPT11 NETUI): the bring-up of libps2_drivers' configure_eeip_network() (ps2_eeip_driver.c; the library was read with objdump for this), step for step but
// with a wait that draws the network screen (ps2_netui.c) and looks at the pad. The library sleeps usleep(1000000) in its loops and calls the progress callback
// only between the phases, so for 15..30 seconds nothing could be shown (in Hardware the last picture simply stayed) and nothing could be cancelled. The calls are
// the library's own: ps2ipInit(), libcglue_ps2ip_getconfig("sm0") / _setconfig(), NetManIoctl(GET_LINK_STATUS). What was printed before is printed as before
// (tools/ps2/net_session.py and the test scenarios wait for "PS2 net: address").
typedef enum { NB_OK, NB_CANCEL, NB_ADAPTER, NB_LINK, NB_DHCP, NB_CONFIG } nb_t;
static INT32 fakelink_ms = -1;

static boolean modules_up, stack_up;
static eeip_network_config_t netcfg; // the static configuration is read through the whole bring-up
static UINT64 retry_after; // I_GetPreciseTime() of the earliest new attempt after the player cancelled / left a failure window (0 = now)
#define NET_RETRY_PAUSE_MS 10000 // a caller that asks again by itself (the master server registration after a failed host start) must not open the window again at once

// PS2-NET-3 (OPT12): the stall watchdog. A thread of its own (priority 1, sleeps one second at a time) that looks at a beat counter the game thread raises in every pass
// of its loops (I_UpdateTime). When the beat stands still for 3 seconds it prints, with write() straight to the console (no stdio lock: the game thread may hold it), the
// state of every EE thread: which of them waits for what. A hang of the whole guest (EE idle loop) was seen with the hardware renderer during the network bring-up; this is
// how the waiting thread and its semaphore are named. Switched on by -netwd (tests); the price is a thread that wakes once a second.
extern void *_gp;
volatile UINT32 ps2net_beat; // raised by I_UpdateTime (src/i_time.c)
static UINT8 *wd_stack; // heap, only with -netwd
static INT32 wd_tid = -1;

static void WdPrint(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void WdPrint(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	if (n > 0)
		write(1, buf, (size_t)(n < (int)sizeof buf ? n : (int)sizeof buf - 1));
}

static void WdDump(const char *why, UINT32 secs)
{
	ee_thread_status_t st;
	ee_sema_t sm;
	INT32 id;

	WdPrint("NETWD %s: no beat for %u s, system time %llu\n", why, (unsigned)secs, (unsigned long long)GetTimerSystemTime());
	for (id = 1; id < 128; id++)
		if (ReferThreadStatus(id, &st) >= 0 && st.status)
			WdPrint("NETWD   thread %d status=%d prio=%d/%d last syscall %p stack=%p wait=%u/%u wakeups=%u\n", (int)id, (int)st.status, (int)st.current_priority,
				(int)st.initial_priority, st.func, st.stack, (unsigned)st.waitType, (unsigned)st.waitId, (unsigned)st.wakeupCount);
	for (id = 1; id < 128; id++)
		if (ReferSemaStatus(id, &sm) >= 0)
			WdPrint("NETWD   sema %d count=%d/%d init=%d waiting=%d attr=%u option=%08x\n", (int)id, (int)sm.count, (int)sm.max_count, (int)sm.init_count, (int)sm.wait_threads,
				(unsigned)sm.attr, (unsigned)sm.option);
}

static void WdThread(void *arg)
{
	UINT32 last = ps2net_beat, still = 0, lines = 0, lines_ref = 0;

	(void)arg;
	for (;;)
	{
		DelayThread(1000000);
		if (++lines_ref == 6)
			WdDump("REFERENCE (running)", 0);
		if (ps2net_beat != last)
		{
			if (still >= 3)
				WdPrint("NETWD: the game thread is running again (beat %u)\n", (unsigned)ps2net_beat);
			last = ps2net_beat;
			still = 0;
			continue;
		}
		still++;
		if (still >= 3 && (still == 3 || still % 10 == 0) && lines++ < 12)
			WdDump("STALL", still);
	}
}

void PS2Net_StartWatchdog(void)
{
	ee_thread_t t;

	if (wd_tid >= 0 || !M_CheckParm("-netwd"))
		return;
	wd_stack = (UINT8 *)memalign(16, 8192);
	if (!wd_stack)
		return;
	memset(&t, 0, sizeof t);
	t.func = (void *)WdThread;
	t.stack = wd_stack;
	t.stack_size = 8192;
	t.gp_reg = &_gp;
	t.initial_priority = 1;
	wd_tid = CreateThread(&t);
	if (wd_tid < 0 || StartThread(wd_tid, NULL) < 0)
		wd_tid = -1;
	else
		WdPrint("NETWD started (thread %d)\n", (int)wd_tid);
}

static void Phase(const char *line)
{
	CONS_Printf("PS2 net: %s\n", line);
}

static INT32 MsSince(UINT64 t0)
{
	return (INT32)(((I_GetPreciseTime() - t0) * 1000) / I_GetPrecisePrecision());
}

// waits `ms` milliseconds; the picture is drawn and the pad read meanwhile. false: the player cancelled
static boolean NetWait(INT32 ms)
{
	const UINT64 t0 = I_GetPreciseTime();

	do
	{
		if (!PS2NetUI_Pump())
			return false;
	}
	while (MsSince(t0) < ms);
	return true;
}

static boolean ParseAddr(const char *parm, struct ip4_addr *out)
{
	const char *s;

	if (!M_CheckParm(parm) || !M_IsNextParm())
		return false;
	s = M_GetNextParm();
	return inet_aton(s, out) != 0;
}

static void BuildConfig(void)
{
	struct ip4_addr ip, nm, gw;

	eeip_network_config_default_dhcp(&netcfg);
	netcfg.on_progress = NULL;
	netcfg.timeout_seconds = M_CheckParm("-nettimeout") && M_IsNextParm() ? atoi(M_GetNextParm()) : 15;
	if (netcfg.timeout_seconds <= 0)
		netcfg.timeout_seconds = 10; // the library's own default
	if (ParseAddr("-ip", &ip))
	{
		// static address: -ip A.B.C.D [-netmask M] [-gateway G]
		if (!ParseAddr("-netmask", &nm))
			IP4_ADDR(&nm, 255, 255, 255, 0);
		if (!ParseAddr("-gateway", &gw))
			IP4_ADDR(&gw, 0, 0, 0, 0);
		netcfg.use_dhcp = false;
		netcfg.ip = ip;
		netcfg.netmask = nm;
		netcfg.gateway = gw;
	}
}

// PS2-NET-4 (OPT12): the threads of the network libraries (NETMAN's Tx / Rx / RPC threads and lwIP's tcpip thread) are created at priorities 86..89, the game thread runs at 8: while
// the game thread computes a frame they get no time at all. A packet the game has just sent waited in the EE until the frame ended (sendto only queues it for the Tx thread), a
// packet that had arrived waited in the same way before it was on the socket. They are raised above the game thread (priority 6, the mixer is at 5); they are short and wait on
// the IOP most of the time. The threads are found by difference: whatever exists after the stack is up and did not exist before the modules were loaded (and is not ours).
#define NET_THREAD_PRIO 6
static UINT8 threads_before[256];
static INT32 boosted_threads;

static void SnapshotThreads(void)
{
	ee_thread_status_t st;
	INT32 tid;

	for (tid = 1; tid < 256; tid++)
		threads_before[tid] = (ReferThreadStatus(tid, &st) >= 0 && st.status) ? 1 : 0;
}

static void BoostNetThreads(void)
{
	ee_thread_status_t st;
	INT32 tid;

	if (M_CheckParm("-netnoboost"))
		return;
	for (tid = 1; tid < 256; tid++)
		if (!threads_before[tid] && tid != wd_tid && ReferThreadStatus(tid, &st) >= 0 && st.status && st.current_priority > NET_THREAD_PRIO)
		{
			const INT32 was = st.current_priority;

			if (ChangeThreadPriority(tid, NET_THREAD_PRIO) >= 0)
			{
				boosted_threads++;
				if (M_CheckParm("-netdebug"))
					CONS_Printf("PS2 net: thread %d raised from priority %d to %d\n", (int)tid, (int)was, (int)NET_THREAD_PRIO);
			}
		}
}

// PS2-NET-7 (OPT12): the DHCP wait. The lease is in the router's hands after 20 ms (DISCOVER, OFFER, REQUEST, ACK: the log of PCSX2 shows both answers 1 ms after each request),
// but lwIP then checks the offered address for a conflict (ACD: three ARP probes, one to two seconds apart, then announcements two seconds apart) and the interface gets its
// address only when that is over: 5.5..7.5 s in which nothing happens (dhcp_status 8, "checking"). acd_tmr() is the function lwIP's own 100 ms timer calls; while the state is
// "checking" it is called ten times for every 100 ms of our loop, from lwIP's thread (tcpip_callback), so the check takes a tenth of the time (the same probes and announcements,
// 100..200 ms apart instead of 1..2 s: a host that owns the address answers an ARP probe within a millisecond).
// (weak: a strong reference made the linker take lwIP's own copies of ip4_addr.o and others from libps2_drivers.a ahead of the stack's, "multiple definition of ip_addr_any";
// both functions are in the ELF anyway, the stack uses them)
extern void acd_tmr(void) __attribute__((weak));
extern signed char tcpip_callback(void (*function)(void *ctx), void *ctx) __attribute__((weak));
#define DHCP_STATE_CHECKING 8
#define ACD_SPEEDUP 10

static void AcdBurst(void *ctx)
{
	INT32 i;

	(void)ctx;
	for (i = 1; i < ACD_SPEEDUP; i++)
		acd_tmr();
}

static nb_t Bringup(void)
{
	t_ip_info info;
	const INT32 limit_ms = netcfg.timeout_seconds * 1000;
	UINT64 t0;

	if (!modules_up)
	{
		enum EEIP_INIT_STATUS st;

		SnapshotThreads();

		PS2NetUI_Step(NETUI_STEP_MODULES, 0);
		Phase("Loading the network modules...");
		st = init_eeip_driver(true); // blocking: three IRX modules (DEV9, NETMAN, SMAP) are loaded through the IOP; nothing can be drawn meanwhile
		if (st != EEIP_INIT_STATUS_OK)
		{
			CONS_Alert(CONS_ERROR, "PS2 net: the network drivers did not start (%d)\n", (int)st);
			return NB_ADAPTER;
		}
		modules_up = true;
	}
	PS2NetUI_StepDone();

	PS2NetUI_Step(NETUI_STEP_STACK, 0);
	if (!stack_up)
	{
		struct ip4_addr zip, znm, zgw;

		Phase("Starting the IP stack...");
		memset(&zip, 0, sizeof zip);
		memset(&znm, 0, sizeof znm);
		memset(&zgw, 0, sizeof zgw);
		ps2ipInit(&zip, &znm, &zgw);
		stack_up = true;
		BoostNetThreads();
		Phase("Setting the Ethernet link mode..."); // link mode AUTO: there is nothing to set
	}
	Phase("Applying the IP configuration...");
	if (libcglue_ps2ip_getconfig("sm0", &info) < 0)
		return NB_CONFIG;
	if (netcfg.use_dhcp)
		info.dhcp_enabled = 1;
	else
	{
		info.ipaddr.s_addr = netcfg.ip.addr;
		info.netmask.s_addr = netcfg.netmask.addr;
		info.gw.s_addr = netcfg.gateway.addr;
		info.dhcp_enabled = 0;
	}
	if (libcglue_ps2ip_setconfig(&info) < 0)
		return NB_CONFIG;
	if (!NetWait(0))
		return NB_CANCEL;
	PS2NetUI_StepDone();

	// the Ethernet link
	PS2NetUI_Step(NETUI_STEP_LINK, netcfg.timeout_seconds);
	Phase("Waiting for the Ethernet link...");
	t0 = I_GetPreciseTime();
	if (fakelink_ms < 0) // -netfakelink SECONDS (a test: the emulator always has a link): the link is taken as down for that long, 999 = never
		fakelink_ms = (M_CheckParm("-netfakelink") && M_IsNextParm()) ? atoi(M_GetNextParm()) * 1000 : 0;
	for (;;)
	{
		if (MsSince(t0) >= fakelink_ms && NetManIoctl(NETMAN_NETIF_IOCTL_GET_LINK_STATUS, NULL, 0, NULL, 0) == NETMAN_NETIF_ETH_LINK_STATE_UP)
			break;
		if (MsSince(t0) >= limit_ms)
			return NB_LINK;
		if (!NetWait(100))
			return NB_CANCEL;
	}
	Phase("Link is up");
	PS2NetUI_StepDone();

	// the address: a DHCP lease, or the static one that is applied already
	PS2NetUI_Step(NETUI_STEP_ADDRESS, netcfg.use_dhcp ? netcfg.timeout_seconds : 0);
	if (netcfg.use_dhcp)
	{
		Phase("Waiting for the DHCP server...");
		t0 = I_GetPreciseTime();
		for (;;)
		{
			// the library's test: DHCP is on and bound (lwIP DHCP_BOUND = 10; 0 = off), plus an address that is not 0.0.0.0
			if (libcglue_ps2ip_getconfig("sm0", &info) >= 0 && info.dhcp_enabled && info.ipaddr.s_addr != 0 && (info.dhcp_status == 10 || info.dhcp_status == 0))
				break;
			if (info.dhcp_status == DHCP_STATE_CHECKING && acd_tmr && tcpip_callback && !M_CheckParm("-netnoacdfast"))
				tcpip_callback(AcdBurst, NULL);
			if (MsSince(t0) >= limit_ms)
				return NB_DHCP;
			if (!NetWait(100))
				return NB_CANCEL;
		}
		Phase("DHCP lease received");
	}
	PS2NetUI_StepDone();
	Phase("Network ready");
	return NB_OK;
}

static netui_fail_t FailReason(nb_t rc)
{
	switch (rc)
	{
		case NB_ADAPTER: return NETUI_FAIL_ADAPTER;
		case NB_LINK: return NETUI_FAIL_LINK;
		case NB_DHCP: return NETUI_FAIL_DHCP;
		default: return NETUI_FAIL_CONFIG;
	}
}

static boolean fakedown_done;

boolean PS2Net_Up(void)
{
	struct ip4_addr ip, nm, gw;
	nb_t rc;
	INT32 attempt = 0;
	boolean ui;

	if (netstate > 0 && modules_up && !M_CheckParm("-netnolinkcheck")
		&& (NetManIoctl(NETMAN_NETIF_IOCTL_GET_LINK_STATUS, NULL, 0, NULL, 0) != NETMAN_NETIF_ETH_LINK_STATE_UP
			|| (M_CheckParm("-netfakedown") && !fakedown_done && (fakedown_done = true)))) // -netfakedown (a test: the emulator always has a link): the first reconnect takes the link as gone
	{
		// PS2-NET-8 (OPT12): the network was up at the last connect, and the cable has been pulled since (the game gave up on the server after 10 s and went back to the title):
		// the next "connect" waits for the link and the lease again, with the network screen, instead of sending into nothing
		CONS_Printf("PS2 net: the Ethernet link is gone, bringing the network up again\n");
		netstate = 0;
	}
	if (netstate)
		return netstate > 0;
	if (retry_after && (INT64)(I_GetPreciseTime() - retry_after) < 0)
	{
		CONS_Printf("PS2 net: not tried again so soon after the last failure\n");
		return false;
	}
	retry_after = 0;
	heap_before = (UINT32)mallinfo().uordblks;
	PS2Net_StartWatchdog();
	BuildConfig();
	ui = PS2NetUI_Begin(netcfg.use_dhcp);

	for (;;)
	{
		attempt++;
		rc = Bringup();
		if (rc == NB_OK)
			break;
		if (rc == NB_CANCEL)
		{
			CONS_Printf("PS2 net: cancelled\n");
			break;
		}
		CONS_Alert(CONS_ERROR, "PS2 net: no network (%d: %s)\n", (int)rc - (int)NB_ADAPTER,
			rc == NB_LINK ? "no Ethernet link" : rc == NB_DHCP ? "no DHCP answer" : rc == NB_ADAPTER ? "no network adapter" : "configuration error");
		if (rc == NB_ADAPTER)
			netstate = -1; // the modules are in an unknown state: no second try in this session
		if (ui && PS2NetUI_Failed(FailReason(rc), attempt, rc != NB_ADAPTER))
			continue; // "Try again"
		break;
	}
	if (rc != NB_OK)
	{
		if (ui)
			PS2NetUI_End();
		retry_after = I_GetPreciseTime() + (UINT64)NET_RETRY_PAUSE_MS * (I_GetPrecisePrecision() / 1000);
		return false;
	}

	eeip_get_current_config(&ip, &nm, &gw);
	{
		// DHCP hands the resolver its server; a static address needs -dns (default: the gateway, which is the usual resolver of a home network)
		struct ip4_addr dns;

		if (ParseAddr("-dns", &dns))
			dns_setserver(0, &dns);
		else if (!netcfg.use_dhcp && gw.addr)
			dns_setserver(0, &gw);
		snprintf(netdns, sizeof netdns, "%s", inet_ntoa(*dns_getserver(0)));
		CONS_Printf("PS2 net: dns %s\n", netdns);
	}
	snprintf(netaddr, sizeof netaddr, "%s", inet_ntoa(ip));
	snprintf(netgw, sizeof netgw, "%s", inet_ntoa(gw));
	snprintf(netmask, sizeof netmask, "%s", inet_ntoa(nm)); // inet_ntoa has one static buffer: copy each result before the next call
	heap_after = (UINT32)mallinfo().uordblks;
	CONS_Printf("PS2 net: address %s, netmask %s, gateway %s (stack heap %u B)\n", netaddr, netmask, netgw, (unsigned)(heap_after - heap_before));
	netstate = 1;
	if (ui)
	{
		PS2NetUI_Ready(netaddr, netmask, netgw, netdns, netcfg.use_dhcp);
		PS2NetUI_End();
	}
	return true;
}

boolean PS2Net_Waiting(const char *what, UINT32 elapsed_ms)
{
	return PS2NetUI_Waiting(what, elapsed_ms);
}

void PS2Net_WaitingEnd(void)
{
	PS2NetUI_WaitingEnd();
}

boolean PS2Net_Reported(void)
{
	return PS2NetUI_Reported();
}

int PS2Net_Unlink(const char *path)
{
	FILE *f;

	if (remove(path) == 0)
		return 0;
	f = fopen(path, "wb");
	if (!f)
		return -1;
	fclose(f);
	return 0;
}

const char *PS2Net_Address(void)
{
	return netstate > 0 ? netaddr : "";
}

const char *PS2Net_Gateway(void)
{
	return netstate > 0 ? netgw : "0.0.0.0";
}

UINT32 PS2Net_HeapUse(void)
{
	return netstate > 0 ? heap_after - heap_before : 0;
}

// PS2-132: -netcmd "300:listserv|420:connect 192.168.1.5" runs console commands at displayed frame N (no keyboard can be used from outside in tests).
// -netcmd file:NAME reads the list from <HOME>/NAME (one "N:command" per line or separated by '|').
#include "../command.h"
#include "../i_system.h"

#define NETCMD_MAX 32
static struct { UINT32 frame; char cmd[96]; boolean done; } netcmdlist[NETCMD_MAX];
static INT32 numnetcmdlist;
static UINT32 netcmd_frames;

static void NetCmd_Parse(const char *spec)
{
	const char *p = spec;

	while (*p && numnetcmdlist < NETCMD_MAX)
	{
		UINT32 n = 0;
		size_t len = 0;

		while (*p == ' ' || *p == '|' || *p == '\n' || *p == '\r')
			p++;
		if (!*p)
			break;
		while (*p >= '0' && *p <= '9')
			n = n * 10 + (UINT32)(*p++ - '0');
		if (*p != ':')
			I_Error("-netcmd: expected N:command near '%.20s'", p);
		p++;
		while (*p && *p != '|' && *p != '\n' && *p != '\r' && len < sizeof netcmdlist[0].cmd - 2)
			netcmdlist[numnetcmdlist].cmd[len++] = *p++;
		netcmdlist[numnetcmdlist].cmd[len] = 0;
		netcmdlist[numnetcmdlist].frame = n;
		numnetcmdlist++;
	}
}

extern UINT32 ps2net_rx, ps2net_tx, ps2net_txerr; // i_tcp.c
extern char ps2net_lastfrom[];

// PS2-NET-2 (OPT12): -netthreads lists the EE threads (priority, state, entry point) once the IP stack is up and again two seconds later: which of them can run while the
// game thread computes a frame? (the netman receive thread and lwIP's thread decide how soon a datagram is on the socket)
static void ListThreads(const char *when)
{
	ee_thread_status_t st;
	INT32 tid, n = 0;

	for (tid = 1; tid < 256; tid++)
		if (ReferThreadStatus(tid, &st) >= 0)
		{
			CONS_Printf("NETTHREAD %s tid=%d status=%d prio=%d/%d func=%p stack=%d wait=%u/%u wakeups=%u%s\n", when, (int)tid, (int)st.status, (int)st.current_priority,
				(int)st.initial_priority, st.func, (int)st.stack_size, (unsigned)st.waitType, (unsigned)st.waitId, (unsigned)st.wakeupCount, tid == GetThreadId() ? " (game)" : "");
			n++;
		}
	CONS_Printf("NETTHREAD %s: %d threads\n", when, (int)n);
}

void PS2Net_Frame(void)
{
	static boolean parsed;
	static UINT32 frames, lastrx, lasttx;
	static INT32 thread_dumps, thread_wait;
	INT32 i;

	if (netstate > 0 && (ps2net_rx | ps2net_tx) && M_CheckParm("-netdebug") && ++frames % 70 == 0)
	{
		CONS_Printf("NETSTAT frame %u: rx=%u (+%u) tx=%u (+%u) txerr=%u last=%s\n", (unsigned)frames, (unsigned)ps2net_rx, (unsigned)(ps2net_rx - lastrx),
			(unsigned)ps2net_tx, (unsigned)(ps2net_tx - lasttx), (unsigned)ps2net_txerr, ps2net_lastfrom);
		lastrx = ps2net_rx;
		lasttx = ps2net_tx;
	}

	if (netstate > 0 && thread_dumps < 2 && M_CheckParm("-netthreads") && (thread_dumps == 0 || ++thread_wait > 140))
		ListThreads(thread_dumps++ ? "later" : "up");

	// PS2-NET-5: the receive thread acknowledges the tics of the server for a joined client
	PS2NetSvc_SetClient(netstate > 0 && netgame && client && gamestate == GS_LEVEL && cl_mode == CL_CONNECTED);
	PS2NetSvc_SetConnected(netstate > 0 && netgame && client && cl_mode == CL_CONNECTED); // OPT13-IO (RS-09): the keep-alive of a long load
	if (netstate > 0 && PS2NetSvc_Running() && M_CheckParm("-netdebug") && frames % 70 == 0)
	{
		nsv_stats_t ns;

		PS2NetSvc_GetStats(&ns);
		CONS_Printf("NETSVC frame %u: received %u dropped %u early-acks %u early-mis %u (errors %u) load-keepalives %u max-depth %u, C heap in use %u B\n", (unsigned)frames, (unsigned)ns.received, (unsigned)ns.dropped,
			(unsigned)ns.early_acks, (unsigned)ns.early_mis, (unsigned)ns.early_ack_errors, (unsigned)ns.load_keepalives, (unsigned)ns.max_depth, (unsigned)mallinfo().uordblks);
	}

	PS2MenuHints_Frame(); // PS2-339: the crawler's step (the command and the options are set up at the first call)
	if (!parsed)
	{
		parsed = true;
		PS2UI_RegisterCommands(); // PS2-336: "ps2_icons"
		if (M_CheckParm("-netcmd") && M_IsNextParm())
		{
			const char *arg = M_GetNextParm();
			static char buf[2048];

			if (!strncmp(arg, "file:", 5))
			{
				char path[256];
				FILE *f;
				size_t got;

				snprintf(path, sizeof path, "%s/%s", I_GetEnv("HOME") ? I_GetEnv("HOME") : ".", arg + 5);
				f = fopen(path, "rb");
				if (!f)
					I_Error("-netcmd: cannot open %s", path);
				got = fread(buf, 1, sizeof buf - 1, f);
				fclose(f);
				buf[got] = 0;
				arg = buf;
			}
			NetCmd_Parse(arg);
		}
	}
	if (!numnetcmdlist)
		return;
	netcmd_frames++;
	for (i = 0; i < numnetcmdlist; i++)
		if (!netcmdlist[i].done && netcmdlist[i].frame <= netcmd_frames)
		{
			netcmdlist[i].done = true;
			CONS_Printf("NETCMD frame %u: %s\n", (unsigned)netcmd_frames, netcmdlist[i].cmd);
			COM_BufAddText(netcmdlist[i].cmd);
			COM_BufAddText("\n");
		}
}
