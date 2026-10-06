// PS2-151 (OPT9-K): USB keyboard, see ps2_kbd.h.
// ps2kbd.irx (ps2sdk usbkbd) is read in its raw mode: a stream of {state 0xF0 up / 0xF1 down, USB HID usage} pairs, one per key change, no layout
// and no auto-repeat or key state kept by the driver. The usage codes are the SDL scancodes, so the key table is the one of src/sdl/i_video.c
// (Impl_SDL_Scancode_To_Keycode); the text of a key is made here (US layout, shift / caps lock / num lock) because that is what SDL_TEXTINPUT gave
// the engine: console, chat, the string fields of the menus and the "connect to" field take ev_text, not the key events.
// -kbdscript "10:+lshift,11:+a,12:-a,13:-lshift" (poll counts, key names below) feeds the same conversion without a keyboard: tests in PCSX2.
#include <kernel.h>
#include <libkbd.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../doomdef.h"
#include "../doomstat.h"
#include "../d_main.h"
#include "../d_event.h"
#include "../command.h"
#include "../keys.h"
#include "../i_system.h"
#include "../m_argv.h"

#include "ps2_usb.h"
#include "ps2_kbd.h"
#include "ps2_kbd_map.h"

#define REPEAT_DELAY_MS    450 // like the Windows default the SDL build ran on: 0.5 s, then ~30 per second
#define REPEAT_INTERVAL_MS 33
#define MAX_RAW_PER_POLL   32
#define MAX_REPEATS        4 // per poll: a stalled frame must not type a screenful
#define READ_GAP_UNSEEN_MS 200 // driver read period until the first event ever
#define READ_GAP_IDLE_MS   50  // ... and after IDLE_AFTER_S seconds without events and without a held key
#define IDLE_AFTER_S       5

static INT32 kbd_state;        // 0 = not tried, 1 = reading the driver, -1 = no driver (script only)
static boolean kbd_logging;    // -kbdlog: every raw event to the log
static UINT8 down[256];        // HID usage -> held
static boolean caps, numlock = true, scrolllock;
static boolean leds_dirty = true;
static INT32 rep_key;          // usage that repeats (the last one pressed), 0 = none
static UINT64 rep_next;
static UINT32 raw_total, polls, reads;
static INT32 held_count;       // keys (and modifiers) held right now
static UINT64 last_event, last_read; // I_GetPreciseTime of the last raw event / the last read of the driver
static UINT32 posted_keys, posted_text;

// ---------------------------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------------------------

static void Post(evtype_t type, INT32 key, boolean repeated)
{
	event_t e;

	e.type = type;
	e.key = key;
	e.x = e.y = 0;
	e.repeated = repeated;
	D_PostEvent(&e);
}

static boolean Held(UINT8 usage)
{
	return down[usage] != 0;
}

// Key down (first press, or a repeat): the key event, then the text event when text input is on at this moment (SDL: the text of a key press only
// reaches the engine when SDL_StartTextInput was active; the key event that switches it on does not type).
static void PostDown(UINT8 u, boolean repeated)
{
	const INT32 key = PS2Kbd_UsageToKey(u);

	if (key)
	{
		Post(ev_keydown, key, repeated);
		posted_keys++;
	}
	if (I_GetTextInputMode() && !Held(HID_LCTRL) && !Held(HID_RCTRL) && !Held(HID_LALT) && !Held(HID_RALT) && !Held(HID_LGUI) && !Held(HID_RGUI))
	{
		const char ch = PS2Kbd_UsageToText(u, Held(HID_LSHIFT) || Held(HID_RSHIFT), caps, numlock);

		if (ch)
		{
			Post(ev_text, (UINT8)ch, repeated);
			posted_text++;
		}
	}
}

static void PostUp(UINT8 u)
{
	const INT32 key = PS2Kbd_UsageToKey(u);

	if (key)
	{
		Post(ev_keyup, key, false);
		posted_keys++;
	}
}

static boolean IsModifier(UINT8 u)
{
	return u >= HID_LCTRL && u <= HID_RGUI;
}

static void RawEvent(boolean isdown, UINT8 u)
{
	if (u < 4) // 0 = no key, 1..3 = roll-over / POST error codes of a boot report
		return;
	raw_total++;
	last_event = I_GetPreciseTime();
	if (kbd_logging)
		CONS_Printf("PS2 kbd: raw %s usage 0x%02X\n", isdown ? "down" : "up", (unsigned)u);

	if (isdown)
	{
		if (Held(u))
		{
			// a second "down" for a held key: the driver does not repeat in raw mode (measured), so its "up" was lost, i.e. the keyboard was
			// pulled out and plugged in again while the key was held: let go of the old press and treat this one as a new press
			if (IsModifier(u))
				return;
			down[u] = 0;
			held_count--;
			if (rep_key == u)
				rep_key = 0;
			PostUp(u);
		}
		down[u] = 1;
		held_count++;
		if (u == HID_CAPS)
		{
			caps = !caps;
			leds_dirty = true;
		}
		else if (u == HID_NUMLK)
		{
			numlock = !numlock;
			leds_dirty = true;
		}
		else if (u == HID_SCROLL)
		{
			scrolllock = !scrolllock;
			leds_dirty = true;
		}
		if (!IsModifier(u) && u != HID_CAPS && u != HID_NUMLK && u != HID_SCROLL)
		{
			rep_key = u;
			rep_next = I_GetPreciseTime() + I_GetPrecisePrecision() * REPEAT_DELAY_MS / 1000;
		}
		PostDown(u, false);
	}
	else if (Held(u))
	{
		down[u] = 0;
		held_count--;
		if (rep_key == u)
			rep_key = 0;
		PostUp(u);
	}
}

static void RunRepeat(void)
{
	INT32 n;

	if (!rep_key || !Held((UINT8)rep_key))
		return;
	for (n = 0; n < MAX_REPEATS && (INT64)(I_GetPreciseTime() - rep_next) >= 0; n++)
	{
		PostDown((UINT8)rep_key, true);
		rep_next += I_GetPrecisePrecision() * REPEAT_INTERVAL_MS / 1000;
	}
	if ((INT64)(I_GetPreciseTime() - rep_next) > 0)
		rep_next = I_GetPreciseTime() + I_GetPrecisePrecision() * REPEAT_INTERVAL_MS / 1000; // a stall: do not owe the repeats
}

static void UpdateModifiers(void)
{
	shiftdown = (UINT8)((Held(HID_LSHIFT) ? 1 : 0) | (Held(HID_RSHIFT) ? 2 : 0));
	ctrldown = (UINT8)((Held(HID_LCTRL) ? 1 : 0) | (Held(HID_RCTRL) ? 2 : 0));
	altdown = (UINT8)((Held(HID_LALT) ? 1 : 0) | (Held(HID_RALT) ? 2 : 0));
	capslock = caps;
}

// ---------------------------------------------------------------------------------------------
// -kbdscript
// ---------------------------------------------------------------------------------------------

#define SCRIPT_MAX 1024
typedef struct
{
	UINT32 poll;
	UINT8 usage;
	UINT8 down;
} kbdstep_t;
static kbdstep_t *script;
static INT32 nscript, scriptpos;

static INT32 NameToUsage(const char *name)
{
	static const struct { const char *name; UINT8 usage; } tab[] = {
		{"enter", 0x28}, {"esc", 0x29}, {"bksp", 0x2A}, {"tab", 0x2B}, {"space", 0x2C}, {"minus", 0x2D}, {"equals", 0x2E}, {"lbracket", 0x2F},
		{"rbracket", 0x30}, {"backslash", 0x31}, {"hash", 0x32}, {"semicolon", 0x33}, {"quote", 0x34}, {"grave", 0x35}, {"comma", 0x36},
		{"period", 0x37}, {"slash", 0x38}, {"caps", 0x39}, {"printscreen", 0x46}, {"scrolllock", 0x47}, {"pause", 0x48}, {"ins", 0x49},
		{"home", 0x4A}, {"pgup", 0x4B}, {"del", 0x4C}, {"end", 0x4D}, {"pgdn", 0x4E}, {"right", 0x4F}, {"left", 0x50}, {"down", 0x51},
		{"up", 0x52}, {"numlock", 0x53}, {"kpdiv", 0x54}, {"kpmul", 0x55}, {"kpminus", 0x56}, {"kpplus", 0x57}, {"kpenter", 0x58},
		{"kp1", 0x59}, {"kp2", 0x5A}, {"kp3", 0x5B}, {"kp4", 0x5C}, {"kp5", 0x5D}, {"kp6", 0x5E}, {"kp7", 0x5F}, {"kp8", 0x60}, {"kp9", 0x61},
		{"kp0", 0x62}, {"kpdot", 0x63}, {"nonusbs", 0x64}, {"menu", 0x65},
		{"lctrl", HID_LCTRL}, {"lshift", HID_LSHIFT}, {"lalt", HID_LALT}, {"lgui", HID_LGUI},
		{"rctrl", HID_RCTRL}, {"rshift", HID_RSHIFT}, {"ralt", HID_RALT}, {"rgui", HID_RGUI}};
	INT32 i;

	if (name[0] >= 'a' && name[0] <= 'z' && !name[1])
		return name[0] - 'a' + 0x04;
	if (name[0] >= '1' && name[0] <= '9' && !name[1])
		return name[0] - '1' + 0x1E;
	if (name[0] == '0' && !name[1])
		return 0x27;
	if (name[0] == 'f' && name[1] >= '1' && name[1] <= '9')
	{
		const INT32 n = atoi(name + 1);

		if (n >= 1 && n <= 12)
			return n <= 10 ? 0x3A + n - 1 : 0x44 + n - 11;
	}
	for (i = 0; i < (INT32)(sizeof tab / sizeof tab[0]); i++)
		if (!strcmp(name, tab[i].name))
			return tab[i].usage;
	if (name[0] == 'h' && name[1]) // h28 = raw usage in hex (only when every character is a hex digit: "home", "hash" are names)
	{
		char *end;
		const long v = strtol(name + 1, &end, 16);

		if (!*end && v > 0 && v < 256)
			return (INT32)v;
	}
	return -1;
}

static void AddStep(UINT32 poll, INT32 usage, boolean isdown)
{
	script[nscript].poll = poll;
	script[nscript].usage = (UINT8)usage;
	script[nscript].down = isdown;
	nscript++;
}

// "poll:+name" press, "poll:-name" release, "poll:name" tap (press now, release at the next poll)
static boolean ScriptParse(const char *spec)
{
	const char *p = spec;

	while (*p && nscript + 2 <= SCRIPT_MAX)
	{
		UINT32 poll = 0;
		char name[16];
		size_t len = 0;
		INT32 usage;
		char sign = 0;

		while (*p == ',' || *p == ' ' || *p == '\n' || *p == '\r')
			p++;
		if (!*p)
			break;
		while (*p >= '0' && *p <= '9')
			poll = poll * 10 + (UINT32)(*p++ - '0');
		if (*p++ != ':')
			return false;
		if (*p == '+' || *p == '-')
			sign = *p++;
		while (*p && *p != ',' && *p != ' ' && *p != '\n' && *p != '\r' && len < sizeof name - 1)
			name[len++] = *p++;
		name[len] = 0;
		usage = NameToUsage(name);
		if (usage < 0)
			return false;
		if (sign)
			AddStep(poll, usage, sign == '+');
		else
		{
			AddStep(poll, usage, true);
			AddStep(poll + 1, usage, false);
		}
	}
	return true;
}

static void ScriptInit(void)
{
	const char *arg;

	if (!M_CheckParm("-kbdscript") || !M_IsNextParm())
		return;
	arg = M_GetNextParm();
	script = calloc(SCRIPT_MAX, sizeof *script);
	if (!script)
		I_Error("-kbdscript: out of memory");
	if (!strncmp(arg, "file:", 5))
	{
		char path[256], *buf = malloc(32768);
		FILE *f;
		size_t got;

		if (!buf)
			I_Error("-kbdscript: out of memory");
		snprintf(path, sizeof path, "%s/%s", I_GetEnv("HOME") ? I_GetEnv("HOME") : ".", arg + 5);
		f = fopen(path, "rb");
		if (!f)
			I_Error("-kbdscript: cannot open %s", path);
		got = fread(buf, 1, 32767, f);
		fclose(f);
		buf[got] = 0;
		if (!ScriptParse(buf))
			I_Error("-kbdscript: cannot parse the script near step %d", (int)nscript + 1);
		free(buf);
	}
	else if (!ScriptParse(arg))
		I_Error("-kbdscript: cannot parse the script near step %d", (int)nscript + 1);
	{
		// stable insertion sort by poll number
		INT32 i, j;

		for (i = 1; i < nscript; i++)
		{
			const kbdstep_t key = script[i];

			for (j = i - 1; j >= 0 && script[j].poll > key.poll; j--)
				script[j + 1] = script[j];
			script[j + 1] = key;
		}
	}
	CONS_Printf("PS2 kbd: -kbdscript with %d steps\n", (int)nscript);
}

static void ScriptRun(void)
{
	while (scriptpos < nscript && script[scriptpos].poll <= polls)
	{
		RawEvent(script[scriptpos].down, script[scriptpos].usage);
		scriptpos++;
	}
}

// ---------------------------------------------------------------------------------------------
// Driver
// ---------------------------------------------------------------------------------------------

static void Command_PS2Kbd_f(void)
{
	CONS_Printf("PS2 kbd: usb mask %u, driver %s, raw events %u, keys posted %u, text posted %u, polls %u, driver reads %u, held %d%s\n",
		PS2USB_Modules(), kbd_state > 0 ? "open" : "none", (unsigned)raw_total, (unsigned)posted_keys, (unsigned)posted_text, (unsigned)polls,
		(unsigned)reads, (int)held_count, script ? ", script" : "");
	CONS_Printf("PS2 kbd: shift %u ctrl %u alt %u caps %d num %d scroll %d, text input %s\n", (unsigned)shiftdown, (unsigned)ctrldown, (unsigned)altdown,
		(int)caps, (int)numlock, (int)scrolllock, I_GetTextInputMode() ? "on" : "off");
}

static void DriverInit(void)
{
	kbd_state = -1;
	kbd_logging = M_CheckParm("-kbdlog") != 0;
	ScriptInit();
	COM_AddCommand("ps2kbd", Command_PS2Kbd_f, COM_LOCAL);
	if (M_CheckParm("-nokbd") || M_CheckParm("-nousb") || !(PS2USB_Modules() & PS2USB_KBD))
	{
		CONS_Printf("PS2 kbd: no driver (usb mask %u)%s\n", PS2USB_Modules(), script ? ", script only" : "");
		return;
	}
	if (PS2KbdInit() < 1)
	{
		CONS_Printf("PS2 kbd: PS2KbdInit failed\n");
		return;
	}
	PS2KbdSetReadmode(PS2KBD_READMODE_RAW);
	PS2KbdSetBlockingMode(PS2KBD_NONBLOCKING); // read() returns 0 when nothing happened: the poll never waits for a key
	PS2KbdFlushBuffer();
	kbd_state = 1;
	CONS_Printf("PS2 kbd: usbkbd driver open, raw mode\n");
}

// Each read is a synchronous fileXio RPC to the IOP (~27 k EE cycles measured in PCSX2, PS2-151), so a keyboard that nobody uses must not be read at the
// tic rate: until the driver has delivered its first event (no keyboard, or one nobody has touched) it is read every 200 ms (the IOP queues the
// events meanwhile, nothing is lost: the first key press shows up at most 0.2 s late), and a keyboard that has been quiet for 5 s with no key held
// every 50 ms. While keys are held or typing goes on it is read on every poll.
static boolean ReadDue(void)
{
	const UINT64 now = I_GetPreciseTime(), prec = I_GetPrecisePrecision();
	UINT64 gap = 0;

	if (!raw_total)
		gap = prec * READ_GAP_UNSEEN_MS / 1000;
	else if (held_count <= 0 && now - last_event > prec * IDLE_AFTER_S)
		gap = prec * READ_GAP_IDLE_MS / 1000;
	if (gap && now - last_read < gap)
		return false;
	last_read = now;
	reads++;
	return true;
}

static void ReadDriver(void)
{
	PS2KbdRawKey rk;
	INT32 n;

	if (!ReadDue())
		return;
	for (n = 0; n < MAX_RAW_PER_POLL; n++)
	{
		if (PS2KbdReadRaw(&rk) < 1)
			break;
		if (rk.state == PS2KBD_RAWKEY_DOWN)
			RawEvent(true, rk.key);
		else if (rk.state == PS2KBD_RAWKEY_UP)
			RawEvent(false, rk.key);
	}
}

static void UpdateLeds(void)
{
	if (!leds_dirty || kbd_state <= 0)
		return;
	leds_dirty = false;
	PS2KbdSetLeds((UINT8)((numlock ? PS2KBD_LED_NUMLOCK : 0) | (caps ? PS2KBD_LED_CAPSLOCK : 0) | (scrolllock ? PS2KBD_LED_SCRLOCK : 0)));
}

void PS2Kbd_Poll(void)
{
	if (!kbd_state)
		DriverInit();
	polls++;
	if (kbd_logging && polls % 100 == 0)
		CONS_Printf("PS2 kbd: poll %u\n", (unsigned)polls); // -kbdlog: where a -kbdscript step number falls in time
	if (script)
		ScriptRun();
	if (kbd_state > 0)
		ReadDriver();
	RunRepeat();
	UpdateLeds();
	UpdateModifiers();
	keyboard_started = (kbd_state > 0 || script) ? 1 : 0;
}

void PS2Kbd_Shutdown(void)
{
	INT32 u;

	for (u = 1; u < 256; u++)
		if (down[u])
		{
			down[u] = 0;
			PostUp((UINT8)u);
		}
	rep_key = 0;
	held_count = 0;
	UpdateModifiers();
	if (kbd_state > 0)
	{
		PS2KbdClose();
		kbd_state = -1;
	}
}

unsigned PS2Kbd_RawCount(void)
{
	return raw_total;
}
