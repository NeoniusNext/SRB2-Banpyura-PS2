// SRB2 PS2 port, OPT12 NET: latency probes of the game connection (PS2-NET-1). See netlat.h.
#include "netlat.h"

#ifdef NETLAT

#include "../doomdef.h"
#include "../doomstat.h"
#include "../g_game.h"
#include "../i_system.h"
#include "../i_time.h"
#include "../m_argv.h"
#include "d_clisrv.h"
#include "net_command.h"

#define NL_RING 256   // tics remembered (a power of two; a round trip is a few tics)
#define NL_BUCKETS 128 // histogram: 4 ms per bucket, the last one is "512 ms and more"
#define NL_NODES 8

typedef struct
{
	UINT32 n, min, max;
	UINT64 sum;
	UINT16 hist[NL_BUCKETS];
} nl_stat_t;

static boolean nl_on, nl_checked;
static UINT64 nl_prec;           // precise units per second
static tic_t nl_next;            // I_GetTime() of the next line

static INT32 nl_passtype, nl_passreal, nl_trace_left = -1;
static precise_t nl_take[NL_RING];   // client: when the game took the datagram with tic N off the ring
static precise_t nl_made[NL_RING];   // server: when tic N was made
static precise_t nl_arrive[NL_RING]; // client: when tic N arrived
static nl_stat_t nl_rtt[NL_NODES];   // server: round trip of a tic per client node
static nl_stat_t nl_cmdgap[NL_NODES]; // server: time between two packets of a client node
static precise_t nl_cmdlast[NL_NODES];
static UINT32 nl_cmdpkts[NL_NODES];
static nl_stat_t nl_tcgap;           // client: time between two PT_SERVERTICS packets
static precise_t nl_tclast;
static UINT32 nl_tcpkts, nl_tcdup;
static nl_stat_t nl_runwait;         // client: arrival of a tic -> its run
static nl_stat_t nl_rxage;           // PS2: arrival of a datagram on the socket -> the game takes it
static UINT64 nl_arrival;            // arrival time of the datagram being handled (0: unknown)
static nl_stat_t nl_rungap;          // client: time between two passes of the main loop that ran tics (28.6 ms when the picture moves evenly)
static nl_stat_t nl_poll;            // time between two looks at the socket
static precise_t nl_polllast;
static nl_stat_t nl_frame;           // time between two passes of the main loop
static precise_t nl_framelast;
static nl_stat_t nl_backlog;         // tics waiting when one runs (in units of 1000)
static UINT32 nl_sent, nl_sentbytes;

static boolean On(void)
{
	if (!nl_checked)
	{
		nl_checked = true;
		nl_on = M_CheckParm("-netlat") != 0;
		nl_prec = I_GetPrecisePrecision();
	}
	return nl_on;
}

static UINT32 Us(precise_t dt)
{
	const INT64 d = (INT64)dt;

	if (d <= 0)
		return 0;
	return (UINT32)(((UINT64)d * 1000000) / nl_prec);
}

static void Add(nl_stat_t *s, UINT32 us)
{
	UINT32 b = us / 4000;

	if (!s->n || us < s->min)
		s->min = us;
	if (us > s->max)
		s->max = us;
	s->sum += us;
	s->n++;
	if (b >= NL_BUCKETS)
		b = NL_BUCKETS - 1;
	if (s->hist[b] < 65535)
		s->hist[b]++;
}

// the value (in us) below which `pct` percent of the samples lie (to the bucket)
static UINT32 Pct(const nl_stat_t *s, UINT32 pct)
{
	UINT32 want = (s->n * pct + 99) / 100, acc = 0, b;

	if (!s->n)
		return 0;
	for (b = 0; b < NL_BUCKETS; b++)
	{
		acc += s->hist[b];
		if (acc >= want)
			return (b + 1) * 4000;
	}
	return s->max;
}

static void Print(const char *what, const char *name, const nl_stat_t *s)
{
	if (!s->n)
		return;
	CONS_Printf("NETLAT %s %s n=%u min=%u avg=%u p95=%u max=%u us\n", what, name, (unsigned)s->n, (unsigned)s->min, (unsigned)(s->sum / s->n),
		(unsigned)Pct(s, 95), (unsigned)s->max);
}

void NetLat_TicMade(tic_t tic)
{
	if (On())
		nl_made[tic % NL_RING] = I_GetPreciseTime();
}

void NetLat_ClientPacket(INT32 node, tic_t was, tic_t acked)
{
	precise_t now;

	if (!On() || node < 1 || node >= NL_NODES)
		return;
	now = I_GetPreciseTime();
	nl_cmdpkts[node]++;
	if (nl_cmdlast[node])
		Add(&nl_cmdgap[node], Us(now - nl_cmdlast[node]));
	nl_cmdlast[node] = now;
	if (acked > was && acked - was < NL_RING / 2)
		Add(&nl_rtt[node], Us(now - nl_made[(acked - 1) % NL_RING])); // the round trip of the newest tic the packet confirms
}

void NetLat_RxAge(UINT64 arrival)
{
	if (!On())
		return;
	nl_arrival = arrival;
	Add(&nl_rxage, Us(I_GetPreciseTime() - arrival));
}

void NetLat_Pass(INT32 type, INT32 realtics)
{
	nl_passtype = type;
	nl_passreal = realtics;
}

void NetLat_RunPass(void)
{
	static precise_t last;
	precise_t now;

	if (!On() || !client || !netgame)
		return;
	now = I_GetPreciseTime();
	if (last)
		Add(&nl_rungap, Us(now - last));
	last = now;
}

void NetLat_ServerTics(tic_t first, tic_t end, tic_t neededtic_before)
{
	precise_t now;
	tic_t t;

	if (!On())
		return;
	now = nl_arrival ? (precise_t)nl_arrival : I_GetPreciseTime(); // the moment the datagram reached the socket, when the receive thread knows it
	nl_tcpkts++;
	if (nl_tclast)
		Add(&nl_tcgap, Us(now - nl_tclast));
	nl_tclast = now;
	if (end <= neededtic_before)
	{
		nl_tcdup++;
		return;
	}
	for (t = first > neededtic_before ? first : neededtic_before; t < end; t++)
	{
		nl_arrive[t % NL_RING] = now;
		nl_take[t % NL_RING] = I_GetPreciseTime();
	}
}

void NetLat_TicRun(tic_t tic, INT32 backlog)
{
	precise_t now;

	if (!On() || !client)
		return;
	now = I_GetPreciseTime();
	if (nl_arrive[tic % NL_RING])
		Add(&nl_runwait, Us(now - nl_arrive[tic % NL_RING]));
	if (nl_trace_left < 0)
		nl_trace_left = M_CheckParm("-netlattrace") ? 700 : 0;
	if (nl_trace_left > 0 && tic > 300)
	{
		nl_trace_left--;
		CONS_Printf("NETLATTRACE tic=%u arrive->take=%u take->run=%u arrive->run=%u us backlog=%d pass=%s realtics=%d\n", (unsigned)tic,
			(unsigned)Us(nl_take[tic % NL_RING] - nl_arrive[tic % NL_RING]), (unsigned)Us(now - nl_take[tic % NL_RING]),
			(unsigned)Us(now - nl_arrive[tic % NL_RING]), (int)backlog, nl_passtype ? "early" : "clock", (int)nl_passreal);
	}
	Add(&nl_backlog, (UINT32)(backlog > 0 ? backlog : 0) * 1000);
}

void NetLat_Poll(void)
{
	precise_t now;

	if (!On())
		return;
	now = I_GetPreciseTime();
	if (nl_polllast)
		Add(&nl_poll, Us(now - nl_polllast));
	nl_polllast = now;
}

void NetLat_Sent(INT32 type, INT32 bytes)
{
	(void)type;
	if (!On())
		return;
	nl_sent++;
	nl_sentbytes += (UINT32)bytes;
}

static void Reset(nl_stat_t *s)
{
	memset(s, 0, sizeof *s);
}

void NetLat_Frame(void)
{
	precise_t now;
	tic_t t;
	INT32 i;

	if (!On())
		return;
	now = I_GetPreciseTime();
	if (nl_framelast)
		Add(&nl_frame, Us(now - nl_framelast));
	nl_framelast = now;
	t = I_GetTime();
	if (t < nl_next)
		return;
	nl_next = t + 2 * TICRATE;
	if (!netgame)
		return;
	{
		INT32 buf = 0;
		UINT32 starves = 0;

#ifdef PS2
		D_NetEarlyState(&buf, &starves);
#endif
		CONS_Printf("NETLAT --- t=%u gametic=%u maketic=%u neededtic=%u ping=%u ms sent=%u (%u B) buf=%d starves=%u\n", (unsigned)t, (unsigned)gametic, (unsigned)maketic, (unsigned)neededtic,
			(unsigned)playerpingtable[consoleplayer], (unsigned)nl_sent, (unsigned)nl_sentbytes, (int)buf, (unsigned)starves);
	}
	Print("frame", "", &nl_frame);
	Print("poll", "", &nl_poll);
	Print("rx-age", "", &nl_rxage);
	Print("run-gap", "", &nl_rungap);
	if (client)
	{
		CONS_Printf("NETLAT tics packets=%u duplicate=%u\n", (unsigned)nl_tcpkts, (unsigned)nl_tcdup);
		Print("tics-gap", "", &nl_tcgap);
		Print("run-wait", "", &nl_runwait);
		if (nl_backlog.n)
			CONS_Printf("NETLAT backlog n=%u avg=%u/1000 max=%u/1000\n", (unsigned)nl_backlog.n, (unsigned)(nl_backlog.sum / nl_backlog.n / 1), (unsigned)nl_backlog.max);
	}
	for (i = 1; i < NL_NODES; i++)
		if (nl_rtt[i].n || nl_cmdpkts[i])
		{
			char name[16];

			snprintf(name, sizeof name, "node%d", (int)i);
			CONS_Printf("NETLAT client-packets %s count=%u\n", name, (unsigned)nl_cmdpkts[i]);
			Print("rtt", name, &nl_rtt[i]);
			Print("cmd-gap", name, &nl_cmdgap[i]);
		}
	Reset(&nl_frame);
	Reset(&nl_poll);
	Reset(&nl_rxage);
	Reset(&nl_rungap);
	Reset(&nl_tcgap);
	Reset(&nl_runwait);
	Reset(&nl_backlog);
	for (i = 0; i < NL_NODES; i++)
	{
		Reset(&nl_rtt[i]);
		Reset(&nl_cmdgap[i]);
		nl_cmdpkts[i] = 0;
	}
	nl_tcpkts = nl_tcdup = 0;
	nl_sent = nl_sentbytes = 0;
}

#endif
