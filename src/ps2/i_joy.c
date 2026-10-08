// SONIC ROBO BLAST 2 - PS2 gamepads: DualShock 2 through libpad (padman.irx is loaded by ps2_boot.c).
// Joystick 1 = pad port 1, joystick 2 = pad port 2 (SDL numbers devices from 1 as well). Reading and the
// button/stick -> event conversion are split: libpad here, the mapping in ps2_padmap.c.
#include <kernel.h>
#include <delaythread.h>
#include <timer.h>
#include <libpad.h>

#include <stdio.h>
#include <string.h>

#include "../doomdef.h"
#include "../doomstat.h"
#include "../d_main.h"
#include "../d_event.h"
#include "../g_input.h"
#include "../i_joy.h"
#include "../i_system.h"
#include "../m_argv.h"
#include "../netcode/d_netcmd.h"

#include "ps2_boot.h"
#include "ps2_padmap.h"
#include "ps2_sys.h"

#define NUMPORTS 2

// one entry per physical pad port
typedef struct
{
	boolean configured;     // analog mode negotiated for the current connection
	boolean analog;         // DualShock in analog mode: stick bytes are valid
	INT32 modetries;
	boolean actuators;      // vibration motors aligned
} padport_t;

// one entry per engine joystick (1 and 2)
typedef struct
{
	boolean started;
	INT32 port;
	boolean connected;      // events are flowing
	INT32 scale;
	ps2pad_state_t map;
} joyslot_t;

static char padbuf[NUMPORTS][256] __attribute__((aligned(64))); // libpad DMA area, 64-byte aligned
static padport_t ports[NUMPORTS];
static joyslot_t slots[2] = {{false, 0, false, 1, {0, 0, {0}}}, {false, 1, false, 1, {0, 0, {0}}}};
static INT32 padlib; // 0 not tried, 1 up, -1 failed

static UINT64 rumble_end[NUMPORTS];

static void ConfigurePort(INT32 p);

// a controller is there (settling commands such as the analog-mode request show up as EXECCMD)
static boolean PortPresent(INT32 p)
{
	const INT32 st = padGetState(p, 0);
	return st == PAD_STATE_STABLE || st == PAD_STATE_EXECCMD || st == PAD_STATE_FINDCTP1;
}

static UINT32 pad_t0;     // PS2-LOAD-3: COP0 Count when the ports were opened
static boolean padsettled; // the detection wait and the analog-mode negotiation of PadLib() have run

static inline UINT32 PadCount(void)
{
	UINT32 v;

	__asm__ volatile("mfc0 %0,$9" : "=r"(v));
	return v;
}

// PS2-LOAD-3: libpad is opened right at the start of main() (PS2Joy_Prewarm), the detection of the controllers (up to 1.3 s with no pad plugged in, a few
// hundred ms with one) then runs while the engine initialises; PadLib() waits only for what is left of the same limits (1000 ms for port 1, 1300 ms in all for
// port 2, counted from the moment the ports were opened). The first PadLib() call used to open the ports and wait the whole time with nothing else going on.
static boolean PadOpen(void)
{
	INT32 p;

	if (padlib)
		return padlib > 0;
	padlib = -1;
	if (ps2boot.sio2man <= 0 || ps2boot.padman <= 0)
	{
		CONS_Printf("PS2 pad: sio2man/padman are not loaded\n");
		return false;
	}
	if (padInit(0) != 1)
	{
		CONS_Printf("PS2 pad: padInit failed\n");
		return false;
	}
	for (p = 0; p < NUMPORTS; p++)
		if (padPortOpen(p, 0, padbuf[p]) != 1)
		{
			CONS_Printf("PS2 pad: padPortOpen(%d) failed\n", (int)p);
			return false;
		}
	padlib = 1;
	pad_t0 = PadCount();
	return true;
}

void PS2Joy_Prewarm(void)
{
	if (!M_CheckParm("-nopadprewarm"))
		PadOpen();
}

static boolean PadLib(void)
{
	INT32 p, ms;

	if (!PadOpen())
		return false;
	if (padsettled)
		return true;
	padsettled = true;

	// detection takes a few hundred milliseconds; port 1 is probed alongside, port 2 gets a short grace
	ms = (INT32)((UINT32)(PadCount() - pad_t0) / 294912u) / 10 * 10; // time already spent since padPortOpen
	if (ms > 1300)
		ms = 1300;
	for (; ms < 1000 && padGetState(0, 0) != PAD_STATE_STABLE; ms += 10)
		DelayThread(10000);
	for (; ms < 1300 && padGetState(1, 0) != PAD_STATE_STABLE; ms += 10)
		DelayThread(10000);
	CONS_Printf("PS2 pad: ports %d/%d (state 6 = ready) after %d ms\n", padGetState(0, 0), padGetState(1, 0), (int)ms);

	// switch whatever is plugged in to analog mode now, so the pad names and the first read are right
	for (p = 0; p < NUMPORTS; p++)
	{
		for (ms = 0; ms < 500 && PortPresent(p) && !ports[p].configured; ms += 10)
		{
			if (padGetState(p, 0) == PAD_STATE_STABLE)
				ConfigurePort(p);
			if (!ports[p].configured)
				DelayThread(10000);
		}
		// the last command (analog mode, motor alignment) is still executing: wait for state 6
		for (ms = 0; ms < 500 && PortPresent(p) && padGetState(p, 0) != PAD_STATE_STABLE; ms += 10)
			DelayThread(10000);
	}
	return true;
}

// DualShock in analog mode, locked (the stick bytes of a digital pad are meaningless)
static void ConfigurePort(INT32 p)
{
	padport_t *pp = &ports[p];
	const INT32 id = padInfoMode(p, 0, PAD_MODECURID, 0);

	if (id == PAD_TYPE_DUALSHOCK)
	{
		static const char align[6] = {0, 1, 0xff, 0xff, 0xff, 0xff}; // motor 0 = small (on/off), 1 = large
		pp->analog = true;
		pp->configured = true;
		pp->actuators = padInfoAct(p, 0, -1, 0) > 0 && padSetActAlign(p, 0, align) != 0;
		return;
	}

	if (pp->modetries < 3)
	{
		INT32 i, n = padInfoMode(p, 0, PAD_MODETABLE, -1);
		for (i = 0; i < n; i++)
			if (padInfoMode(p, 0, PAD_MODETABLE, i) == PAD_TYPE_DUALSHOCK)
			{
				pp->modetries++;
				padSetMainMode(p, 0, PAD_MMODE_DUALSHOCK, PAD_MMODE_LOCK); // async: stable again in a moment
				return;
			}
	}
	pp->analog = false;
	pp->configured = true;
}

// PS2-136: -padscript "10:1:+cross,25:1:-cross,40:2:lx=255,..." drives the pads without a human (tests in PCSX2, where no pad can be pressed from
// outside): the number is the count of PS2Joy_Poll calls (one per displayed frame), the next field is the pad port (1 or 2), then +button / -button
// (up down left right cross circle square triangle l1 r1 l2 r2 start select l3 r3) or axis=value (lx ly rx ry, 0..255, 128 = centre). The raw
// state goes through the same conversion as a real pad (ps2_padmap.c), so the whole path to the engine events is the one that is shipped.
// -padscript file:NAME reads the list from <HOME>/NAME. A port with a script is "plugged in" whatever libpad says.
#define SCRIPT_MAX 12288 // PS2-141: 20 minutes of two scripted pads (tools/ps2/gen_padscript.py, a step every ~12 polls)
typedef struct
{
	UINT32 frame;
	UINT8 port;
	UINT8 axis;   // 0 = button, 1..4 = lx ly rx ry
	UINT8 down;
	UINT8 val;
	UINT16 mask;
} padstep_t;
static padstep_t *script; // allocated by ScriptInit: nothing in bss for the normal game
static INT32 nscript, scriptpos;
static UINT32 pollcount;
static boolean vactive[NUMPORTS];
static ps2pad_raw_t vraw[NUMPORTS];

static boolean ScriptParse(const char *spec)
{
	static const struct { const char *name; UINT16 mask; } btn[] = {
		{"up", PS2PAD_UP}, {"down", PS2PAD_DOWN}, {"left", PS2PAD_LEFT}, {"right", PS2PAD_RIGHT}, {"cross", PS2PAD_CROSS}, {"circle", PS2PAD_CIRCLE},
		{"square", PS2PAD_SQUARE}, {"triangle", PS2PAD_TRIANGLE}, {"l1", PS2PAD_L1}, {"r1", PS2PAD_R1}, {"l2", PS2PAD_L2}, {"r2", PS2PAD_R2},
		{"start", PS2PAD_START}, {"select", PS2PAD_SELECT}, {"l3", PS2PAD_L3}, {"r3", PS2PAD_R3}};
	static const char *const axes[4] = {"lx", "ly", "rx", "ry"};
	const char *p = spec;

	while (*p && nscript < SCRIPT_MAX)
	{
		padstep_t s;
		char name[16];
		size_t len = 0;
		INT32 i;

		memset(&s, 0, sizeof s);
		while (*p == ',' || *p == ' ' || *p == '\n' || *p == '\r')
			p++;
		if (!*p)
			break;
		while (*p >= '0' && *p <= '9')
			s.frame = s.frame * 10 + (UINT32)(*p++ - '0');
		if (*p++ != ':')
			return false;
		s.port = (UINT8)(*p++ - '0');
		if (s.port < 1 || s.port > NUMPORTS || *p++ != ':')
			return false;
		s.down = 1;
		if (*p == '+' || *p == '-')
			s.down = (*p++ == '+');
		while (*p && *p != ',' && *p != '=' && *p != '\n' && *p != '\r' && len < sizeof name - 1)
			name[len++] = *p++;
		name[len] = 0;
		if (*p == '=')
		{
			INT32 v = 0;

			p++;
			while (*p >= '0' && *p <= '9')
				v = v * 10 + (*p++ - '0');
			for (i = 0; i < 4; i++)
				if (!strcmp(name, axes[i]))
					s.axis = (UINT8)(i + 1);
			if (!s.axis)
				return false;
			s.val = (UINT8)(v > 255 ? 255 : v);
		}
		else
		{
			for (i = 0; i < (INT32)(sizeof btn / sizeof btn[0]); i++)
				if (!strcmp(name, btn[i].name))
					s.mask = btn[i].mask;
			if (!s.mask)
				return false;
		}
		s.port--;
		script[nscript++] = s;
		vactive[s.port] = true;
	}
	return true;
}

static void ScriptInit(void)
{
	static boolean done;
	INT32 p;

	if (done)
		return;
	done = true;
	if (M_CheckParm("-padscript") && M_IsNextParm())
	{
		const char *arg = M_GetNextParm();
		const size_t bufsize = 262144;
		char *buf = malloc(bufsize);

		script = calloc(SCRIPT_MAX, sizeof *script);
		if (!buf || !script)
			I_Error("-padscript: out of memory");

		if (!strncmp(arg, "file:", 5))
		{
			char path[256];
			FILE *f;
			size_t got = 0;

			snprintf(path, sizeof path, "%s/%s", I_GetEnv("HOME") ? I_GetEnv("HOME") : ".", arg + 5);
			f = fopen(path, "rb");
			if (!f)
				I_Error("-padscript: cannot open %s", path);
			got = fread(buf, 1, bufsize - 1, f);
			fclose(f);
			buf[got] = 0;
			arg = buf;
		}
		if (!ScriptParse(arg))
			I_Error("-padscript: cannot parse the script near step %d", (int)nscript + 1);
		{
			// stable insertion sort by poll number: the lists of the two pads may be written one after the other
			INT32 i, j;

			for (i = 1; i < nscript; i++)
			{
				const padstep_t key = script[i];

				for (j = i - 1; j >= 0 && script[j].frame > key.frame; j--)
					script[j + 1] = script[j];
				script[j + 1] = key;
			}
		}
		for (p = 0; p < NUMPORTS; p++)
		{
			vraw[p].lx = vraw[p].ly = vraw[p].rx = vraw[p].ry = 128;
			vraw[p].analog = 1;
		}
		CONS_Printf("PS2 pad: -padscript with %d steps\n", (int)nscript);
		free(buf);
	}
}

static void ScriptRun(void)
{
	pollcount++;
	while (scriptpos < nscript && script[scriptpos].frame <= pollcount)
	{
		const padstep_t *s = &script[scriptpos++];
		ps2pad_raw_t *r = &vraw[s->port];

		if (s->axis)
			(&r->lx)[s->axis - 1] = s->val;
		else if (s->down)
			r->buttons |= s->mask;
		else
			r->buttons &= (UINT16)~s->mask;
	}
}

// false: no usable pad on the port right now
static boolean ReadPort(INT32 p, ps2pad_raw_t *raw)
{
	if (vactive[p])
	{
		*raw = vraw[p];
		return true;
	}
	static struct padButtonStatus bs __attribute__((aligned(64)));
	padport_t *pp = &ports[p];

	if (padGetState(p, 0) != PAD_STATE_STABLE)
	{
		pp->configured = false;
		pp->modetries = 0;
		return false;
	}
	if (!pp->configured)
	{
		ConfigurePort(p);
		if (!pp->configured)
			return false;
	}
	if (padRead(p, 0, &bs) == 0)
		return false;

	raw->buttons = (UINT16)~bs.btns; // libpad: 0 = pressed
	raw->lx = bs.ljoy_h;
	raw->ly = bs.ljoy_v;
	raw->rx = bs.rjoy_h;
	raw->ry = bs.rjoy_v;
	raw->analog = pp->analog && ((bs.mode >> 4) == 7 || (bs.mode >> 4) == 5);
	return true;
}

static void PostEvents(INT32 slot, const ps2pad_event_t *ev, INT32 n)
{
	INT32 i;

	for (i = 0; i < n; i++)
	{
		event_t e;
		e.x = e.y = 0;
		e.repeated = false;
		switch (ev[i].kind)
		{
			case PS2PADEV_KEY:
				e.type = ev[i].down ? ev_keydown : ev_keyup;
				e.key = (slot ? KEY_2JOY1 : KEY_JOY1) + ev[i].index;
				break;
			case PS2PADEV_HAT:
				e.type = ev[i].down ? ev_keydown : ev_keyup;
				e.key = (slot ? KEY_2HAT1 : KEY_HAT1) + ev[i].index;
				break;
			default:
				e.type = slot ? ev_joystick2 : ev_joystick;
				e.key = ev[i].index; // axis set
				e.x = ev[i].x; // PS2PAD_AXIS_NONE == INT32_MAX: axis not changed
				e.y = ev[i].y;
				break;
		}
		D_PostEvent(&e);
	}
}

static void PollSlot(INT32 j)
{
	joyslot_t *s = &slots[j];
	ps2pad_event_t ev[PS2PAD_MAXEVENTS];
	ps2pad_raw_t raw;
	ps2pad_cfg_t cfg;
	INT32 n;

	if (!s->started)
		return;

	if (ReadPort(s->port, &raw))
	{
		cfg.gamepadstyle = (j ? Joystick2 : Joystick).bGamepadStyle;
		cfg.scale = s->scale;
		n = PS2Pad_Update(&s->map, &raw, &cfg, ev, PS2PAD_MAXEVENTS);
		s->connected = true;
	}
	else
	{
		// pad pulled out or still negotiating: let go of everything once
		if (!s->connected)
			return;
		n = PS2Pad_Release(&s->map, ev, PS2PAD_MAXEVENTS);
		s->connected = false;
	}
	PostEvents(j, ev, n);
}

static void SetRumble(INT32 p, UINT8 small, UINT8 large)
{
	char act[6] = {small ? 1 : 0, (char)large, 0, 0, 0, 0};

	if (ports[p].actuators)
		padSetActDirect(p, 0, act);
}

void PS2Joy_Poll(void)
{
	INT32 p;

	if (padlib <= 0)
		return;
	ScriptInit();
	if (nscript)
		ScriptRun();
	if (splitscreen && !slots[1].started && !M_CheckParm("-nojoy") && (vactive[1] || PortPresent(1)))
		CV_SetValue(&cv_usejoystick2, 2); // PS2-136: the second local player plays on the pad of port 2 (InitSlot starts it)
	I_GetJoystickEvents();
	I_GetJoystick2Events();

	for (p = 0; p < NUMPORTS; p++)
		if (rumble_end[p] && (INT64)(I_GetPreciseTime() - rumble_end[p]) >= 0)
		{
			rumble_end[p] = 0;
			SetRumble(p, 0, 0);
		}
}

void I_GetJoystickEvents(void)
{
	PollSlot(0);
}

void I_GetJoystick2Events(void)
{
	PollSlot(1);
}

// neutral events for everything that is held
static void ReleaseSlot(INT32 j)
{
	ps2pad_event_t ev[PS2PAD_MAXEVENTS];

	PostEvents(j, ev, PS2Pad_Release(&slots[j].map, ev, PS2PAD_MAXEVENTS));
	slots[j].connected = false;
}

void PS2Joy_Shutdown(void)
{
	INT32 j, p;

	for (j = 0; j < 2; j++)
	{
		ReleaseSlot(j);
		slots[j].started = false;
	}
	for (p = 0; p < NUMPORTS; p++)
		if (padlib > 0 && ports[p].actuators)
			SetRumble(p, 0, 0);
}

void PS2Joy_WaitStart(void)
{
	ps2pad_raw_t raw;
	INT32 p;

	if (!PadLib())
		return;
	for (;;)
	{
		for (p = 0; p < NUMPORTS; p++)
			if (ReadPort(p, &raw) && (raw.buttons & PS2PAD_START))
				return;
		if (PS2Boot_PowerRequested())
			return;
		DelayThread(20000);
	}
}

void I_JoyScale(void)
{
	Joystick.bGamepadStyle = cv_joyscale.value==0;
	slots[0].scale = Joystick.bGamepadStyle ? 1 : cv_joyscale.value;
}

void I_JoyScale2(void)
{
	Joystick2.bGamepadStyle = cv_joyscale2.value==0;
	slots[1].scale = Joystick2.bGamepadStyle ? 1 : cv_joyscale2.value;
}

static void InitSlot(INT32 j)
{
	consvar_t *cv = j ? &cv_usejoystick2 : &cv_usejoystick;
	joyslot_t *s = &slots[j], *other = &slots[!j];
	const INT32 port = cv->value - 1;

	if (M_CheckParm("-nojoy"))
		return;

	if (!PadOpen()) // PS2-LOAD-3: the ports only have to be open; the controller detection goes on in the background (PadLib() is for who needs to know what is plugged in)
	{
		cv->value = 0;
		return;
	}

	if (cv->value && port >= 0 && port < NUMPORTS && !(other->started && other->port == port))
	{
		if (!(s->started && s->port == port))
		{
			ReleaseSlot(j);
			s->port = port;
			s->started = true;
			CONS_Printf("PS2 pad: joystick %d is port %d\n", (int)j + 1, (int)port + 1);
		}
		if (j)
			I_JoyScale2();
		else
			I_JoyScale();
		return;
	}

	// off, unknown port, or the other joystick already has it
	ReleaseSlot(j);
	s->started = false;
	if (cv->value)
		cv->value = 0;
}

void I_InitJoystick(void)
{
	InitSlot(0);
}

void I_InitJoystick2(void)
{
	InitSlot(1);
}

// highest port that has a controller: SDL numbers the attached devices from 1
INT32 I_NumJoys(void)
{
	INT32 p, n = 0;

	if (!PadLib())
		return 0;
	for (p = 0; p < NUMPORTS; p++)
		if (PortPresent(p))
			n = p + 1;
	return n;
}

const char *I_GetJoyName(INT32 joyindex)
{
	static char joyname[64];
	const char *kind = "Controller";
	INT32 p = joyindex - 1;

	joyname[0] = 0;
	if (!PadLib() || p < 0 || p >= NUMPORTS)
		return joyname;

	if (!PortPresent(p))
		snprintf(joyname, sizeof joyname, "No controller (port %d)", (int)p + 1);
	else
	{
		switch (padInfoMode(p, 0, PAD_MODECURID, 0))
		{
			case PAD_TYPE_DUALSHOCK: kind = "DualShock 2"; break;
			case PAD_TYPE_ANALOG: kind = "Analog Controller"; break;
			case PAD_TYPE_DIGITAL: kind = "Digital Controller"; break;
			default: break;
		}
		snprintf(joyname, sizeof joyname, "%s (port %d)", kind, (int)p + 1);
	}
	return joyname;
}

// Force feedback: SRB2 has no haptics (SDL's I_Tactile does nothing) and nothing calls this, but the plan maps
// it to the DualShock motors: magnitude -> large motor, small motor above half.
static void Tactile(INT32 j, FFType type, const JoyFF_t *effect)
{
	const INT32 p = slots[j].port;

	(void)type;
	if (!slots[j].started || !ports[p].actuators)
		return;
	if (!effect || effect->Magnitude <= 0)
	{
		rumble_end[p] = 0;
		SetRumble(p, 0, 0);
		return;
	}
	{
		const INT32 mag = effect->Magnitude > 10000 ? 10000 : effect->Magnitude;
		const UINT64 us = effect->Duration ? effect->Duration : 250000;
		SetRumble(p, mag > 5000, (UINT8)(mag * 255 / 10000));
		rumble_end[p] = I_GetPreciseTime() + us * 147; // 147.456 bus ticks per microsecond
	}
}

void I_Tactile(FFType Type, const JoyFF_t *Effect)
{
	Tactile(0, Type, Effect);
}

void I_Tactile2(FFType Type, const JoyFF_t *Effect)
{
	Tactile(1, Type, Effect);
}
