// PS2-155 (OPT9-M): USB mouse, see ps2_mouse.h.
// ps2mouse.irx (ps2sdk usbmouse) is read in its "diff" mode: libmouse's PS2MouseRead returns the motion / wheel summed since the previous read and
// the button bits (BTN1 left, BTN2 right, BTN3 middle, the DBL bits are not used: the engine counts double clicks itself). The driver merges all
// mice that are plugged in into ONE stream (measured in PCSX2 with two HID mice: PS2MouseEnum = 2, the buttons of both are OR-ed), so a second mouse
// cannot be told apart: the stream drives player 1 when use_mouse is on, otherwise player 2 when use_mouse2 is on (one mouse + one pad in split
// screen); see the table in docs/GATES/g1/opt9-M.md.
// The conversion to events is the one of the SDL port (src/sdl/i_video.c: one ev_mouse per poll with the summed motion, KEY_MOUSE1 + n for the
// buttons, a keydown without keyup for each wheel notch whose gamekeydown is cleared at the next poll).
// -usbtest "M-lines" or -usbtest file:NAME (<HOME>/NAME) feeds recorded HID boot-protocol reports through the same conversion without a mouse
// (tests in PCSX2, where no pointer can be moved from outside, see tools/ps2/usb_run.py); format below at ScriptParse.
#include <kernel.h>
#include <libmouse.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../doomdef.h"
#include "../doomstat.h"
#include "../d_main.h"
#include "../d_event.h"
#include "../command.h"
#include "../g_input.h"
#include "../g_game.h"
#include "../i_system.h"
#include "../m_argv.h"
#include "../v_video.h"
#include "../netcode/d_netcmd.h"

#include "ps2_usb.h"
#include "ps2_mouse.h"

#define COUNT_PERIOD 35 // polls (one per tic: one second) between two PS2MouseEnum calls
#define MAX_WHEEL    4  // notches posted per poll and direction
#define NBUTTONS     3  // BTN1..BTN3 of ps2mouse.h

typedef struct
{
	INT32 x, y, wheel;
	UINT32 buttons;
} mdata_t;

static INT32 drv_state;       // 0 = not tried, 1 = reading ps2mouse.irx, -1 = no driver
static INT32 nmice;           // PS2MouseEnum at the last count (script: the "C" lines)
static INT32 wasmice = -1;
static UINT32 polls;          // PS2Mouse_Poll calls
static UINT32 nextcount;      // poll number of the next count
static UINT8 held[2];         // buttons held, per player slot (bit n = KEY_MOUSE1 + n / KEY_2MOUSE1 + n)
static INT32 curx = -1, cury; // virtual pointer of player 1
static UINT32 posted_ev;
static UINT32 reads, polcalls;
static UINT64 readcycles, polcycles; // EE cycles (cop0 Count differences) spent in PS2MouseRead / in the whole poll
static boolean mouse_logging; // -mouselog: every read that moved or pressed something to the log

// ---------------------------------------------------------------------------------------------
// Events (SDL port: Impl_HandleMouseButtonEvent / Impl_HandleMouseWheelEvent / the ev_mouse of I_GetEvent)
// ---------------------------------------------------------------------------------------------

static void Post(evtype_t type, INT32 key, INT32 x, INT32 y)
{
	event_t e;

	memset(&e, 0, sizeof e);
	e.type = type;
	e.key = key;
	e.x = x;
	e.y = y;
	D_PostEvent(&e);
	posted_ev++;
}

static void PostButton(INT32 player, INT32 n, boolean isdown)
{
	Post(isdown ? ev_keydown : ev_keyup, (player ? KEY_2MOUSE1 : KEY_MOUSE1) + n, 0, 0);
}

static void ReleaseAll(INT32 player)
{
	INT32 n;

	for (n = 0; n < NBUTTONS; n++)
		if (held[player] & (1 << n))
			PostButton(player, n, false);
	held[player] = 0;
}

static boolean Wants(INT32 player)
{
	return (player ? cv_usemouse2.value : cv_usemouse.value) != 0;
}

// the player a (merged) mouse stream drives, -1 = nobody
static INT32 Target(void)
{
	return Wants(0) ? 0 : (Wants(1) ? 1 : -1);
}

static INT32 Clamp(INT32 v, INT32 hi)
{
	return v < 0 ? 0 : (v > hi ? hi : v);
}

static void Apply(INT32 player, const mdata_t *m)
{
	const UINT32 now = m->buttons & ((1u << NBUTTONS) - 1);
	INT32 n, up = m->wheel > 0 ? m->wheel : 0, dn = m->wheel < 0 ? -m->wheel : 0;

	ReleaseAll(!player); // the cvars were switched over while a button was down
	if (curx < 0)
		curx = vid.width / 2, cury = vid.height / 2;
	if (m->x || m->y)
	{
		Post(player ? ev_mouse2 : ev_mouse, 0, m->x, m->y);
		if (!player)
		{
			curx = Clamp(curx + m->x, vid.width - 1);
			cury = Clamp(cury + m->y, vid.height - 1);
		}
	}
	for (n = 0; n < NBUTTONS; n++)
	{
		const UINT32 bit = 1u << n;

		if ((now & bit) && !(held[player] & bit))
		{
			held[player] |= (UINT8)bit;
			PostButton(player, n, true);
		}
		else if (!(now & bit) && (held[player] & bit))
		{
			held[player] &= (UINT8)~bit;
			PostButton(player, n, false);
		}
	}
	// a notch has no up event: the engine clears the key at the next poll (ClearWheel)
	for (n = 0; n < (up > MAX_WHEEL ? MAX_WHEEL : up); n++)
		Post(ev_keydown, player ? KEY_2MOUSEWHEELUP : KEY_MOUSEWHEELUP, 0, 0);
	for (n = 0; n < (dn > MAX_WHEEL ? MAX_WHEEL : dn); n++)
		Post(ev_keydown, player ? KEY_2MOUSEWHEELDOWN : KEY_MOUSEWHEELDOWN, 0, 0);
}

static void ClearWheel(void)
{
	gamekeydown[KEY_MOUSEWHEELDOWN] = gamekeydown[KEY_MOUSEWHEELUP] = 0;
	gamekeydown[KEY_2MOUSEWHEELDOWN] = gamekeydown[KEY_2MOUSEWHEELUP] = 0;
}

// ---------------------------------------------------------------------------------------------
// -usbtest
// ---------------------------------------------------------------------------------------------
// Lines (or ';'-separated items) of
//   <poll> <dev> <b> <x> <y> [<w>]     one HID boot-protocol report of mouse <dev> (0 or 1) at poll number <poll> (1 = the first PS2Mouse_Poll):
//                                      b = button bits (1 left, 2 right, 4 middle), x y w = signed 8-bit counts (decimal, -128..127)
//   C <poll> <n>                       from <poll> on <n> mice are plugged in (hot-plug test; without any C line one mouse is there)
//   S any|level|menu                   when poll number 1 is: the first poll (default), the first poll of a level, the first poll with a menu open
// Reports of the same poll are summed like the driver's diff mode does (motion and wheel added, the last button state wins), the reports of the
// two devices of the same poll are merged into the one stream the real driver delivers.
#define TEST_MAX 4096
typedef struct
{
	UINT32 poll;
	SINT8 dev;  // -1 = "C" line
	SINT8 x, y, w;
	UINT8 b;
	UINT8 count;
} tstep_t;
static tstep_t *test;
static INT32 ntest, testpos;
static INT32 tcond;   // "S" line: 0 any (the first poll), 1 level (the first poll of a level), 2 menu (the first poll with a menu open)
static UINT32 tstart; // poll number the script clock started at (0 = not yet)
static boolean tdone; // the "script done" line was printed
static UINT8 tbtn[2]; // button bits of the two replayed devices

static const char *SkipWhite(const char *p)
{
	while (*p == ' ' || *p == '\t' || *p == ',')
		p++;
	return p;
}

static boolean ReadInt(const char **pp, INT32 *out)
{
	const char *p = SkipWhite(*pp);
	char *end;
	long v;

	if (!((*p >= '0' && *p <= '9') || *p == '-' || *p == '+')) // a number on this line only (strtol would skip a newline)
		return false;
	v = strtol(p, &end, 10);
	if (end == p)
		return false;
	*out = (INT32)v;
	*pp = end;
	return true;
}

static boolean ScriptParse(const char *spec)
{
	const char *p = spec;

	while (*p && ntest < TEST_MAX)
	{
		tstep_t s;
		INT32 v, poll;

		memset(&s, 0, sizeof s);
		while (*p == ' ' || *p == '\n' || *p == '\r' || *p == ';' || *p == '\t')
			p++;
		if (!*p)
			break;
		if (*p == '#') // comment to the end of the line
		{
			while (*p && *p != '\n')
				p++;
			continue;
		}
		if (*p == 'S' || *p == 's') // S any|level|menu: when the clock of the poll numbers starts
		{
			p = SkipWhite(p + 1);
			tcond = !strncmp(p, "level", 5) ? 1 : (!strncmp(p, "menu", 4) ? 2 : 0);
			while (*p && *p != '\n' && *p != ';')
				p++;
			continue;
		}
		if (*p == 'C' || *p == 'c')
		{
			p++;
			if (!ReadInt(&p, &poll) || !ReadInt(&p, &v))
				return false;
			s.poll = (UINT32)poll;
			s.dev = -1;
			s.count = (UINT8)v;
		}
		else
		{
			if (!ReadInt(&p, &poll) || !ReadInt(&p, &v) || v < 0 || v > 1)
				return false;
			s.poll = (UINT32)poll;
			s.dev = (SINT8)v;
			if (!ReadInt(&p, &v))
				return false;
			s.b = (UINT8)v;
			if (!ReadInt(&p, &v))
				return false;
			s.x = (SINT8)v;
			if (!ReadInt(&p, &v))
				return false;
			s.y = (SINT8)v;
			if (ReadInt(&p, &v))
				s.w = (SINT8)v;
		}
		while (*p && *p != '\n' && *p != ';')
			p++;
		test[ntest++] = s;
	}
	return true;
}

static void ScriptInit(void)
{
	const char *arg;
	INT32 i, j;

	if (!M_CheckParm("-usbtest") || !M_IsNextParm())
		return;
	arg = M_GetNextParm();
	test = calloc(TEST_MAX, sizeof *test);
	if (!test)
		I_Error("-usbtest: out of memory");
	if (!strncmp(arg, "file:", 5))
	{
		char path[256], *buf = malloc(65536);
		FILE *f;
		size_t got;

		if (!buf)
			I_Error("-usbtest: out of memory");
		snprintf(path, sizeof path, "%s/%s", I_GetEnv("HOME") ? I_GetEnv("HOME") : ".", arg + 5);
		f = fopen(path, "rb");
		if (!f)
			I_Error("-usbtest: cannot open %s", path);
		got = fread(buf, 1, 65535, f);
		fclose(f);
		buf[got] = 0;
		if (!ScriptParse(buf))
			I_Error("-usbtest: cannot parse %s near step %d", path, (int)ntest + 1);
		free(buf);
	}
	else if (!ScriptParse(arg))
		I_Error("-usbtest: cannot parse the script near step %d", (int)ntest + 1);
	for (i = 1; i < ntest; i++) // stable insertion sort by poll
	{
		const tstep_t key = test[i];

		for (j = i - 1; j >= 0 && test[j].poll > key.poll; j--)
			test[j + 1] = test[j];
		test[j + 1] = key;
	}
	nmice = 1; // a replayed mouse is plugged in from the start (C lines change that)
	CONS_Printf("PS2 mouse: -usbtest with %d steps\n", (int)ntest);
}

// the reports due at this poll, summed (diff mode): motion and wheel added, the button bits of both devices OR-ed (the real driver merges them)
static boolean ScriptRead(mdata_t *m)
{
	boolean any = false;

	m->x = m->y = m->wheel = 0;
	// the script clock: poll numbers count from the start condition of the "S" line (default: from the first poll)
	if (!tstart)
	{
		if (tcond == 1 && gamestate != GS_LEVEL)
			return false;
		if (tcond == 2 && !menuactive)
			return false;
		tstart = polls;
	}
	while (testpos < ntest && test[testpos].poll <= polls - tstart + 1)
	{
		const tstep_t *s = &test[testpos++];

		if (s->dev < 0)
		{
			nmice = s->count;
			if (!nmice)
				tbtn[0] = tbtn[1] = 0; // an unplugged mouse forgets what it held
			continue;
		}
		m->x += s->x;
		m->y += s->y;
		m->wheel += s->w;
		tbtn[(INT32)s->dev] = s->b;
		any = true;
	}
	m->buttons = tbtn[0] | tbtn[1];
	if (testpos >= ntest && !tdone)
	{
		tdone = true;
		CONS_Printf("PS2 mouse: -usbtest script done at poll %u (script poll %u)\n", (unsigned)polls, (unsigned)(polls - tstart + 1));
	}
	return any;
}

// ---------------------------------------------------------------------------------------------
// Driver
// ---------------------------------------------------------------------------------------------

static void Command_PS2Mouse_f(void)
{
	CONS_Printf("PS2 mouse: usb mask %u, driver %s, %d mice, polls %u, reads %u, events posted %u, use_mouse %d/%d%s\n", PS2USB_Modules(),
		drv_state > 0 ? "open" : "none", (int)nmice, (unsigned)polls, (unsigned)reads, (unsigned)posted_ev, (int)cv_usemouse.value,
		(int)cv_usemouse2.value, test ? ", -usbtest" : "");
	CONS_Printf("PS2 mouse: poll avg %u EE cycles over %u polls, read avg %u cycles over %u reads, held %02X/%02X\n",
		polcalls ? (unsigned)(polcycles / polcalls) : 0u, (unsigned)polcalls, reads ? (unsigned)(readcycles / reads) : 0u, (unsigned)reads,
		(unsigned)held[0], (unsigned)held[1]);
}

static void DriverInit(void)
{
	drv_state = -1;
	mouse_logging = M_CheckParm("-mouselog") != 0;
	ScriptInit();
	COM_AddCommand("ps2mouse", Command_PS2Mouse_f, COM_LOCAL);
	if (M_CheckParm("-nomouse") || M_CheckParm("-nousb") || !(PS2USB_Modules() & PS2USB_MOUSE))
	{
		CONS_Printf("PS2 mouse: no driver (usb mask %u)%s\n", PS2USB_Modules(), test ? ", script only" : "");
		return;
	}
	if (PS2MouseInit() < 1)
	{
		CONS_Printf("PS2 mouse: PS2MouseInit failed\n");
		return;
	}
	PS2MouseSetReadMode(PS2MOUSE_READMODE_DIFF);
	PS2MouseSetThres(0); // no acceleration: the engine has its own sensitivity (mousesens)
	PS2MouseSetAccel(1.0f);
	drv_state = 1;
	CONS_Printf("PS2 mouse: ps2mouse driver open, diff mode\n");
}

static UINT32 Cycles(void)
{
	UINT32 v;

	__asm__ volatile("mfc0 %0,$9" : "=r"(v));
	return v;
}

void PS2Mouse_Startup(INT32 player)
{
	(void)player;
	// a mouse that is switched off lets go of what it holds; nothing else is needed (the driver is opened by the first poll that wants it)
	if (!Wants(0) && !Wants(1))
		ReleaseAll(0), ReleaseAll(1);
	else
	{
		const INT32 t = Target();

		if (t != 0)
			ReleaseAll(0);
		if (t != 1)
			ReleaseAll(1);
	}
}

void PS2Mouse_Poll(void)
{
	const UINT32 c0 = Cycles();
	INT32 target;
	mdata_t m = {0, 0, 0, 0};

	polls++;
	ClearWheel();
	if (!drv_state)
		DriverInit();
	if (drv_state <= 0 && !test)
		goto out;
	target = Target();
	if (target < 0)
	{
		ReleaseAll(0);
		ReleaseAll(1);
		goto out;
	}

	if (test)
		ScriptRead(&m);
	else if (polls >= nextcount)
	{
		// hot-plug: count the mice once a second
		nextcount = polls + COUNT_PERIOD;
		nmice = (INT32)PS2MouseEnum();
	}
	if (nmice != wasmice)
	{
		CONS_Printf("PS2 mouse: %d mice plugged in\n", (int)nmice);
		if (nmice > 0 && wasmice <= 0 && !test)
		{
			PS2MouseData flush;

			PS2MouseRead(&flush); // what piled up before the first poll is not the player's motion
		}
		else if (nmice <= 0)
			ReleaseAll(target);
		wasmice = nmice;
	}
	if (nmice > 0)
	{
		if (!test)
		{
			PS2MouseData d;
			const UINT32 r0 = Cycles();

			if (PS2MouseRead(&d) < 0)
			{
				nextcount = polls; // the driver does not answer: count again at the next poll
				goto out;
			}
			m.x = d.x;
			m.y = d.y;
			m.wheel = d.wheel;
			m.buttons = d.buttons;
			readcycles += Cycles() - r0;
			reads++;
		}
		if (mouse_logging && (m.x || m.y || m.wheel || (m.buttons & 7) != held[target]))
			CONS_Printf("PS2 mouse: poll %u dx %d dy %d wheel %d buttons %X\n", (unsigned)polls, (int)m.x, (int)m.y, (int)m.wheel, (unsigned)m.buttons);
		Apply(target, &m);
	}

out:
	if (mouse_logging && (nmice > 0 || test))
	{
		// what the engine did with the events of the previous polls: printed when it changed (z: a "wheel 1 up" bound to jump shows up there)
		static angle_t la, la2;
		static INT32 ai, ai2, lz;
		static UINT8 k[7];
		const UINT8 now[7] = {gamekeydown[KEY_MOUSE1], gamekeydown[KEY_MOUSE1 + 1], gamekeydown[KEY_MOUSE1 + 2], gamekeydown[KEY_MOUSEWHEELUP],
			gamekeydown[KEY_MOUSEWHEELDOWN], gamekeydown[KEY_2MOUSE1], gamekeydown[KEY_2MOUSEWHEELUP]};
		const INT32 z = (gamestate == GS_LEVEL && players[consoleplayer].mo) ? (INT32)(players[consoleplayer].mo->z >> FRACBITS) : 0;

		if (la != localangle || la2 != localangle2 || ai != localaiming || ai2 != localaiming2 || lz != z || memcmp(k, now, sizeof k))
		{
			la = localangle, la2 = localangle2, ai = localaiming, ai2 = localaiming2, lz = z;
			memcpy(k, now, sizeof k);
			CONS_Printf("PS2 mouse: state poll %u angle %u aiming %d angle2 %u aiming2 %d z %d keys L%d R%d M%d up%d dn%d 2L%d 2up%d\n", (unsigned)polls,
				(unsigned)localangle, (int)localaiming, (unsigned)localangle2, (int)localaiming2, (int)z, now[0], now[1], now[2], now[3], now[4], now[5], now[6]);
		}
	}
	polcycles += Cycles() - c0;
	polcalls++;
	if (mouse_logging && !(polls % 350)) // every 10 s of game time: what the polling costs the game thread
		CONS_Printf("PS2 mouse: cost poll avg %u EE cycles over %u polls, read avg %u cycles over %u reads, %d mice\n", (unsigned)(polcycles / polcalls),
			(unsigned)polcalls, reads ? (unsigned)(readcycles / reads) : 0u, (unsigned)reads, (int)nmice);
}

void PS2Mouse_Shutdown(void)
{
	ReleaseAll(0);
	ReleaseAll(1);
}

void PS2Mouse_GetCursor(INT32 *x, INT32 *y)
{
	if (curx < 0)
	{
		curx = vid.width / 2;
		cury = vid.height / 2;
	}
	if (x)
		*x = curx;
	if (y)
		*y = cury;
}

unsigned PS2Mouse_Count(void)
{
	return nmice > 0 ? (unsigned)nmice : 0;
}

unsigned PS2Mouse_EventCount(void)
{
	return posted_ev;
}
