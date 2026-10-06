// PS2 network bring-up (PS2-120): see ps2_net.h.
#include <kernel.h>
#include <sifrpc.h>
#include <malloc.h>
#include <ps2_eeip_driver.h>
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>

#include "../doomdef.h"
#include "../d_main.h"
#include "../i_video.h"
#include "../m_argv.h"
#include "../v_video.h"

#include "ps2_net.h"

static INT32 netstate; // 0 not tried, 1 up, -1 failed
static char netaddr[24], netgw[24], netmask[24], netdns[24];
static UINT32 heap_before, heap_after;

static const char *const phase_text[] = {
	"Setting the Ethernet link mode...", "Starting the IP stack...", "Applying the IP configuration...",
	"Waiting for the Ethernet link...", "Link is up", "Waiting for the DHCP server...", "DHCP lease received", "Network ready"
};

// A blocking bring-up of up to ~20 s: show where it is (the main loop is not running)
static void Splash(const char *line)
{
	CONS_Printf("PS2 net: %s\n", line);
	if (rendermode != render_soft || !screens[0])
		return;
	V_DrawFill(0, 0, BASEVIDWIDTH, BASEVIDHEIGHT, 31 | V_NOSCALESTART);
	V_DrawCenteredString(BASEVIDWIDTH/2, 84, 0, "NETWORK");
	V_DrawCenteredString(BASEVIDWIDTH/2, 100, 0, line);
	I_FinishUpdate();
}

static void Progress(enum EEIP_PROGRESS_EVENT ev, void *user)
{
	(void)user;
	if ((unsigned)ev < sizeof phase_text / sizeof phase_text[0])
		Splash(phase_text[ev]);
}

static boolean ParseAddr(const char *parm, struct ip4_addr *out)
{
	const char *s;

	if (!M_CheckParm(parm) || !M_IsNextParm())
		return false;
	s = M_GetNextParm();
	return inet_aton(s, out) != 0;
}

boolean PS2Net_Up(void)
{
	eeip_network_config_t cfg;
	struct ip4_addr ip, nm, gw;
	enum EEIP_INIT_STATUS st;
	enum EEIP_NET_STATUS ns;

	if (netstate)
		return netstate > 0;
	netstate = -1;
	heap_before = (UINT32)mallinfo().uordblks;

	Splash("Loading the network modules...");
	st = init_eeip_driver(true);
	if (st != EEIP_INIT_STATUS_OK)
	{
		CONS_Alert(CONS_ERROR, "PS2 net: the network drivers did not start (%d)\n", (int)st);
		return false;
	}

	eeip_network_config_default_dhcp(&cfg);
	cfg.on_progress = Progress;
	cfg.timeout_seconds = M_CheckParm("-nettimeout") && M_IsNextParm() ? atoi(M_GetNextParm()) : 15;
	if (ParseAddr("-ip", &ip))
	{
		// static address: -ip A.B.C.D [-netmask M] [-gateway G]
		if (!ParseAddr("-netmask", &nm))
			IP4_ADDR(&nm, 255, 255, 255, 0);
		if (!ParseAddr("-gateway", &gw))
			IP4_ADDR(&gw, 0, 0, 0, 0);
		cfg.use_dhcp = false;
		cfg.ip = ip;
		cfg.netmask = nm;
		cfg.gateway = gw;
	}

	ns = configure_eeip_network(&cfg);
	if (ns != EEIP_NET_STATUS_OK)
	{
		CONS_Alert(CONS_ERROR, "PS2 net: no network (%d: %s)\n", (int)ns,
			ns == EEIP_NET_STATUS_LINK_TIMEOUT ? "no Ethernet link" : ns == EEIP_NET_STATUS_DHCP_TIMEOUT ? "no DHCP answer" : "configuration error");
		return false;
	}
	eeip_get_current_config(&ip, &nm, &gw);
	{
		// DHCP hands the resolver its server; a static address needs -dns (default: the gateway, which is the usual resolver of a home network)
		struct ip4_addr dns;

		if (ParseAddr("-dns", &dns))
			dns_setserver(0, &dns);
		else if (!cfg.use_dhcp && gw.addr)
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
	return true;
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
