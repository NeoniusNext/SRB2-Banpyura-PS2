// PS2 network bring-up (PS2-120): see ps2_net.h.
#include <kernel.h>
#include <sifrpc.h>
#include <malloc.h>
#include <netman.h>
#include <ps2_eeip_driver.h>
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>

#include "../doomdef.h"
#include "../d_main.h"
#include "../i_system.h"
#include "../i_video.h"
#include "../m_argv.h"
#include "../v_video.h"

#include "ps2_net.h"
#include "ps2_netui.h"
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

static boolean modules_up, stack_up;
static eeip_network_config_t netcfg; // the static configuration is read through the whole bring-up
static UINT64 retry_after; // I_GetPreciseTime() of the earliest new attempt after the player cancelled / left a failure window (0 = now)
#define NET_RETRY_PAUSE_MS 10000 // a caller that asks again by itself (the master server registration after a failed host start) must not open the window again at once

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

static nb_t Bringup(void)
{
	t_ip_info info;
	const INT32 limit_ms = netcfg.timeout_seconds * 1000;
	UINT64 t0;

	if (!modules_up)
	{
		enum EEIP_INIT_STATUS st;

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
	for (;;)
	{
		if (NetManIoctl(NETMAN_NETIF_IOCTL_GET_LINK_STATUS, NULL, 0, NULL, 0) == NETMAN_NETIF_ETH_LINK_STATE_UP)
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

boolean PS2Net_Up(void)
{
	struct ip4_addr ip, nm, gw;
	nb_t rc;
	INT32 attempt = 0;
	boolean ui;

	if (netstate)
		return netstate > 0;
	if (retry_after && (INT64)(I_GetPreciseTime() - retry_after) < 0)
	{
		CONS_Printf("PS2 net: not tried again so soon after the last failure\n");
		return false;
	}
	retry_after = 0;
	heap_before = (UINT32)mallinfo().uordblks;
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
static struct { UINT32 frame; char cmd[96]; boolean done; } netcmds[NETCMD_MAX];
static INT32 numnetcmds;
static UINT32 netcmd_frames;

static void NetCmd_Parse(const char *spec)
{
	const char *p = spec;

	while (*p && numnetcmds < NETCMD_MAX)
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
		while (*p && *p != '|' && *p != '\n' && *p != '\r' && len < sizeof netcmds[0].cmd - 2)
			netcmds[numnetcmds].cmd[len++] = *p++;
		netcmds[numnetcmds].cmd[len] = 0;
		netcmds[numnetcmds].frame = n;
		numnetcmds++;
	}
}

extern UINT32 ps2net_rx, ps2net_tx, ps2net_txerr; // i_tcp.c
extern char ps2net_lastfrom[];

void PS2Net_Frame(void)
{
	static boolean parsed;
	static UINT32 frames, lastrx, lasttx;
	INT32 i;

	if (netstate > 0 && (ps2net_rx | ps2net_tx) && M_CheckParm("-netdebug") && ++frames % 70 == 0)
	{
		CONS_Printf("NETSTAT frame %u: rx=%u (+%u) tx=%u (+%u) txerr=%u last=%s\n", (unsigned)frames, (unsigned)ps2net_rx, (unsigned)(ps2net_rx - lastrx),
			(unsigned)ps2net_tx, (unsigned)(ps2net_tx - lasttx), (unsigned)ps2net_txerr, ps2net_lastfrom);
		lastrx = ps2net_rx;
		lasttx = ps2net_tx;
	}

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
	if (!numnetcmds)
		return;
	netcmd_frames++;
	for (i = 0; i < numnetcmds; i++)
		if (!netcmds[i].done && netcmds[i].frame <= netcmd_frames)
		{
			netcmds[i].done = true;
			CONS_Printf("NETCMD frame %u: %s\n", (unsigned)netcmd_frames, netcmds[i].cmd);
			COM_BufAddText(netcmds[i].cmd);
			COM_BufAddText("\n");
		}
}
